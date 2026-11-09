#pragma once

// Texture baking (docs/design/M11-P8.md): Karma's Texture Baker. The film is
// one UDIM tile of the bake (low) mesh's texture space; every pixel sample is
// a point on the low mesh, found by its texture coordinate, and the shading
// point is where rays from there meet the scene (Trace), the high mesh at the
// same texture coordinate (UV Match) or the low mesh itself (Single). The
// maps are filtered like the beauty: a sample adds its values, a sample that
// finds no shading point adds zero, so every map is premultiplied by the
// coverage (the alpha map, Af). Conventions measured with Karma CPU
// (Houdini 22.0.466), docs/design/M11-P8.md section 2.

#include "core/film/film.h"
#include "core/film/filter.h"
#include "core/scene/camera.h"
#include "core/scene/scene.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nova {

struct WaveQueues;
struct TileRect;
struct RenderSettings;

enum class BakeMode : uint8_t { Trace, UvMatch, Single };
// Where trace rays start: both ways along the low normal (none), from a cage
// with the low mesh's topology, from a cage found by texture coordinate, or
// from the low point pushed out by the trace bias (one inward ray).
enum class BakeCage : uint8_t { None, Topology, Uv, Offset };
// The position map's space: world, or fit into the low mesh's bounds, the
// scene's (visible geometry) or the user's box.
enum class BakeBox : uint8_t { World, BakeMesh, Scene, User };

struct BakeSettings {
    BakeMode mode = BakeMode::Trace;
    std::vector<MeshId> low;  // the bake meshes
    std::vector<MeshId> cage; // Topology: parallel to `low`; Uv: any
    std::vector<MeshId> high; // UV Match: the meshes the shading points are on
    BakeCage cageMode = BakeCage::None;
    std::string uv = "st"; // the texture coordinate primvar
    int udim = 1001;
    bool mikkt = true; // MikkTSpace tangents, else the texture coordinate derivatives
    float traceBias = 0.001f;
    float traceMax = 1e17f;
    // Per-point bias and maximum distance on the low mesh (empty: the
    // global values).
    std::string traceBiasAttribute, traceMaxAttribute;
    int ocSamples = 16;
    float ocMaxDist = -1.0f, ocBias = 0.001f;
    int cuSamples = 16;
    float cuScale = 1.0f, cuDist = 0.025f, cuBias = 0.75f;
    int cvSamples = 16;
    float cvDist = 0.05f, cvBias = 0.0f;
    int thSamples = 1;
    int egSamples = 9, egMode = 0;
    float egRadius = 0.01f, egCusp = 30.0f;
    int rnSamples = 9, rnMode = 0;
    float rnRadius = 0.01f, rnCusp = 30.0f;
    BakeBox box = BakeBox::World;
    bool boxUniform = true;
    float boxMin[3] = {-1.0f, -1.0f, -1.0f}, boxMax[3] = {1.0f, 1.0f, 1.0f};
    // Lighting outputs (beauty, LPEs) are path traced from the shading points.
    bool lighting = false;

    bool operator==(const BakeSettings &) const = default;
};

// Karma's curvature response: Schlick's bias function on [0, 1]
// (bias 0.5 is the identity, 0 maps to 0, 1 to 1 for any x > 0).
NOVA_INLINE float schlickBias(float x, float b) {
    if (x <= 0.0f)
        return 0.0f;
    if (b <= 0.0f)
        return 0.0f;
    return x / ((1.0f / b - 2.0f) * (1.0f - x) + 1.0f);
}

// The low mesh's frame and point at a texture coordinate.
struct BakePoint {
    uint32_t tri = 0; // into the baker's triangles
    Vec3f b{};        // barycentrics
    Vec3f P{}, N{}, T{}, B{};
    float bias = 0.0f, maxT = 0.0f;
};

// One pixel sample of a bake wave.
struct BakeSample {
    uint32_t pixel = 0;
    uint32_t index = 0; // the sample's number in its pixel
    bool hit = false;
    BakePoint low;
    SurfaceHit shade;           // the shading point
    Vec3f org{}, dir{};         // the ray that found it (its view: dir faces the front)
    float t = 0.0f;             // along it
    uint32_t prim = 0;          // the shading point's primitive and barycentrics (resolveHit's input)
    Vec2f uv{};
};

// Per-thread scratch of bake waves.
struct BakeScratch {
    std::vector<BakeSample> samples;
    std::vector<Vec3f> org, dir;
    std::vector<float> tMax, time, seed, t;
    std::vector<uint8_t> type, occluded;
    std::vector<uint32_t> mesh, prim, instance, owner;
    std::vector<Vec2f> uv;
    std::vector<float> values;  // query results per sample
    std::vector<float> exported; // material values per sample and plane (3 each)
};

class Baker {
public:
    // The bake meshes in world space (their first instance at the frame),
    // the texture-space lookup of the tile and the tangent frames.
    void build(const Scene &scene, const BakeSettings &settings);
    const BakeSettings &settings() const { return m_settings; }
    bool empty() const { return m_low.tris.empty(); }

    // The samples of a wave: `spp` samples for every pixel of the tile (or
    // the listed film pixels), each pixel's next ones (its count), with
    // their shading points. Returns the rays traced.
    uint64_t generate(const Scene &scene, const Camera &camera, const PixelFilter &filter, uint32_t seed,
                      const TileRect &tile, const uint32_t *pixels, uint32_t pixelCount, uint32_t spp,
                      const Film &film, BakeScratch &s) const;
    // Adds the samples' maps to the film's planes (BakeKind utilities and
    // the other utility planes at the shading points); `own`: also their
    // counts and alpha (no path tracing). Returns the rays traced.
    uint64_t accumulate(const Scene &scene, const RenderSettings &settings, const Camera &camera, uint32_t seed,
                        Film &film, BakeScratch &s, bool own) const;

    // The low mesh at a texture coordinate of the tile (u, v in [0, 1]),
    // for tests.
    bool lookup(Vec2f st, BakePoint &out) const;

private:
    struct Tri {
        Vec3f P[3], N[3], T[3];
        Vec2f uv[3];
        Vec3f dPdu, dPdv;
        float sign = 1.0f;
        float bias[3], maxT[3];
        MeshId mesh = kInvalidId;
        uint32_t prim = 0;
    };
    struct Surface {
        std::vector<Tri> tris;
        int grid = 0;
        std::vector<uint32_t> cellStart, cellTris;
        void index(Vec2f origin);
        bool find(Vec2f uv, Vec2f origin, uint32_t &tri, Vec3f &b) const;
    };
    void addMesh(const Scene &scene, MeshId id, Surface &out, bool frames) const;
    bool lowPoint(uint32_t tri, Vec3f b, BakePoint &out) const;

    BakeSettings m_settings;
    Surface m_low, m_cage, m_high;
    Vec2f m_origin{0.0f, 0.0f}; // the tile's texture-space corner
    std::vector<Vec3f> m_cagePoints; // Topology: per low triangle corner
    Vec3f m_boxMin{}, m_boxSize{1.0f, 1.0f, 1.0f};
};

} // namespace nova
