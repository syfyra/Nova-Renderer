#pragma once

// The GPU counterpart of the CPU passes (docs/design/M10.md D4, D8, D9): the
// device scene, the wave buffers and the device film. A pass generates the
// camera rays, then per depth finds the closest hits (OptiX), shades them,
// traces the shadow rays (OptiX) and adds their radiance, and flushes the
// samples into the film, which is copied back into the host's Film.

#include "core/film/film.h"
#include "core/film/filter.h"
#include "core/gpu/device.h"
#include "core/gpu/kernel_types.h"
#include "core/gpu/optix_scene.h"
#include "core/integrator/guide_field.h"
#include "core/integrator/settings.h"
#include "core/integrator/wavefront.h"
#include "core/scene/camera.h"
#include "core/texture/texture.h"
#include "core/texture/texture_budget.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nova {
class Scene;
}

namespace nova::gpu {

class GpuRenderer {
public:
    GpuRenderer() : m_accel(m_device) {}
    ~GpuRenderer();
    GpuRenderer(const GpuRenderer &) = delete;
    GpuRenderer &operator=(const GpuRenderer &) = delete;

    // Opens the device with the OptiX programs and the kernels; false with
    // the reason in `error`.
    bool init(std::string &error);
    const std::string &deviceName() const { return m_device.name(); }
    const std::string &lastError() const { return m_device.lastError(); }

    // Uploads the scene if its version changed: the geometry and GAS of the
    // meshes that changed, the rest anew.
    bool setScene(const Scene &scene);
    // RenderSettings::gpuTextureBudget in MB (0: the default, half the
    // memory free when the first texture loads). A change is planned again
    // at the next setScene, also of an unchanged scene; only the textures
    // whose kept levels change are read again.
    void setTextureBudget(uint32_t megabytes);
    // The texture budget in bytes (0: the default, not measured yet).
    size_t textureBudgetBytes() const { return m_textureBudget; }
    // The device film becomes a copy of `film` (its beauty and alpha sums and
    // its counts).
    bool setFilm(const Film &film);
    // The device film's sums into `film` (beauty, alpha, AO, light path
    // expression outputs).
    bool downloadFilm(Film &film);
    // `spp` samples for every pixel of `region`, accumulated in the device
    // film; `film`'s counts (and cryptomattes) follow at once, its sums with
    // downloadFilm(). Adds the rays traced to `rays`.
    // With lpe, the light path expressions' outputs as well; with a list,
    // only those film pixels (proxy and adaptive passes). lpeGeneration
    // names the tables' contents (the caller's counter, changed with every
    // rebuild) so the device copy is kept while it stays; 0 uploads them
    // with every pass.
    // With `guide` (path guiding, docs/design/M11-P9c.md) its field guides
    // the pass and, while it trains, the pass's records add to the device's
    // copy of its sums (downloadGuide brings them to the host).
    bool renderPass(const Camera &camera, const RenderSettings &settings, const PixelFilter &filter,
                    const TileRect &region, uint32_t spp, Film &film, uint64_t &rays, const LpeTables *lpe = nullptr,
                    const std::vector<uint32_t> *list = nullptr, uint64_t lpeGeneration = 0,
                    const PathGuide *guide = nullptr);
    // Adds the device's training sums of the field's current generation to
    // the host's (before the host ends an iteration).
    bool downloadGuide(PathGuide &guide);

private:
    // Copies to the device into a buffer of "into" (put: of the scene's,
    // replaced with every upload).
    template <class T> Span<T> upload(std::vector<DeviceBuffer> &into, const T *data, size_t count);
    template <class T> Span<T> put(const T *data, size_t count) { return upload(m_sceneBuffers, data, count); }
    template <class T> Span<T> put(const std::vector<T> &v) { return put(v.data(), v.size()); }
    bool reserveWave(uint32_t paths);
    bool reserveLpe(uint32_t paths);
    bool reserveMedia(uint32_t paths);
    bool reserveFake(uint32_t paths);
    bool reserveWalks(uint32_t paths);
    bool uploadLpe(const LpeTables &tables, uint64_t generation);
    // The field's arrays on the device (when its generation changed; sums
    // cleared) and the wave's records.
    bool uploadGuide(const PathGuide &guide);
    bool reserveGuide(uint32_t paths);
    bool launch(CUfunction fn, uint32_t threads, void *args);

    // Textures (D10): per TextureId of the scene's sampler its shape, the
    // levels the budget plan keeps of each tile (planned before any texel is
    // read) and its tiles' MIP chains on the device (uploaded on first use:
    // only the planned levels are read) with its texture objects (per
    // address modes and filter) and UDIM tables.
    struct Texture {
        bool ok = false;         // uploaded, with at least one tile
        bool udim = false;
        bool known = false;      // `info` holds the texture's shape
        bool uploaded = false;   // its upload was tried (per `plan`)
        uint32_t generation = 0; // the sampler's textureGeneration when its shape was taken
        TexInfo info;
        std::vector<TexTilePlan> plan; // per tile of `info` (empty: not planned)
        size_t bytes = 0;              // its chains on the device
        std::vector<std::pair<uint32_t, CUmipmappedArray>> tiles; // UDIM number (0: none), chain
        std::vector<uint32_t> levels;
        std::vector<CUtexObject> objects;
        std::vector<DeviceBuffer> tables; // UDIM sets' objects per tile
    };
    // The textures the device samples (setScene's rule) in order of first
    // use, the budget and the plan over them.
    struct TexturePlan {
        std::vector<TextureId> used;
        size_t budget = 0;
        TexBudgetPlan plan;
    };
    Texture &textureEntry(const ITextureSampler *sampler, TextureId id);
    Texture &texture(const ITextureSampler *sampler, TextureId id);
    TexturePlan planTextures(const Scene &scene);
    // Whether the device copies are what `plan` keeps (and those no longer
    // used leave room for it).
    bool texturesMatch(const TexturePlan &plan) const;
    // Drops the device copies `plan` changes (only those) and those no longer
    // used when they do not fit beside it; the plan's levels are uploaded on
    // first use.
    void applyTexturePlan(TexturePlan &plan);
    size_t textureBudget();
    DeviceTexSlot textureSlot(const ITextureSampler *sampler, const TexSampler &s);
    CUtexObject textureObject(Texture &tex, CUmipmappedArray chain, uint32_t levels, const TexSlot &slot, bool udim);
    // A texture's device copy, objects and slots (its shape stays); after a
    // device sync.
    void freeTexture(TextureId id, Texture &tex);
    // Every texture's device copy (the shapes stay).
    void freeTextureData();
    void releaseTextures();
    // Whether `sampler` (the current one) replaced the data of a texture
    // uploaded from it (ITextureSampler::textureGeneration).
    bool texturesChanged(const ITextureSampler *sampler) const;

    Device m_device;
    OptixScene m_accel;
    CUmodule m_module = nullptr;
    CUfunction m_generate = nullptr, m_shade = nullptr, m_shadow = nullptr, m_ao = nullptr, m_flush = nullptr;
    CUfunction m_media = nullptr, m_volumes = nullptr, m_fakeStart = nullptr, m_fakeStep = nullptr;
    CUfunction m_walkExit = nullptr;
    CUfunction m_guideSnap = nullptr, m_guideRecord = nullptr;

    // The scene: its buffers and their views.
    const Scene *m_scene = nullptr;
    uint64_t m_sceneVersion = ~uint64_t(0);
    std::vector<DeviceBuffer> m_sceneBuffers;
    // Per MeshId its geometry on the device (the description's arrays and the
    // attributes its materials read from them), kept while
    // MeshRecord::geometryVersion stays (IPR, D14).
    struct MeshGeometry {
        MeshGeometry() = default;
        MeshGeometry(MeshGeometry &&) = default; // no copies: the buffers move
        MeshGeometry &operator=(MeshGeometry &&) = default;
        uint64_t version = 0;
        std::vector<DeviceBuffer> buffers;
        MeshDescView view;
        std::unordered_map<const float *, const float *> attributes;
    };
    std::vector<MeshGeometry> m_geometry;
    GpuScene m_view;
    bool m_failed = false;

    // The wave: path state, two ray queues (this depth, the next), the
    // shadow queue and the queue lengths (rays A, rays B, shadow rays, AO
    // rays).
    uint32_t m_capacity = 0;
    uint32_t m_shadowSlots = 0; // shadow rays per path the wave holds (2, 3 with walks, +1 with shadow-linked domes)
    std::vector<DeviceBuffer> m_waveBuffers;
    GpuWave m_wave;
    GpuRays m_rays[2];
    DeviceBuffer m_counts;
    DeviceBuffer m_camera, m_filter;
    DeviceBuffer m_tables; // the BSDF tables and the XYZ matrix
    DeviceBuffer m_hitFilter; // the any-hit filter's GpuFilter (per pass)

    const ITextureSampler *m_sampler = nullptr;
    std::unordered_map<TextureId, Texture> m_textures;
    std::unordered_map<uint64_t, DeviceTexSlot> m_texSlots; // by texture << 16 | address modes, filter
    size_t m_textureBudget = 0, m_textureBytes = 0;
    uint32_t m_budgetSetting = 0;  // nova:gpuTextureBudget (MB, 0: the default)
    bool m_budgetChanged = false;  // the setting changed since the last plan
    bool m_budgetWarned = false;
    DeviceBuffer m_tileCones, m_pixelList;

    // Light path expressions: the tables on the device (uploaded when they
    // change; keyed by the caller's generation, not by the host tables'
    // addresses, which a freed and recompiled program can reuse), the
    // per-path states and the shadow rays' part arena.
    std::vector<DeviceBuffer> m_lpeBuffers, m_lpeWave;
    LpeView m_lpe;
    uint64_t m_lpeKey[5] = {};
    uint32_t m_lpeCapacity = 0;  // paths
    size_t m_lpeRayCapacity = 0; // shadow rays (parts: four per ray)
    // Volumes: the per-ray medium state (allocated when the scene has volumes).
    std::vector<DeviceBuffer> m_mediaWave;
    uint32_t m_mediaCapacity = 0;
    // Fake caustics: the shadow rays' transmittances and the crossings' two
    // queues with their hits (allocated when the scene has fake caustics).
    std::vector<DeviceBuffer> m_fakeWave;
    GpuRays m_fakeRays[2];
    uint32_t m_fakeCapacity = 0;
    // Subsurface walks: the queue (allocated when a material may walk) and
    // its copy for the walk raygen.
    bool m_mayWalk = false;
    std::vector<DeviceBuffer> m_walkWave;
    DeviceBuffer m_walkParams;
    uint32_t m_walkCapacity = 0;

    // Path guiding: the field's device copy (spatial nodes, leaves, sampling
    // nodes, building children, building leaves, building sums, leaf counts)
    // of the generation m_guideGeneration, its view, and the wave's records.
    std::vector<DeviceBuffer> m_guideBuffers;
    uint64_t m_guideGeneration = 0;
    GuideView m_guideView;
    std::vector<DeviceBuffer> m_guideWave;
    uint32_t m_guideCapacity = 0;

    // The film.
    DeviceBuffer m_colorSum, m_alphaSum, m_count, m_aoPlane, m_lpeSum, m_lpeLit;
    std::vector<uint32_t> m_cryptoMesh, m_cryptoPrim; // a wave's camera hits on the host
    int m_width = 0, m_height = 0;
};

} // namespace nova::gpu
