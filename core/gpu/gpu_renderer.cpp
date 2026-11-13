#include "core/gpu/gpu_renderer.h"

#include "core/bsdf/thin_film.h"
#include "core/integrator/volume_media.h"
#include "core/gpu/embedded.h"
#include "core/scene/scene.h"
#include "core/texture/ray_cone.h"
#include "core/texture/texel_stream.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace nova::gpu {

namespace {

constexpr uint32_t kBlock = 256;
// Paths per wave (docs/design/M10.md D16): at most 2M (1080p at one sample),
// fewer when the device's free memory is short (planWave), never planned
// below 64k.
constexpr uint32_t kMaxWavePaths = 1u << 21;
constexpr uint32_t kMinWavePaths = 1u << 16;
// The kernels' stacks per streaming multiprocessor, for the plan before the
// first launch: measured on an RTX 5080 (84 of them) 23.5 MB each with the
// kernels of the RTX 50 series' compiler and 27.7 MB with those of the older
// architectures' (docs/design/M10.md, section 8).
constexpr size_t kStackBytesPerProcessor = size_t(32) << 20;

// GpuRenderer::setKernelImage: the fatbin to load instead of the embedded one.
const void *g_kernelImage = nullptr;

// BsdfTables' members in the order of kBsdfTableFloats.
constexpr const float *BsdfTables::*kTableMembers[kBsdfTableCount] = {
    &BsdfTables::ggxE,       &BsdfTables::ggxEavg, &BsdfTables::ggxABC,   &BsdfTables::dielRT, &BsdfTables::dielRTinv,
    &BsdfTables::dielR,      &BsdfTables::fresnelAvg, &BsdfTables::charlieE, &BsdfTables::thinRT, &BsdfTables::thinR};

// Uploads the bands of rows a sampler reads (ITextureSampler::textureLevels)
// into the levels of a tile's chain as they come: no level is ever whole in
// host memory.
class ChainUpload final : public TexRowSink {
public:
    ChainUpload(Device &device, CUmipmappedArray chain, const TexTileInfo &tile, const TexTilePlan &plan,
                size_t texel)
        : m_device(device), m_chain(chain), m_tile(tile), m_plan(plan), m_texel(texel) {}
    bool put(const TexRows &r) override {
        const uint32_t l = r.level;
        if (l < m_plan.first || l - m_plan.first >= m_plan.count || r.width != levelSize(m_tile.width, l) ||
            r.height != levelSize(m_tile.height, l) || r.y < 0 || r.rows <= 0 || r.y + r.rows > r.height || !r.texels)
            return false;
        const CudaApi &cu = m_device.cu();
        if (l != m_level) {
            if (!m_device.check(cu.p_cuMipmappedArrayGetLevel(&m_array, m_chain, l - m_plan.first),
                                "cuMipmappedArrayGetLevel"))
                return false;
            m_level = l;
        }
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_HOST;
        copy.srcHost = r.texels;
        copy.srcPitch = size_t(r.width) * m_texel;
        copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        copy.dstArray = m_array;
        copy.dstY = size_t(r.y);
        copy.WidthInBytes = size_t(r.width) * m_texel;
        copy.Height = size_t(r.rows);
        if (!m_device.check(cu.p_cuMemcpy2DAsync(&copy, m_device.stream()), "cuMemcpy2DAsync") ||
            !m_device.sync()) // the band is reused
            return false;
        m_bytes += size_t(r.width) * size_t(r.rows) * m_texel;
        return true;
    }
    // Whether every planned level came whole.
    bool complete() const { return m_bytes == m_plan.bytes; }

private:
    Device &m_device;
    CUmipmappedArray m_chain;
    const TexTileInfo &m_tile;
    const TexTilePlan &m_plan;
    size_t m_texel;
    CUarray m_array = nullptr;
    uint32_t m_level = ~0u;
    size_t m_bytes = 0;
};

// Whether a material's closures can hold a subsurface walk: a native model
// with a subsurface weight (uniform, or one its program writes), or a graph
// with a subsurface leaf or a native segment (conservatively).
bool mayWalk(const SurfaceInputs &in, const shading::Program *p) {
    const auto native = [](SurfaceModel m) { return m == SurfaceModel::StandardSurface || m == SurfaceModel::OpenPBR; };
    if (in.model == SurfaceModel::Graph) {
        if (!in.graph)
            return false;
        const ClosureGraph &g = *in.graph;
        for (int i = 0; i < g.nodeCount; ++i)
            if (g.node[i].op == GraphOp::Leaf && g.node[i].kind == LeafKind::Subsurface)
                return true;
        for (int s = 0; s < g.segmentCount; ++s)
            if (native(g.segment[s].model))
                return true;
        return false;
    }
    if (!native(in.model))
        return false;
    const uint16_t weight = surface_field::subsurfaceWeight;
    if (in.f(weight) > 0.0f)
        return true;
    if (p)
        for (const shading::OutputBinding &o : p->outputs)
            if (o.field <= weight && weight < o.field + o.width)
                return true;
    return false;
}

// Which textures the device samples (D10), one rule for the texture budget's
// plan and setScene's upload: a surface material's when its per-hit program
// or per-hit opacity program runs on the device (VEX materials are grey
// stand-ins, D12; a program over kGpuMaxRows falls back to its uniform
// inputs), a volume's material's when its shader runs there.
bool standIn(const MaterialRecord &rec) {
    return rec.resolved.model == SurfaceModel::External || (rec.desc.external && rec.desc.external->shader);
}

bool opacityRuns(const MaterialRecord &rec) {
    const shading::Program *p = rec.desc.program.get();
    const shading::Program *op = p ? p->opacity.get() : nullptr;
    return op && !rec.opacityUniforms.empty() && op->rowCount <= kGpuMaxRows;
}

bool surfaceTexturesOnDevice(const MaterialRecord &rec) {
    const shading::Program *p = rec.desc.program.get();
    return !standIn(rec) && p && ((rec.varying && p->rowCount <= kGpuMaxRows) || opacityRuns(rec));
}

bool volumeShaderOnDevice(const VolumeRecord &rec) { return rec.program && rec.program->rowCount <= kGpuMaxRows; }

// The texture samplers the device uses: the surface materials', then the
// volume shaders'. The budget favours them in this order, so it must not
// follow the ids, which follow the host's sync order (docs/bug-audit-2026-11.md
// D-1): materials go by their names' cryptomatte ids, volumes by their keys.
template <class F> void forEachDeviceSampler(const Scene &scene, F &&f) {
    std::vector<MaterialId> materials;
    for (MaterialId id = 0; id < scene.materialCount(); ++id)
        if (scene.hasMaterial(id) && surfaceTexturesOnDevice(scene.material(id)))
            materials.push_back(id);
    std::sort(materials.begin(), materials.end(), [&](MaterialId a, MaterialId b) {
        const uint32_t ka = scene.material(a).desc.cryptoId, kb = scene.material(b).desc.cryptoId;
        return ka != kb ? ka < kb : a < b;
    });
    for (MaterialId id : materials)
        for (const TexSampler &s : scene.material(id).samplers)
            f(s);
    std::vector<VolumeId> volumes;
    for (VolumeId id = 0; id < scene.volumeCount(); ++id)
        if (volumeShaderOnDevice(scene.volumeRecord(id)))
            volumes.push_back(id);
    std::sort(volumes.begin(), volumes.end(), [&](VolumeId a, VolumeId b) {
        const uint64_t ka = scene.volumeRecord(a).key, kb = scene.volumeRecord(b).key;
        return ka != kb ? ka < kb : a < b;
    });
    for (VolumeId id : volumes)
        for (const TexSampler &s : scene.material(scene.volumeRecord(id).material).samplers)
            f(s);
}

} // namespace

GpuRenderer::~GpuRenderer() {
    releaseTextures();
    if (m_module) {
        Device::Scope scope(m_device);
        m_device.cu().p_cuModuleUnload(m_module);
    }
}

void GpuRenderer::freeTexture(TextureId id, Texture &tex) {
    const CudaApi &cu = m_device.cu();
    for (CUtexObject o : tex.objects)
        cu.p_cuTexObjectDestroy(o);
    for (auto &[udim, chain] : tex.tiles)
        cu.p_cuMipmappedArrayDestroy(chain);
    tex.objects.clear();
    tex.tables.clear();
    tex.tiles.clear();
    tex.levels.clear();
    tex.ok = tex.uploaded = false;
    m_textureBytes -= std::min(m_textureBytes, tex.bytes);
    tex.bytes = 0;
    for (auto it = m_texSlots.begin(); it != m_texSlots.end();)
        it = (it->first >> 16) == id ? m_texSlots.erase(it) : std::next(it);
}

void GpuRenderer::freeTextureData() {
    if (m_textures.empty())
        return;
    Device::Scope scope(m_device);
    m_device.sync();
    for (auto &[id, tex] : m_textures)
        freeTexture(id, tex);
    m_texSlots.clear();
    m_textureBytes = 0;
}

void GpuRenderer::releaseTextures() {
    freeTextureData();
    m_textures.clear();
}

bool GpuRenderer::texturesChanged(const ITextureSampler *sampler) const {
    if (!sampler || sampler != m_sampler)
        return false;
    for (const auto &[id, tex] : m_textures)
        if (sampler->textureGeneration(id) != tex.generation)
            return true;
    return false;
}

GpuRenderer::Texture &GpuRenderer::textureEntry(const ITextureSampler *sampler, TextureId id) {
    auto [it, fresh] = m_textures.try_emplace(id);
    Texture &tex = it->second;
    if (fresh) {
        // Also for a missing file: one that appears later is a new generation.
        tex.generation = sampler->textureGeneration(id);
        tex.known = sampler->textureInfo(id, tex.info) && !tex.info.tiles.empty();
    }
    return tex;
}

size_t GpuRenderer::textureBudget() {
    if (m_textureBudget == 0) {
        // nova:gpuTextureBudget's default: half the memory free when the
        // first texture loads (with the textures already held, after a
        // change of the setting).
        m_textureBudget = std::max<size_t>((m_device.freeMemory() + m_textureBytes) / 2, 1);
    }
    return m_textureBudget;
}

GpuRenderer::TexturePlan GpuRenderer::planTextures(const Scene &scene) {
    TexturePlan p;
    if (!m_sampler)
        return p;
    std::unordered_set<TextureId> seen;
    forEachDeviceSampler(scene, [&](const TexSampler &s) {
        if (s.texture != kNoTexture && seen.insert(s.texture).second)
            p.used.push_back(s.texture);
    });
    std::vector<const TexInfo *> infos;
    infos.reserve(p.used.size());
    bool any = false;
    for (TextureId id : p.used) {
        const Texture &tex = textureEntry(m_sampler, id);
        infos.push_back(tex.known ? &tex.info : nullptr);
        any = any || tex.known;
    }
    // The default budget is measured only once there are textures to load.
    p.budget = any ? textureBudget() : m_textureBudget;
    p.plan = planTextureBudget(infos, p.budget);
    return p;
}

bool GpuRenderer::texturesMatch(const TexturePlan &p) const {
    std::unordered_set<TextureId> used(p.used.begin(), p.used.end());
    for (size_t i = 0; i < p.used.size(); ++i) {
        const auto it = m_textures.find(p.used[i]);
        if (it == m_textures.end() || it->second.plan != p.plan.textures[i])
            return false;
    }
    size_t unused = 0;
    for (const auto &[id, tex] : m_textures)
        if (tex.uploaded && !used.count(id))
            unused += tex.bytes;
    return unused == 0 || unused + p.plan.bytes <= p.budget;
}

void GpuRenderer::applyTexturePlan(TexturePlan &p) {
    // Device copies stay while they hold what the plan keeps, and those no
    // longer used while they leave room for it; so the device holds what a
    // fresh upload of this scene would (IPR), reading again only what the
    // plan changes.
    std::unordered_set<TextureId> used(p.used.begin(), p.used.end());
    std::vector<TextureId> drop;
    size_t unused = 0;
    for (size_t i = 0; i < p.used.size(); ++i) {
        const Texture &tex = m_textures[p.used[i]];
        if (tex.uploaded && tex.plan != p.plan.textures[i])
            drop.push_back(p.used[i]);
    }
    for (const auto &[id, tex] : m_textures)
        if (tex.uploaded && !used.count(id))
            unused += tex.bytes;
    if (unused > 0 && unused + p.plan.bytes > p.budget)
        for (const auto &[id, tex] : m_textures)
            if (tex.uploaded && !used.count(id))
                drop.push_back(id);
    if (!drop.empty()) {
        m_device.sync();
        for (TextureId id : drop)
            freeTexture(id, m_textures[id]);
    }
    for (size_t i = 0; i < p.used.size(); ++i)
        m_textures[p.used[i]].plan = std::move(p.plan.textures[i]);
    m_budgetChanged = false;
    if (p.plan.reduced && !m_budgetWarned) {
        m_budgetWarned = true;
        std::fprintf(stderr,
                     "[Nova] XPU: textures need %.0f MB, more than the GPU texture budget (%.0f MB); the finer MIP "
                     "levels of the largest are left out (down to %d texels)\n",
                     double(p.plan.fullBytes) / 1048576.0, double(p.budget) / 1048576.0,
                     1 << std::max(p.plan.cap, 0));
    }
}

GpuRenderer::Texture &GpuRenderer::texture(const ITextureSampler *sampler, TextureId id) {
    Texture &tex = textureEntry(sampler, id);
    if (tex.uploaded)
        return tex;
    tex.uploaded = true;
    if (!tex.known)
        return tex;
    if (tex.plan.size() != tex.info.tiles.size()) { // not planned: what the budget has left
        const size_t budget = textureBudget(), left = budget > m_textureBytes ? budget - m_textureBytes : 0;
        tex.plan = planTextureBudget({&tex.info}, left).textures[0];
    }
    const CudaApi &cu = m_device.cu();
    const size_t texel = texelBytes(tex.info);
    // Host memory first: nothing may throw (std::bad_alloc, which
    // Renderer::gpuPass survives) while a chain has no owner.
    tex.tiles.reserve(tex.info.tiles.size());
    tex.levels.reserve(tex.info.tiles.size());
    for (size_t i = 0; i < tex.info.tiles.size(); ++i) {
        const TexTileInfo &tile = tex.info.tiles[i];
        const TexTilePlan &plan = tex.plan[i];
        if (plan.count == 0)
            continue;
        CUDA_ARRAY3D_DESCRIPTOR desc{};
        desc.Width = size_t(levelSize(tile.width, plan.first));
        desc.Height = size_t(levelSize(tile.height, plan.first));
        desc.Format = tex.info.half ? CU_AD_FORMAT_HALF : CU_AD_FORMAT_FLOAT;
        desc.NumChannels = 4;
        CUmipmappedArray chain = nullptr;
        if (!m_device.check(cu.p_cuMipmappedArrayCreate(&chain, &desc, plan.count), "cuMipmappedArrayCreate"))
            continue;
        tex.tiles.emplace_back(tile.udim, chain); // owned from here on (reserved: cannot throw)
        tex.levels.push_back(plan.count);
        ChainUpload upload(m_device, chain, tile, plan, texel);
        TexLevelRequest request;
        request.udim = tile.udim;
        request.first = plan.first;
        request.count = plan.count;
        request.half = tex.info.half;
        if (!sampler->textureLevels(id, request, upload) || !upload.complete()) {
            m_device.sync();
            cu.p_cuMipmappedArrayDestroy(chain);
            tex.tiles.pop_back();
            tex.levels.pop_back();
            continue;
        }
        tex.bytes += plan.bytes;
        m_textureBytes += plan.bytes;
    }
    tex.udim = tex.info.udim;
    tex.ok = !tex.tiles.empty();
    return tex;
}

CUtexObject GpuRenderer::textureObject(Texture &tex, CUmipmappedArray chain, uint32_t levels, const TexSlot &slot,
                                       bool udim) {
    const auto mode = [udim](TexWrap w) {
        if (udim) // tiles never wrap into each other
            return CU_TR_ADDRESS_MODE_CLAMP;
        switch (w) {
        case TexWrap::Periodic:
            return CU_TR_ADDRESS_MODE_WRAP;
        case TexWrap::Mirror:
            return CU_TR_ADDRESS_MODE_MIRROR;
        case TexWrap::Black:
            return CU_TR_ADDRESS_MODE_BORDER; // border color 0
        default: // Clamp; Constant's outside is the fallback before the lookup
            return CU_TR_ADDRESS_MODE_CLAMP;
        }
    };
    CUDA_RESOURCE_DESC res{};
    res.resType = CU_RESOURCE_TYPE_MIPMAPPED_ARRAY;
    res.res.mipmap.hMipmappedArray = chain;
    CUDA_TEXTURE_DESC desc{};
    desc.addressMode[0] = mode(slot.wrapS);
    desc.addressMode[1] = mode(slot.wrapT);
    desc.addressMode[2] = CU_TR_ADDRESS_MODE_CLAMP;
    const bool closest = slot.filter == TexFilter::Closest; // cubic filters linearly
    desc.filterMode = closest ? CU_TR_FILTER_MODE_POINT : CU_TR_FILTER_MODE_LINEAR;
    desc.mipmapFilterMode = desc.filterMode;
    desc.maxAnisotropy = closest ? 1 : 8;
    desc.maxMipmapLevelClamp = float(levels - 1);
    desc.flags = CU_TRSF_NORMALIZED_COORDINATES;
    CUtexObject object = 0;
    if (tex.objects.size() == tex.objects.capacity()) // the push_back below cannot throw
        tex.objects.reserve(std::max<size_t>(4, tex.objects.capacity() * 2));
    if (!m_device.check(m_device.cu().p_cuTexObjectCreate(&object, &res, &desc, nullptr), "cuTexObjectCreate"))
        return 0;
    tex.objects.push_back(object);
    return object;
}

DeviceTexSlot GpuRenderer::textureSlot(const ITextureSampler *sampler, const TexSampler &s) {
    DeviceTexSlot d;
    d.slot = s.slot;
    if (!sampler || s.texture == kNoTexture)
        return d;
    const uint64_t key = uint64_t(s.texture) << 16 | uint64_t(s.slot.wrapS) << 8 | uint64_t(s.slot.wrapT) << 4 |
                         uint64_t(s.slot.filter);
    auto it = m_texSlots.find(key);
    if (it == m_texSlots.end()) {
        DeviceTexSlot made;
        Texture &tex = texture(sampler, s.texture);
        if (tex.ok && !tex.udim) {
            made.object = textureObject(tex, tex.tiles[0].second, tex.levels[0], s.slot, false);
        } else if (tex.ok) {
            std::vector<unsigned long long> objects(kUdimTiles, 0);
            for (size_t i = 0; i < tex.tiles.size(); ++i) {
                const uint32_t udim = tex.tiles[i].first;
                if (udim >= 1001 && udim < 1001 + uint32_t(kUdimTiles))
                    objects[udim - 1001] = textureObject(tex, tex.tiles[i].second, tex.levels[i], s.slot, true);
            }
            tex.tables.emplace_back();
            if (m_device.upload(tex.tables.back(), objects))
                made.tiles = tex.tables.back().as<unsigned long long>();
        }
        it = m_texSlots.emplace(key, made).first;
    }
    d.object = it->second.object;
    d.tiles = it->second.tiles;
    return d;
}

void GpuRenderer::setKernelImage(const void *fatbin) { g_kernelImage = fatbin; }

bool GpuRenderer::init(std::string &error) {
    if (!m_device.init(error) || !m_accel.init(error))
        return false;
    Device::Scope scope(m_device);
    const CudaApi &cu = m_device.cu();
    const auto function = [&](CUfunction &fn, const char *name) {
        return m_device.check(cu.p_cuModuleGetFunction(&fn, m_module, name), name);
    };
    // The driver takes from the fatbin the code that fits the device. Where it
    // holds none for the device's architecture the driver compiles its PTX,
    // which takes minutes; the driver keeps the result, so only once.
    const void *kernels = g_kernelImage ? g_kernelImage : static_cast<const void *>(kNovaKernels_words);
    if (!g_kernelImage && !m_device.hasKernelCode())
        std::fprintf(stderr,
                     "[Nova] XPU: no kernels built for the architecture of %s: the NVIDIA driver compiles "
                     "them now, which takes several minutes the first time\n",
                     m_device.description().c_str());
    if (!m_device.check(cu.p_cuModuleLoadData(&m_module, kernels), "cuModuleLoadData") ||
        !function(m_generate, "novaGenerate") || !function(m_shade, "novaShade") ||
        !function(m_shadow, "novaShadow") || !function(m_ao, "novaAo") || !function(m_flush, "novaFlush") ||
        !function(m_media, "novaMedia") || !function(m_volumes, "novaVolumes") ||
        !function(m_fakeStart, "novaFakeStart") || !function(m_fakeStep, "novaFakeStep") ||
        !function(m_walkExit, "novaWalkExit") || !function(m_guideSnap, "novaGuideSnap") ||
        !function(m_guideRecord, "novaGuideRecord") ||
        !function(m_outlineMain, "novaOutlineMain") ||
        !function(m_outlineRays, "novaOutlineRays") || !function(m_outlineFlags, "novaOutlineFlags") ||
        !function(m_outlineFinish, "novaOutlineFinish") ||
        !m_counts.reserve(m_device, 8 * sizeof(uint32_t)) ||
        !m_device.check(cu.p_cuMemsetD8Async(m_counts.ptr(), 0, m_counts.size(), m_device.stream()), "cuMemsetD8Async")) {
        error = m_device.lastError();
        return false;
    }
    // The BSDF tables and the XYZ matrix (constant): one buffer.
    const BsdfTables &host = bsdfTables();
    std::vector<float> blob;
    size_t offsets[kBsdfTableCount + 1] = {};
    for (int i = 0; i < kBsdfTableCount; ++i) {
        offsets[i] = blob.size();
        blob.insert(blob.end(), host.*kTableMembers[i], host.*kTableMembers[i] + kBsdfTableFloats[i]);
    }
    offsets[kBsdfTableCount] = blob.size();
    blob.insert(blob.end(), kXyzToRec709Grey, kXyzToRec709Grey + 9);
    if (!m_device.upload(m_tables, blob)) {
        error = m_device.lastError();
        return false;
    }
    for (int i = 0; i < kBsdfTableCount; ++i)
        m_view.tables.*kTableMembers[i] = m_tables.as<float>() + offsets[i];
    m_view.xyzToRgb = m_tables.as<float>() + offsets[kBsdfTableCount];
    if (!m_device.sync()) {
        error = m_device.lastError();
        return false;
    }
    return true;
}

template <class T>
Span<T> GpuRenderer::upload(std::vector<DeviceBuffer> &into, const T *data, size_t count) {
    if (count == 0 || !data)
        return {};
    into.emplace_back();
    if (!m_device.upload(into.back(), data, count)) {
        m_failed = true;
        return {};
    }
    return {into.back().template as<T>(), count};
}

void GpuRenderer::setTextureBudget(uint32_t megabytes) {
    if (megabytes == m_budgetSetting)
        return;
    m_budgetSetting = megabytes;
    m_textureBudget = size_t(megabytes) << 20; // 0: the default, measured when textures load next
    m_budgetChanged = true;
    m_budgetWarned = false;
}

bool GpuRenderer::setScene(const Scene &scene) {
    // A texture the host prepared again under its id (changed on disk) leaves
    // the scene's version as it was: its stale device copy goes, and the
    // materials are uploaded again with the new texture objects.
    const bool texturesStale = texturesChanged(scene.textureSampler());
    if (&scene == m_scene && scene.version() == m_sceneVersion && !texturesStale) {
        if (!m_budgetChanged)
            return true;
        // A new texture budget (IPR) and the same scene: uploaded again only
        // if the new plan keeps other levels.
        Device::Scope scope(m_device);
        if (texturesMatch(planTextures(scene))) {
            m_budgetChanged = false;
            return true;
        }
    }
    Device::Scope scope(m_device);
    m_scene = nullptr;
    m_failed = false;
    m_sceneBuffers.clear();
    uint32_t skipped = 0;
    if (!m_accel.build(scene, skipped))
        return false;
    if (skipped > 0)
        std::fprintf(stderr, "[Nova] XPU: %u objects without primitives the GPU can build are left out\n", skipped);

    // Meshes: geometry (kept while it stays), instances, the material of each
    // triangle, ray limits and the attributes their materials read (each
    // array uploaded once).
    std::vector<GpuMesh> meshes(scene.meshCount());
    m_geometry.resize(scene.meshCount());
    std::vector<uint32_t> slots;
    std::vector<GpuBinding> bindings;
    std::vector<AttributeRef> refs;
    std::unordered_map<const float *, const float *> attributes;
    for (MeshId id = 0; id < scene.meshCount(); ++id) {
        const MeshRecord &rec = scene.meshRecord(id);
        MeshGeometry &geo = m_geometry[id];
        if (!OptixScene::supports(rec)) {
            geo = MeshGeometry{};
            continue;
        }
        GpuMesh &g = meshes[id];
        const MeshDesc &d = rec.desc;
        if (!geo.version || geo.version != rec.geometryVersion) {
            geo = MeshGeometry{};
            geo.version = rec.geometryVersion;
            const auto keep = [&](const auto &a) { return upload(geo.buffers, a.data(), a.size()); };
            MeshDescView &v = geo.view;
            v.points = keep(d.points);
            v.pointSteps = d.pointSteps;
            v.shape = d.shape;
            v.sphereRadius = keep(d.sphereRadius);
            v.indices = keep(d.indices);
            v.normals = keep(d.normals);
            v.curveSegments = keep(d.curveSegments);
            v.segmentCurve = keep(d.segmentCurve);
            v.curveRadius = keep(d.curveRadius);
            v.curveBasis = d.curveBasis;
            v.curveStyle = d.curveStyle;
            v.pointRadius = keep(d.pointRadius);
            v.pointStyle = d.pointStyle;
        }
        g.view.desc = geo.view;
        g.view.instances = put(rec.instances);
        g.view.motionSteps = rec.motionSteps;
        g.view.key = rec.key;
        if (rec.motionSteps > 1) { // OptiX's keys and interpolation (D6)
            g.view.motionKeys = put(OptixScene::continuousKeys(rec));
            g.view.nlerp = 1;
        }
        slots.clear();
        for (MaterialId m : rec.materialSlots)
            slots.push_back(scene.hasMaterial(m) ? m : kDefaultMaterialId);
        g.triangleSlots = put(rec.triangleSlots);
        g.slotMaterial = put(slots);
        g.rayLimits = rec.rayLimits;
        // An attribute's device copy: the description's arrays with the
        // geometry, instance-rate ones with every upload.
        const auto deviceRef = [&](AttributeRef &r) {
            if (!r.data)
                return;
            const bool own = r.source != AttributeSource::Instance;
            const float *&device = (own ? geo.attributes : attributes)[r.data];
            if (!device) {
                // The counts Scene::bindName validated: Uniform is per hit
                // prim (triangle, sphere, curve segment or point), curve
                // Corner three values per segment.
                const size_t corners = rec.desc.isCurves() ? 3 * rec.desc.primitiveCount() : rec.desc.indices.size();
                const size_t count = r.source == AttributeSource::Constant  ? 1
                                     : r.source == AttributeSource::Uniform  ? rec.desc.primitiveCount()
                                     : r.source == AttributeSource::Vertex   ? rec.desc.pointCount()
                                     : r.source == AttributeSource::Corner   ? corners
                                     : r.source == AttributeSource::Instance ? rec.instanceCount()
                                                                             : 0;
                device = own ? upload(geo.buffers, r.data, count * r.width).ptr : put(r.data, count * r.width).ptr;
            }
            r.data = device;
            if (!r.data)
                r.source = AttributeSource::None;
        };
        bindings.clear();
        for (const MeshRecord::MaterialBinding &b : rec.bindings) {
            refs = b.refs;
            for (AttributeRef &r : refs)
                deviceRef(r);
            bindings.push_back({b.material, put(refs).ptr});
        }
        g.outline = rec.outline; // Karma's outlines and simplified shading (M11 P9a)
        g.displayColor = rec.displayColor;
        deviceRef(g.displayColor);
        g.bindings = put(bindings);
        g.shadowMasks = put(rec.shadowMasks);
        g.fakeCaustics = uint8_t(rec.fakeCaustics);
        g.shading = rec.shading;
        g.visibility = rec.visibility;
        g.holdout = uint8_t(rec.holdout);
        g.emitterOf = put(rec.emitterOf);
        g.emitterBase = rec.emitterBase;
        g.emitterStride = rec.emitterStride;
        g.receiverSets = put(rec.receiverSets);
    }
    m_view.meshes = put(meshes).ptr;

    // Materials: the inputs with the uniform outputs applied (graphs
    // uploaded), and the programs of those that vary per hit with their
    // textures. External (VEX) materials are grey stand-ins (D12).
    if (scene.textureSampler() != m_sampler || texturesStale) {
        releaseTextures();
        m_sampler = scene.textureSampler();
    }
    TexturePlan texturePlan = planTextures(scene);
    applyTexturePlan(texturePlan);
    std::vector<DeviceTexSlot> texSlots;
    std::vector<SurfaceInputs> inputs(scene.materialCount(), lambertDefaults());
    std::vector<GpuMaterial> materials(scene.materialCount());
    uint32_t standIns = 0, large = 0;
    m_mayWalk = false;
    for (MaterialId id = 0; id < scene.materialCount(); ++id) {
        if (!scene.hasMaterial(id))
            continue;
        const MaterialRecord &rec = scene.material(id);
        SurfaceInputs &in = inputs[id];
        if (standIn(rec)) {
            in = lambertDefaults(spectrum(0.5f));
            ++standIns;
            continue;
        }
        in = rec.resolved;
        const shading::Program *p = rec.desc.program.get();
        m_mayWalk = m_mayWalk || mayWalk(in, p);
        if (in.model == SurfaceModel::Graph)
            in.graph = in.graph ? put(in.graph, 1).ptr : nullptr;
        GpuMaterial &g = materials[id];
        // Opacity (M3 K10): uniform, or the per-hit opacity program.
        g.hasOpacity = uint8_t(rec.hasOpacity);
        g.uniformOpacity = rec.uniformOpacity;
        const shading::Program *op = p ? p->opacity.get() : nullptr;
        const bool opacityVaries = opacityRuns(rec);
        if (opacityVaries) {
            g.opacityVaries = 1;
            g.opacityCode = put(op->code).ptr;
            g.opacityCodeSize = uint32_t(op->code.size());
            g.opacityUniforms = put(rec.opacityUniforms).ptr;
            g.opacityGlobals = put(op->globals);
            g.opacityPrimvars = put(op->primvarBindings);
            g.opacityOutputs = put(op->outputs);
        }
        if (p && (rec.varying || opacityVaries)) { // the primvar order and textures programs share
            g.primvarNames = uint32_t(p->primvars.size());
            g.tangentUv = p->tangentUv;
            if (surfaceTexturesOnDevice(rec)) { // as the texture plan
                texSlots.clear();
                for (const TexSampler &s : rec.samplers)
                    texSlots.push_back(textureSlot(m_sampler, s));
                g.textures = put(texSlots).ptr;
                g.textureCount = uint32_t(texSlots.size());
            }
        }
        if (!rec.varying || !p)
            continue;
        if (p->rowCount > kGpuMaxRows) {
            ++large;
            continue;
        }
        g.code = put(p->code).ptr;
        g.codeSize = uint32_t(p->code.size());
        g.uniforms = put(rec.uniforms).ptr;
        g.globals = put(p->globals);
        g.primvars = put(p->primvarBindings);
        g.outputs = put(p->outputs);
        g.varying = 1;
        g.footprints = !rec.samplers.empty() || p->needsFootprints;
    }
    if (standIns > 0)
        std::fprintf(stderr, "[Nova] XPU: %u VEX materials render as grey on the GPU\n", standIns);
    if (large > 0)
        std::fprintf(stderr, "[Nova] XPU: %u materials need more than %u rows; the GPU uses their uniform inputs\n",
                     large, kGpuMaxRows);
    const Span<SurfaceInputs> resolved = put(inputs);
    for (MaterialId id = 0; id < scene.materialCount(); ++id)
        materials[id].resolved = resolved.ptr + id;
    m_view.materials = put(materials).ptr;

    // Lights, their textures, dome distributions and filters (M11-P4), the
    // power table.
    std::vector<LightData> lights = scene.lights();
    for (LightData &l : lights) {
        if (l.texels)
            l.texels = put(l.texels, size_t(l.texWidth) * l.texHeight).ptr;
        if (l.filters)
            l.filters = put(l.filters, l.filterCount).ptr;
        if (l.portals) // a dome's light portals (M11-P9b)
            l.portals = put(l.portals, l.portalCount).ptr;
        Distribution2DView &d = l.distribution;
        if (d.func) {
            const size_t nu = d.nu, nv = d.nv;
            d.func = put(d.func, nu * nv).ptr;
            d.conditionalCdf = put(d.conditionalCdf, nv * (nu + 1)).ptr;
            d.rowIntegral = put(d.rowIntegral, nv).ptr;
            d.marginalCdf = put(d.marginalCdf, nv + 1).ptr;
        }
    }
    m_view.lights = put(lights).ptr;
    m_view.lightCount = scene.analyticLightCount();
    const LightSamplerView sampler = scene.lightSampler();
    m_view.lightSampler.pmf = put(sampler.pmf, lights.size()).ptr;
    m_view.lightSampler.threshold = put(sampler.threshold, sampler.count).ptr;
    m_view.lightSampler.alias = put(sampler.alias, sampler.count).ptr;
    m_view.lightSampler.count = sampler.count;
    // The light tree (M9 D11) with its lights at infinity, the light-linking
    // masks (D15).
    const LightTreeView tree = scene.lightTree();
    const size_t treeLights = tree.lightCount;
    LightTreeView &t = m_view.tree;
    t = tree;
    t.nodes = put(tree.nodes, tree.nodeCount).ptr;
    t.trail = put(tree.trail, treeLights).ptr;
    t.infinite = put(tree.infinite, tree.infiniteCount).ptr;
    t.infiniteE = put(tree.infiniteE, tree.infiniteCount).ptr;
    t.infiniteDir = put(tree.infiniteDir, tree.infiniteCount).ptr;
    t.infiniteTable.pmf = put(tree.infiniteTable.pmf, tree.infiniteCount).ptr;
    t.infiniteTable.threshold = put(tree.infiniteTable.threshold, tree.infiniteTable.count).ptr;
    t.infiniteTable.alias = put(tree.infiniteTable.alias, tree.infiniteTable.count).ptr;
    t.infiniteSlot = put(tree.infiniteSlot, treeLights).ptr;
    t.cut = put(tree.cut, tree.cutCount).ptr;
    if (tree.portalCount && tree.infiniteCount > 0) { // domes with light portals (M11-P9b)
        const uint32_t last = tree.infiniteCount - 1;
        t.portalTris = put(tree.portalTris, size_t(tree.portalBegin[last]) + tree.portalCount[last]).ptr;
        t.portalBegin = put(tree.portalBegin, tree.infiniteCount).ptr;
        t.portalCount = put(tree.portalCount, tree.infiniteCount).ptr;
    }
    t.links.light = put(tree.links.light, treeLights).ptr;
    t.links.receiver = put(tree.links.receiver, tree.receiverSetCount).ptr;
    m_view.links = t.links; // over every light, as Scene::lightLinks()
    m_view.shadowDomes = put(scene.shadowLinkedDomes()).ptr;
    m_view.shadowDomeCount = uint32_t(scene.shadowLinkedDomes().size());
    m_view.anyEmitters = uint8_t(scene.anyEmitters());
    uint32_t categories = 0;
    for (const LightData &l : lights)
        categories += l.shadowLinked() && l.shadowCategory >= 7;
    if (categories > 0)
        std::fprintf(stderr, "[Nova] XPU: shadow-link categories beyond the 7th are not separated on the GPU yet "
                             "(%u lights): every object shadows them\n", categories);
    m_view.hasAreaLights = m_view.hasVisibleAreaLights = 0;
    for (uint32_t i = 0; i < m_view.lightCount; ++i) {
        const bool area = isAreaLight(lights[i].type) && !lights[i].delta;
        m_view.hasAreaLights |= uint8_t(area);
        m_view.hasVisibleAreaLights |= uint8_t(area && lights[i].cameraVisible);
    }
    // Volumes (G5): the trackers' data, the fields' NanoVDB grids (each
    // buffer once), the volume shaders with their uniforms and textures.
    std::vector<GpuVolume> volumes(scene.volumeCount());
    std::unordered_map<const void *, const void *> gridCopies;
    std::vector<const void *> grids;
    std::vector<int32_t> fieldBindings;
    uint32_t largeShaders = 0;
    for (VolumeId id = 0; id < scene.volumeCount(); ++id) {
        const VolumeRecord &rec = scene.volumeRecord(id);
        GpuVolume &g = volumes[id];
        g.receiverSet = rec.receiverSet;
        MediumVolume m = mediumVolume(rec);
        m.layout = put(&rec.layout, 1).ptr;
        m.majorant = put(rec.majorant).ptr;
        m.extinctionMajorant = put(rec.extinctionMajorant).ptr;
        const Distribution1DView cells = rec.lightCells.view();
        m.lightCells.func = put(cells.func, cells.count).ptr;
        m.lightCells.cdf = put(cells.cdf, cells.count ? size_t(cells.count) + 1 : 0).ptr;
        if (const MediumShell *shell = rec.desc.shell.get()) { // a medium bounded by a surface (M11-P5)
            m.shell.triangles = put(shell->triangles).ptr;
            m.shell.nodes = put(shell->nodes).ptr;
        }
        g.medium = m;
        g.indexToObject = rec.indexToObject;
        g.objectToWorld = rec.objectToWorld;
        g.motionKeys = put(rec.motionKeys);
        g.motionSteps = rec.motionSteps;
        grids.clear();
        for (const VolumeField &f : rec.desc.fields) {
            const void *copy = nullptr;
            if (f.grid && f.grid->grid()) {
                const void *&known = gridCopies[f.grid->data()];
                if (!known)
                    known = put(static_cast<const uint8_t *>(f.grid->data()), f.grid->size()).ptr;
                copy = known;
            }
            grids.push_back(copy);
        }
        g.grids = put(grids);
        g.fieldObjectToIndex = put(rec.fieldObjectToIndex);
        g.reference = rec.reference;
        for (int c = 0; c < 3; ++c)
            g.velocity[c] = rec.velocity[size_t(c)];
        g.velocityTime[0] = rec.desc.velocityTime[0];
        g.velocityTime[1] = rec.desc.velocityTime[1];
        g.volumeLimit = rec.desc.volumeLimit;
        if (const shading::Program *p = rec.program) {
            if (!volumeShaderOnDevice(rec)) { // as the texture plan
                ++largeShaders; // the default volume stands in
                continue;
            }
            const MaterialRecord &mat = scene.material(rec.material);
            g.code = put(p->code).ptr;
            g.codeSize = uint32_t(p->code.size());
            g.uniforms = put(mat.volumeUniforms).ptr;
            g.globals = put(p->globals);
            g.primvars = put(p->primvarBindings);
            g.outputs = put(p->outputs);
            fieldBindings.clear();
            for (const std::array<int32_t, 4> &b : rec.bindings)
                fieldBindings.insert(fieldBindings.end(), b.begin(), b.end());
            g.bindings = put(fieldBindings).ptr;
            g.maskFields = put(rec.maskFields);
            g.constants = put(rec.constantValues).ptr;
            g.defaults = put(&mat.volume, 1).ptr;
            texSlots.clear();
            for (const TexSampler &s : mat.samplers)
                texSlots.push_back(textureSlot(m_sampler, s));
            g.textures = put(texSlots).ptr;
            g.textureCount = uint32_t(texSlots.size());
            g.program = 1;
        }
    }
    if (largeShaders > 0)
        std::fprintf(stderr, "[Nova] XPU: %u volume shaders need more than %u rows; the GPU renders them as density\n",
                     largeShaders, kGpuMaxRows);
    m_view.volumes = put(volumes).ptr;
    m_view.activeVolumes = put(scene.activeVolumes()).ptr;
    m_view.activeVolumeCount = uint32_t(scene.activeVolumes().size());
    m_view.levelDependent = uint8_t(scene.anyLevelDependentVolume());
    m_view.holdoutVolumes = uint8_t(scene.anyHoldoutVolume());
    m_view.backgroundVolumes = uint8_t(scene.anyBackgroundVolume());
    m_view.volumeShadowVisibility = uint8_t(scene.volumeShadowVisibilityDiffers());
    // The sky atmosphere (docs/design/M11-P7.md) with its altitude levels.
    m_view.atmosphere = AtmosphereView{};
    if (const AtmosphereView *atmosphere = scene.atmosphere()) {
        m_view.atmosphere = *atmosphere;
        m_view.atmosphere.levels = put(scene.atmosphereLevels()).ptr;
    }
    // Equiangular sampling (M9 D17): the lights that opted in.
    const Scene::EquiangularLights eq = scene.equiangularLights();
    m_view.eqLights = put(eq.lights, eq.table.count).ptr;
    m_view.eqTable.pmf = put(eq.table.pmf, eq.table.count).ptr;
    m_view.eqTable.threshold = put(eq.table.threshold, eq.table.count).ptr;
    m_view.eqTable.alias = put(eq.table.alias, eq.table.count).ptr;
    m_view.eqTable.count = eq.table.count;
    m_view.eqPmf = put(eq.pmf, scene.analyticLightCount()).ptr;
    m_view.tolerance = scene.intersectionTolerance();
    m_view.anyFiltered = uint8_t(scene.anyFiltered());
    m_view.nestedDielectrics = uint8_t(scene.nestedDielectrics());
    // Holdouts (core/integrator/holdout.h).
    m_view.anyHoldouts = uint8_t(scene.anyHoldouts());
    m_view.shadowVisibilityDiffers = uint8_t(scene.shadowVisibilityDiffers());
    m_view.illumBackground = 0;
    if (scene.anyBackground())
        for (uint32_t i = 0; i < scene.analyticLightCount(); ++i)
            m_view.illumBackground |= uint8_t(scene.lights()[i].illumBackground);
    if (m_failed || !m_device.sync())
        return false;
    m_scene = &scene;
    m_sceneVersion = scene.version();
    return true;
}

bool GpuRenderer::setFilm(const Film &film) {
    Device::Scope scope(m_device);
    m_width = film.width;
    m_height = film.height;
    const size_t n = size_t(film.width) * size_t(film.height);
    const UtilityPlane *ao = const_cast<Film &>(film).findUtility(UtilityKind::Ao);
    for (int plane = 0; plane < kOutlinePlaneCount; ++plane) { // Karma's outline planes
        const UtilityPlane *p = const_cast<Film &>(film).findUtility(UtilityKind(int(UtilityKind::Outline) + plane));
        if (p && !m_device.upload(m_outlinePlanes[plane], p->data))
            return false;
    }
    return m_device.upload(m_colorSum, film.colorSum.data(), n) && m_device.upload(m_alphaSum, film.alphaSum.data(), n) &&
           m_device.upload(m_count, film.count.data(), n) && (!ao || m_device.upload(m_aoPlane, ao->data)) &&
           m_device.upload(m_lpeSum, film.lpeSum) && m_device.upload(m_lpeLit, film.lpeLit) && m_device.sync();
}

uint32_t GpuRenderer::shadowSlots() const {
    // Pre-lit background holdouts with illumbackground lights use the walk
    // exits' kind (kernels.cu shadePrelit); the sky atmosphere's ground with
    // groundreceiveshadow has a kind of its own (shadeGround).
    return (m_mayWalk || m_view.illumBackground ? 3u : 2u) + (m_view.shadowDomeCount > 0 ? 1u : 0u) +
           (groundShadows() ? 1u : 0u);
}

bool GpuRenderer::reserveAll(uint32_t paths, const WaveNeeds &needs) {
    return reserveWave(paths) && reserveWalks(paths) && (!needs.lpe || reserveLpe(paths)) &&
           (!needs.media || reserveMedia(paths)) && (!needs.fake || reserveFake(paths)) &&
           (!needs.guide || reserveGuide(paths));
}

void GpuRenderer::releaseWave() {
    m_waveBuffers.clear();
    m_lpeWave.clear();
    m_mediaWave.clear();
    m_walkWave.clear();
    m_fakeWave.clear();
    m_guideWave.clear();
    m_capacity = m_shadowSlots = m_lpeCapacity = m_mediaCapacity = m_walkCapacity = m_fakeCapacity = m_guideCapacity = 0;
    m_lpeRayCapacity = 0;
    m_nestedReserved = false;
}

size_t GpuRenderer::waveBytes() const {
    size_t bytes = 0;
    for (const std::vector<DeviceBuffer> *group :
         {&m_waveBuffers, &m_lpeWave, &m_mediaWave, &m_walkWave, &m_fakeWave, &m_guideWave})
        for (const DeviceBuffer &b : *group)
            bytes += b.size();
    return bytes;
}

bool GpuRenderer::planWave(const WaveNeeds &needs) {
    // What a path costs depends on what the wave holds.
    const uint32_t key = 1u | (needs.lpe ? 2u : 0u) | (needs.media ? 4u : 0u) | (needs.fake ? 8u : 0u) |
                         (needs.guide ? 16u : 0u) | (m_mayWalk ? 32u : 0u) | (m_view.nestedDielectrics ? 64u : 0u) |
                         (shadowSlots() << 8);
    if (m_waveLimit != 0 && key == m_waveKey)
        return true;
    // The cost, measured on a small wave: the bytes of every buffer it holds.
    constexpr uint32_t kProbe = 4096;
    releaseWave();
    if (!reserveAll(kProbe, needs))
        return false;
    m_pathBytes = (waveBytes() + kProbe - 1) / kProbe;
    // Of the memory free now, with the scene, its textures and the film on
    // the device already, the kernels' stacks come off first: the driver
    // allocates them at the first launch, for every thread the device can
    // run at once. The waves take at most half the rest; the other half
    // stays for acceleration structures rebuilt in IPR, textures loaded
    // later and the denoiser.
    const size_t free = m_device.freeMemory() + waveBytes();
    const size_t stacks = m_launched ? 0 : size_t(std::max(m_device.processors(), 1)) * kStackBytesPerProcessor;
    const uint64_t fit = uint64_t((free > stacks ? free - stacks : 0) / 2) / std::max<size_t>(m_pathBytes, 1);
    m_waveLimit = uint32_t(std::clamp<uint64_t>(fit, kMinWavePaths, kMaxWavePaths));
    m_waveKey = key;
    if (m_waveLimit <= kMaxWavePaths / 2 && !m_waveNoted) { // waves that show in the render time
        m_waveNoted = true;
        std::fprintf(stderr, "[Nova] XPU: %zu MB of device memory free: waves of %u paths instead of %u\n", free >> 20,
                     m_waveLimit, kMaxWavePaths);
    }
    return true;
}

bool GpuRenderer::reserveWave(uint32_t paths) {
    const uint32_t slots = shadowSlots();
    const bool nested = m_view.nestedDielectrics != 0;
    if (paths <= m_capacity && slots <= m_shadowSlots && (!nested || m_nestedReserved))
        return true;
    m_waveBuffers.clear();
    bool ok = true;
    const auto field = [&](auto *&ptr, size_t count) {
        using T = std::remove_reference_t<decltype(*ptr)>;
        m_waveBuffers.emplace_back();
        ok = ok && m_waveBuffers.back().reserve(m_device, count * sizeof(T));
        ptr = m_waveBuffers.back().as<T>();
    };
    const auto rays = [&](GpuRays &r, uint32_t countSlot, size_t n) {
        field(r.org, n);
        field(r.dir, n);
        field(r.tMax, n);
        field(r.time, n);
        field(r.path, n);
        field(r.filterSeed, n);
        field(r.rayType, n);
        r.count = m_counts.as<uint32_t>() + countSlot;
        r.capacity = uint32_t(n);
    };
    GpuWave &w = m_wave;
    field(w.pixel, paths);
    field(w.beta, paths);
    field(w.radiance, paths);
    field(w.alpha, paths);
    field(w.pixelSeed, paths);
    field(w.sampleIndex, paths);
    field(w.bsdfPdf, paths);
    field(w.diffuseShare, paths);
    field(w.etaScale, paths);
    field(w.sigmaA, paths);
    field(w.inMedium, paths);
    // Only scenes with nested dielectrics carry the media (72 bytes a path).
    if (nested)
        field(w.nested, paths);
    else
        w.nested = nullptr;
    field(w.eventCounts, paths);
    field(w.limitDither, paths);
    field(w.lightOnly, paths);
    field(w.afterDiffuse, paths);
    field(w.classShare, paths);
    field(w.pathAlpha, paths);
    field(w.holdout, paths);
    field(w.coneWidth, paths);
    field(w.coneSpread, paths);
    field(w.prevP, paths);
    field(w.prevN, paths);
    field(w.prevReceiver, paths);
    field(w.prevSet, paths);
    field(w.hitMesh, paths);
    field(w.hitPrim, paths);
    field(w.hitUv, paths);
    field(w.hitInstance, paths);
    field(w.lightMesh, paths);
    field(w.lightHitT, paths);
    field(w.lightPrim, paths);
    field(w.lightUv, paths);
    field(w.lightInstance, paths);
    // Up to two light samples per path and depth: at the hit or volume
    // event, and an equiangular one along the ray's medium segment; with
    // subsurface walks a third at a walk's exit; with shadow-linked domes
    // one more, a dome's BSDF sample at the event or the exit (path.h); with
    // the atmosphere's ground shadowed one at the ground (one per path: the
    // shadow kernel's runs, kernel_types.h shadowSlot). A push past the
    // budget is dropped (pushRay).
    const size_t shadows = size_t(paths) * slots;
    rays(w.shadow, 2, shadows);
    field(w.shadowContribution, shadows);
    field(w.occluded, shadows);
    field(w.shadowCategory, shadows);
    field(w.shadowLight, shadows);
    field(w.shadowEmitter, shadows);
    field(w.shadowBary, shadows);
    field(w.shadowClamp, shadows);
    field(w.shadowSlot, shadows);
    field(w.ao, paths);
    rays(w.aoRays, 3, paths);
    field(w.aoOccluded, paths);
    field(w.aoBelow, paths);
    field(w.cryptoMesh, paths);
    field(w.cryptoPrim, paths);
    field(w.outline, paths);
    field(w.outlineMain, paths);
    field(w.outlineRadius, paths);
    field(w.outlineFlagged, paths);
    field(w.outlineTypes, paths);
    field(w.cryptoInstance, paths);
    field(w.camUv, paths);
    field(w.camT, paths);
    field(w.camEventT, paths);
    field(w.camOrg, paths);
    field(w.camDir, paths);
    field(w.camTime, paths);
    rays(m_rays[0], 0, paths);
    rays(m_rays[1], 1, paths);
    m_capacity = ok ? paths : 0;
    m_shadowSlots = ok ? slots : 0;
    m_nestedReserved = ok && nested;
    return ok;
}

bool GpuRenderer::uploadLpe(const LpeTables &tables, uint64_t generation) {
    const lpe::Program &prog = tables.program;
    // Generation 0: tables of unknown origin, uploaded every time.
    const uint64_t key[5] = {generation, prog.stateCount, tables.meshTag.size(), tables.lightTag.size(),
                             m_sceneVersion};
    if (generation != 0 && std::equal(key, key + 5, m_lpeKey) && m_lpe.program.next)
        return true;
    m_lpeBuffers.clear();
    bool ok = true;
    const auto copy = [&](const auto *data, size_t count) {
        using T = std::remove_const_t<std::remove_reference_t<decltype(*data)>>;
        if (!count || !data)
            return Span<T>{};
        m_lpeBuffers.emplace_back();
        ok = ok && m_device.upload(m_lpeBuffers.back(), data, count);
        return Span<T>{m_lpeBuffers.back().as<T>(), count};
    };
    LpeView v = tables.view();
    const size_t symbols = size_t(lpe::kTypeCount) * lpe::kScatterCount * prog.tagCount * prog.labelCount;
    v.program.next = copy(prog.next, size_t(prog.stateCount) * prog.classCount).ptr;
    v.program.symbolClass = copy(prog.symbolClass, symbols).ptr;
    v.program.outBegin = copy(prog.outBegin, size_t(prog.stateCount) + 1).ptr;
    v.program.outs = copy(prog.outs, prog.outBegin[prog.stateCount]).ptr;
    v.program.outputFlags = nullptr; // not read on the device
    v.meshTag = copy(v.meshTag.data(), v.meshTag.size());
    v.materialTag = copy(v.materialTag.data(), v.materialTag.size());
    v.volumeTag = copy(v.volumeTag.data(), v.volumeTag.size());
    v.lightTag = copy(v.lightTag.data(), v.lightTag.size());
    v.albedoAfter = copy(v.albedoAfter.data(), v.albedoAfter.size());
    v.plainBegin = copy(v.plainBegin.data(), v.plainBegin.size());
    v.unitBegin = copy(v.unitBegin.data(), v.unitBegin.size());
    v.litBegin = copy(v.litBegin.data(), v.litBegin.size());
    v.plainOuts = copy(v.plainOuts.data(), v.plainOuts.size());
    v.unitOuts = copy(v.unitOuts.data(), v.unitOuts.size());
    v.litSlots = copy(v.litSlots.data(), v.litSlots.size());
    v.unitShadow = copy(v.unitShadow.data(), v.unitShadow.size());
    if (!ok)
        return false;
    m_lpe = v;
    std::copy(key, key + 5, m_lpeKey);
    return true;
}

bool GpuRenderer::reserveLpe(uint32_t paths) {
    // Per shadow ray its parts' range, as many rays as the shadow queue holds
    // (reserveWave ran first: m_shadowSlots per path, 2 to 5 with walks,
    // shadow-linked domes and the atmosphere's ground); up to 4 parts per
    // shadow ray on average, more are
    // dropped (lpe_path.h).
    const size_t rays = size_t(paths) * m_shadowSlots;
    if (paths > m_lpeCapacity || rays > m_lpeRayCapacity) {
        m_lpeWave.clear();
        m_lpeWave.resize(6);
        const size_t parts = rays * 4;
        if (!m_lpeWave[0].reserve(m_device, paths * sizeof(lpe::State)) ||
            !m_lpeWave[1].reserve(m_device, rays * sizeof(uint32_t)) || !m_lpeWave[2].reserve(m_device, rays) ||
            !m_lpeWave[3].reserve(m_device, parts * sizeof(lpe::State)) ||
            !m_lpeWave[4].reserve(m_device, parts * sizeof(Spectrum)) ||
            !m_lpeWave[5].reserve(m_device, parts * sizeof(Spectrum))) {
            m_lpeCapacity = 0;
            m_lpeRayCapacity = 0;
            return false;
        }
        m_lpeCapacity = paths;
        m_lpeRayCapacity = rays;
    }
    GpuWave &w = m_wave;
    w.lpeState = m_lpeWave[0].as<lpe::State>();
    w.partBegin = m_lpeWave[1].as<uint32_t>();
    w.partCount = m_lpeWave[2].as<uint8_t>();
    w.partState = m_lpeWave[3].as<lpe::State>();
    w.partValue = m_lpeWave[4].as<Spectrum>();
    w.partUnit = m_lpeWave[5].as<Spectrum>();
    w.partUsed = m_counts.as<uint32_t>() + 4;
    w.partCapacity = uint32_t(m_lpeRayCapacity * 4);
    return true;
}

bool GpuRenderer::reserveMedia(uint32_t paths) {
    if (paths > m_mediaCapacity) {
        m_mediaWave.clear();
        m_mediaWave.resize(16);
        const size_t n = paths, c = size_t(paths) * kGpuCrossings;
        const size_t sizes[16] = {n * sizeof(Spectrum), n * sizeof(Spectrum), n * sizeof(Spectrum), n,
                                  n * sizeof(float),    n * sizeof(uint32_t), n * sizeof(PhaseFunction), n,
                                  c * sizeof(uint32_t), c * sizeof(float),    c * sizeof(Spectrum),
                                  n * sizeof(uint32_t), sizeof(uint32_t),     n * sizeof(float),
                                  n * sizeof(float),    n * sizeof(Spectrum)};
        for (int i = 0; i < 16; ++i)
            if (!m_mediaWave[size_t(i)].reserve(m_device, sizes[i])) {
                m_mediaCapacity = 0;
                return false;
            }
        m_mediaCapacity = paths;
    }
    GpuWave &w = m_wave;
    w.mediumT = m_mediaWave[0].as<Spectrum>();
    w.mediumAlpha = m_mediaWave[1].as<Spectrum>();
    w.lightT = m_mediaWave[2].as<Spectrum>();
    w.volumeHit = m_mediaWave[3].as<uint8_t>();
    w.volumeT = m_mediaWave[4].as<float>();
    w.volumeId = m_mediaWave[5].as<uint32_t>();
    w.volumePhase = m_mediaWave[6].as<PhaseFunction>();
    w.crossCount = m_mediaWave[7].as<uint8_t>();
    w.crossLight = m_mediaWave[8].as<uint32_t>();
    w.crossT = m_mediaWave[9].as<float>();
    w.crossAtten = m_mediaWave[10].as<Spectrum>();
    w.volumeShade = m_mediaWave[11].as<uint32_t>();
    w.volumeShadeCount = m_mediaWave[12].as<uint32_t>();
    w.eqA = m_mediaWave[13].as<float>();
    w.eqB = m_mediaWave[14].as<float>();
    w.mediumAlphaV = m_mediaWave[15].as<Spectrum>();
    return true;
}

bool GpuRenderer::reserveWalks(uint32_t paths) {
    GpuWalks &k = m_wave.walks;
    k = GpuWalks{};
    k.count = m_counts.as<uint32_t>() + 7;
    if (!m_mayWalk)
        return true;
    constexpr int kFields = 17;
    if (paths > m_walkCapacity) {
        m_walkWave.clear();
        m_walkWave.resize(kFields);
        const size_t n = paths;
        const size_t sizes[kFields] = {4, 4, 4, 4, 4, 4, sizeof(Vec3f), sizeof(Vec3f), sizeof(SubsurfaceLobe), 1,
                                       sizeof(Spectrum), sizeof(Vec3f), sizeof(Vec3f), 4, 4, sizeof(Vec2f), 4};
        for (int i = 0; i < kFields; ++i)
            if (!m_walkWave[size_t(i)].reserve(m_device, n * sizes[i])) {
                m_walkCapacity = 0;
                return false;
            }
        m_walkCapacity = paths;
    }
    k.ray = m_walkWave[0].as<uint32_t>();
    k.path = m_walkWave[1].as<uint32_t>();
    k.time = m_walkWave[2].as<float>();
    k.mesh = m_walkWave[3].as<uint32_t>();
    k.instance = m_walkWave[4].as<uint32_t>();
    k.hitT = m_walkWave[5].as<float>();
    k.P = m_walkWave[6].as<Vec3f>();
    k.ng = m_walkWave[7].as<Vec3f>();
    k.medium = m_walkWave[8].as<SubsurfaceLobe>();
    k.exited = m_walkWave[9].as<uint8_t>();
    k.weight = m_walkWave[10].as<Spectrum>();
    k.org = m_walkWave[11].as<Vec3f>();
    k.dir = m_walkWave[12].as<Vec3f>();
    k.t = m_walkWave[13].as<float>();
    k.prim = m_walkWave[14].as<uint32_t>();
    k.uv = m_walkWave[15].as<Vec2f>();
    k.entry = m_walkWave[16].as<float>();
    k.capacity = m_walkCapacity;
    k.pixelSeed = m_wave.pixelSeed;
    k.sampleIndex = m_wave.sampleIndex;
    k.meshes = m_view.meshes;
    return true;
}

bool GpuRenderer::reserveFake(uint32_t paths) {
    // Per shadow ray its transmittance, length left and a crossing's hit;
    // two queues of them.
    const size_t n = size_t(paths) * m_shadowSlots;
    constexpr int kFields = 6 + 2 * 7;
    if (n > m_fakeCapacity) {
        m_fakeWave.clear();
        m_fakeWave.resize(kFields);
        const size_t queue[7] = {sizeof(Vec3f), sizeof(Vec3f), sizeof(float), sizeof(float),
                                 sizeof(uint32_t), sizeof(float), sizeof(uint8_t)};
        const size_t sizes[6] = {sizeof(Spectrum), sizeof(float), sizeof(uint32_t),
                                 sizeof(uint32_t), sizeof(Vec2f), sizeof(uint32_t)};
        for (int i = 0; i < kFields; ++i)
            if (!m_fakeWave[size_t(i)].reserve(m_device, n * (i < 6 ? sizes[i] : queue[(i - 6) % 7]))) {
                m_fakeCapacity = 0;
                return false;
            }
        m_fakeCapacity = uint32_t(n);
    }
    GpuWave &w = m_wave;
    w.fakeT = m_fakeWave[0].as<Spectrum>();
    w.fakeLeft = m_fakeWave[1].as<float>();
    w.fakeMesh = m_fakeWave[2].as<uint32_t>();
    w.fakePrim = m_fakeWave[3].as<uint32_t>();
    w.fakeUv = m_fakeWave[4].as<Vec2f>();
    w.fakeInstance = m_fakeWave[5].as<uint32_t>();
    for (int q = 0; q < 2; ++q) {
        const DeviceBuffer *b = &m_fakeWave[size_t(6 + 7 * q)];
        GpuRays &r = m_fakeRays[q];
        r.org = b[0].as<Vec3f>();
        r.dir = b[1].as<Vec3f>();
        r.tMax = b[2].as<float>();
        r.time = b[3].as<float>();
        r.path = b[4].as<uint32_t>();
        r.filterSeed = b[5].as<float>();
        r.rayType = b[6].as<uint8_t>();
        r.count = m_counts.as<uint32_t>() + 5 + q;
        r.capacity = m_fakeCapacity;
    }
    return true;
}

bool GpuRenderer::uploadGuide(const PathGuide &guide) {
    if (guide.generation() == m_guideGeneration)
        return true;
    // Every array anew; the building sums and leaf counts start at zero (the
    // host holds what the CPU recorded and adds the device's at the end).
    m_guideBuffers.clear();
    m_guideBuffers.resize(7);
    GuideView v;
    bool ok = true;
    const auto put = [&](size_t i, const auto &vec) {
        using T = typename std::decay_t<decltype(vec)>::value_type;
        if (vec.empty())
            return static_cast<T *>(nullptr);
        if (!m_device.upload(m_guideBuffers[i], vec)) {
            ok = false;
            return static_cast<T *>(nullptr);
        }
        return m_guideBuffers[i].template as<T>();
    };
    const auto zeros = [&](size_t i, size_t count) -> uint64_t * {
        if (!count)
            return nullptr;
        DeviceBuffer &b = m_guideBuffers[i];
        if (!b.reserve(m_device, count * sizeof(uint64_t)) ||
            !m_device.check(m_device.cu().p_cuMemsetD8Async(b.ptr(), 0, count * sizeof(uint64_t), m_device.stream()),
                            "cuMemsetD8Async")) {
            ok = false;
            return nullptr;
        }
        return b.as<uint64_t>();
    };
    v.spatial = put(0, guide.spatial);
    v.leaves = put(1, guide.leaves);
    v.nodes = put(2, guide.nodes);
    v.buildChild = put(3, guide.buildChild);
    v.buildLeaf = put(4, guide.buildLeaf);
    v.buildSum = zeros(5, guide.buildSum.size());
    v.leafCount = zeros(6, guide.leafCount.size());
    if (!ok) {
        m_guideBuffers.clear();
        m_guideGeneration = 0;
        return false;
    }
    m_guideView = v;
    m_guideGeneration = guide.generation();
    return true;
}

bool GpuRenderer::downloadGuide(PathGuide &guide) {
    if (guide.generation() != m_guideGeneration || m_guideBuffers.size() < 7)
        return true; // nothing of this generation on the device
    Device::Scope scope(m_device);
    std::vector<uint64_t> sums(guide.buildSum.size()), counts(guide.leafCount.size());
    if ((!sums.empty() && !m_device.download(sums.data(), m_guideBuffers[5].ptr(), sums.size() * sizeof(uint64_t))) ||
        (!counts.empty() && !m_device.download(counts.data(), m_guideBuffers[6].ptr(), counts.size() * sizeof(uint64_t))))
        return false;
    for (size_t i = 0; i < sums.size(); ++i)
        guide.buildSum[i] += sums[i];
    for (size_t i = 0; i < counts.size(); ++i)
        guide.leafCount[i] += counts[i];
    // The device's sums now live on the host: cleared for further passes.
    m_guideGeneration = 0;
    return true;
}

bool GpuRenderer::reserveGuide(uint32_t paths) {
    if (paths > m_guideCapacity) {
        m_guideWave.clear();
        m_guideWave.resize(5);
        const size_t n = paths, v = size_t(paths) * guide::kVertices;
        if (!m_guideWave[0].reserve(m_device, n) || !m_guideWave[1].reserve(m_device, n) ||
            !m_guideWave[2].reserve(m_device, v * sizeof(uint32_t)) ||
            !m_guideWave[3].reserve(m_device, v * sizeof(Spectrum)) ||
            !m_guideWave[4].reserve(m_device, v * sizeof(Spectrum))) {
            m_guideCapacity = 0;
            return false;
        }
        m_guideCapacity = paths;
    }
    GuidePaths &g = m_wave.guide;
    g.count = m_guideWave[0].as<uint8_t>();
    g.pending = m_guideWave[1].as<uint8_t>();
    g.slot = m_guideWave[2].as<uint32_t>();
    g.weight = m_guideWave[3].as<Spectrum>();
    g.snap = m_guideWave[4].as<Spectrum>();
    return true;
}

bool GpuRenderer::launch(CUfunction fn, uint32_t threads, void *args) {
    if (threads == 0)
        return true;
    void *params[] = {args};
    return m_device.check(m_device.cu().p_cuLaunchKernel(fn, (threads + kBlock - 1) / kBlock, 1, 1, kBlock, 1, 1, 0,
                                                         m_device.stream(), params, nullptr),
                          "cuLaunchKernel");
}

bool GpuRenderer::renderPass(const Camera &camera, const RenderSettings &settings, const PixelFilter &filter,
                             const TileRect &region, uint32_t spp, Film &film, uint64_t &rays,
                             const LpeTables *lpe, const std::vector<uint32_t> *list, uint64_t lpeGeneration,
                             const PathGuide *guide) {
    if (!m_scene || film.width != m_width || film.height != m_height)
        return false;
    Device::Scope scope(m_device);
    const CudaApi &cu = m_device.cu();
    const CUstream stream = m_device.stream();
    if (!m_device.upload(m_camera, &camera, 1) || !m_device.upload(m_filter, &filter, 1))
        return false;
    GpuSettings gs;
    gs.maxDepth = settings.maxDepth;
    gs.rrStartDepth = settings.rrStartDepth;
    gs.clampIndirect = settings.clampIndirect;
    gs.clampDirect = settings.clampDirect;
    gs.clampEmissionIndirect = uint8_t(settings.clampEmissionIndirect);
    gs.constrainRoughness = uint8_t(settings.constrainRoughness);
    gs.lightStrategy = settings.lightStrategy;
    gs.limits = settings.limits;
    gs.globals = shadeGlobals(settings, camera); // the frame, and the camera for NDC positions
    gs.textureFilterWidth = settings.textureFilterWidth;
    UtilityPlane *aoPlane = film.findUtility(UtilityKind::Ao);
    gs.ao = aoPlane != nullptr;
    gs.crypto = !film.crypto.empty();
    gs.outline = settings.outline; // Karma's outlines and look-dev overrides (M11 P9a)
    gs.look = settings.look;
    const bool outlines = outlineOn(settings.outline);
    gs.records = film.wantsSampleRecords();
    // The camera cone of each 32 x 32 tile of the region at its center, as
    // the CPU's tiles (Renderer::buildTiles, generateCameraRays).
    const int tilesX = (region.width() + 31) / 32, tilesY = (region.height() + 31) / 32;
    std::vector<Vec2f> cones(size_t(std::max(tilesX * tilesY, 1)));
    for (int ty = 0; ty < tilesY; ++ty)
        for (int tx = 0; tx < tilesX; ++tx) {
            const int x0 = region.x0 + tx * 32, y0 = region.y0 + ty * 32;
            const int x1 = std::min(x0 + 32, region.x1), y1 = std::min(y0 + 32, region.y1);
            Vec2f &c = cones[size_t(ty) * size_t(tilesX) + size_t(tx)];
            cameraCone(camera,
                       {float(x0 + (x1 - x0) / 2 + camera.imageOriginX) + 0.5f,
                        float(y0 + (y1 - y0) / 2 + camera.imageOriginY) + 0.5f},
                       c.x, c.y);
        }
    if (!m_device.upload(m_tileCones, cones))
        return false;
    GpuFilm gf;
    gf.colorSum = m_colorSum.as<Spectrum>();
    gf.alphaSum = m_alphaSum.as<float>();
    gf.count = m_count.as<uint32_t>();
    gf.ao = aoPlane ? m_aoPlane.as<float>() : nullptr;
    for (int plane = 0; plane < kOutlinePlaneCount; ++plane)
        gf.outline[plane] = film.findUtility(UtilityKind(int(UtilityKind::Outline) + plane)) &&
                                    m_outlinePlanes[plane].size() > 0
                                ? m_outlinePlanes[plane].as<float>()
                                : nullptr;
    gf.width = film.width;
    // Light path expressions: the tables, the device film's sums.
    const bool lpeOn = lpe && film.lpeOutputs > 0;
    GpuScene view = m_view;
    if (lpeOn) {
        if (!uploadLpe(*lpe, lpeGeneration))
            return false;
        view.lpe = m_lpe;
        gf.lpeSum = m_lpeSum.as<Spectrum>();
        gf.lpeLit = m_lpeLit.as<Spectrum>();
        gf.pixels = size_t(film.width) * size_t(film.height);
    }
    view.useTree = uint8_t(settings.lightTree);
    // Path guiding: the field's device copy with this pass's flags (sampling,
    // training) and record scale.
    bool guideTraining = false;
    view.guide = GuideView{};
    if (guide) {
        if (!uploadGuide(*guide))
            return false;
        const GuideView host = guide->view();
        view.guide = m_guideView;
        view.guide.origin = host.origin;
        view.guide.size = host.size;
        view.guide.recordScale = host.recordScale;
        view.guide.probability = host.probability;
        view.guide.filter = host.filter;
        view.guide.components = host.components;
        view.guide.sampling = uint8_t(host.sampling && m_guideView.nodes);
        view.guide.training = uint8_t(host.training && m_guideView.buildChild && m_guideView.buildSum);
        guideTraining = view.guide.training != 0;
    }
    // The any-hit filter's view of the scene (filtered meshes only).
    const GpuFilter hitFilter{view, gs.globals};
    if (!m_device.upload(m_hitFilter, &hitFilter, 1))
        return false;
    const void *anyHit = view.anyFiltered ? m_hitFilter.as<GpuFilter>() : nullptr;
    const auto setCount = [&](const GpuRays &r, uint32_t n) {
        return m_device.check(cu.p_cuMemsetD32Async(CUdeviceptr(r.count), n, 1, stream), "cuMemsetD32Async");
    };

    if (list && !list->empty() && !m_device.upload(m_pixelList, *list))
        return false;
    const uint32_t regionPixels = list ? uint32_t(list->size()) : uint32_t(region.pixelCount());
    const bool media = m_view.anyMedia();
    const bool fake = m_scene->anyFakeCaustics();
    // A wave holds whole pixels (all `spp` samples of each), as many as the
    // device's memory allows (D16).
    const WaveNeeds needs{lpeOn, media, fake, guideTraining};
    if (!planWave(needs))
        return false;
    uint32_t wavePixels = std::max(1u, m_waveLimit / std::max(1u, spp));
    for (uint32_t first = 0; first < regionPixels; first += wavePixels) {
        uint32_t pixels = std::min(wavePixels, regionPixels - first), paths = pixels * spp;
        // Out of memory after all (the plan is from the first pass; an edit
        // or another program took memory since): waves of half the paths.
        while (!reserveAll(paths, needs)) {
            if (!m_device.outOfMemory() || pixels == 1 || paths <= kMinWavePaths)
                return false;
            releaseWave();
            m_device.forgetError();
            m_waveLimit = std::max(kMinWavePaths, std::min(m_waveLimit, paths) / 2);
            wavePixels = std::max(1u, m_waveLimit / std::max(1u, spp));
            pixels = std::min(wavePixels, regionPixels - first);
            paths = pixels * spp;
            std::fprintf(stderr, "[Nova] XPU: out of device memory: waves of %u paths\n", m_waveLimit);
        }
        if (!setCount(m_rays[0], paths))
            return false;
        if (!lpeOn)
            m_wave.lpeState = nullptr, m_wave.partBegin = nullptr;
        if (!guideTraining)
            m_wave.guide = GuidePaths{};
        if (!fake)
            m_wave.fakeT = nullptr;
        GenerateArgs g;
        g.wave = m_wave;
        g.rays = m_rays[0];
        g.film = gf;
        g.camera = m_camera.as<Camera>();
        g.filter = m_filter.as<PixelFilter>();
        g.x0 = region.x0;
        g.y0 = region.y0;
        g.width = region.width();
        g.firstPixel = first;
        g.pixels = list ? m_pixelList.as<uint32_t>() : nullptr;
        g.tileCones = m_tileCones.as<Vec2f>();
        g.tilesX = tilesX;
        g.pixelCount = pixels;
        g.spp = spp;
        g.seed = settings.seed;
        g.sampleOffset = film.sampleOffset;
        g.lpeStart = lpeOn ? lpe->program.start : lpe::State(0);
        g.filterSeeds = view.anyFiltered;
        g.records = gs.records;
        if (!launch(m_generate, paths, &g))
            return false;
        // Per depth the queue lengths come back after shading (12 bytes): the
        // launches cover the rays there are, and the wave ends when none are
        // left (deviation from D8: launches over the capacity cost 0.3 ms per
        // empty depth, most of a pass at the reference scenes' path lengths).
        int cur = 0;
        uint32_t count = paths;
        for (uint32_t depth = 0; depth <= settings.maxDepth && count > 0; ++depth) {
            const GpuRays &r = m_rays[cur], &next = m_rays[cur ^ 1];
            TraceParams closest;
            closest.org = r.org;
            closest.dir = r.dir;
            closest.tMax = r.tMax;
            closest.time = r.time;
            closest.count = r.count;
            closest.mesh = m_wave.hitMesh;
            closest.prim = m_wave.hitPrim;
            closest.uv = m_wave.hitUv;
            closest.instance = m_wave.hitInstance;
            closest.filter = anyHit;
            closest.seed = r.filterSeed;
            closest.rayType = r.rayType;
            closest.depth = depth;
            // Where scattered rays find their lights when shadow visibility
            // differs from theirs (intersectRays): the rays as shadow rays,
            // traced first (the closest query overwrites tMax).
            const bool lightQuery = depth > 0 && view.shadowVisibilityDiffers;
            if (lightQuery) {
                TraceParams light = closest;
                light.mesh = m_wave.lightMesh;
                light.prim = m_wave.lightPrim;
                light.uv = m_wave.lightUv;
                light.instance = m_wave.lightInstance;
                light.hitT = m_wave.lightHitT;
                light.shadowClass = 1;
                if (!m_accel.launch(Raygen::Closest, light, count))
                    return false;
                rays += count;
            }
            if (!m_accel.launch(Raygen::Closest, closest, count) || !setCount(next, 0) ||
                !setCount(m_wave.shadow, 0) || !setCount(m_wave.aoRays, 0) ||
                !m_device.check(cu.p_cuMemsetD32Async(CUdeviceptr(m_wave.walks.count), 0, 1, stream),
                                "cuMemsetD32Async") ||
                (lpeOn && !m_device.check(cu.p_cuMemsetD32Async(CUdeviceptr(m_wave.partUsed), 0, 1, stream),
                                          "cuMemsetD32Async")))
                return false;
            if (outlines && depth == 0) {
                // Karma's outlines (traceOutlines): the camera hits, then per
                // stencil point its rays (in aoRays, their hits in the light
                // arrays, free at depth 0) and the types they flag.
                OutlineArgs o;
                o.scene = view;
                o.settings = gs;
                o.wave = m_wave;
                o.rays = r;
                if (!launch(m_outlineMain, count, &o))
                    return false;
                const uint32_t points = outlinePointCount(settings.outline);
                for (uint32_t point = 0; point < points; ++point) {
                    o.point = point;
                    TraceParams stencil;
                    stencil.org = m_wave.aoRays.org;
                    stencil.dir = m_wave.aoRays.dir;
                    stencil.tMax = m_wave.aoRays.tMax;
                    stencil.time = m_wave.aoRays.time;
                    stencil.count = m_wave.aoRays.count;
                    stencil.mesh = m_wave.lightMesh;
                    stencil.prim = m_wave.lightPrim;
                    stencil.uv = m_wave.lightUv;
                    stencil.instance = m_wave.lightInstance;
                    stencil.hitT = m_wave.lightHitT;
                    stencil.filter = anyHit;
                    stencil.seed = m_wave.aoRays.filterSeed;
                    stencil.rayType = m_wave.aoRays.rayType;
                    stencil.depth = 0;
                    if (!setCount(m_wave.aoRays, 0) || !launch(m_outlineRays, count, &o) ||
                        !m_accel.launch(Raygen::Closest, stencil, count) || !launch(m_outlineFlags, count, &o))
                        return false;
                }
                rays += uint64_t(count) * points; // at most
                if (!launch(m_outlineFinish, count, &o) || !setCount(m_wave.aoRays, 0))
                    return false;
            }
            ShadeArgs s;
            s.scene = view;
            s.film = gf;
            s.settings = gs;
            s.wave = m_wave;
            if (!lightQuery)
                s.wave.lightMesh = nullptr;
            s.rays = r;
            s.next = next;
            s.depth = depth;
            // Path guiding: the last bounce's vertices take their snapshots.
            if (guideTraining && depth > 0 && !launch(m_guideSnap, count, &s))
                return false;
            // Volumes: the medium along each ray first, the scattering events
            // after the surfaces (both append shadow rays and continuations).
            if (media && (!m_device.check(cu.p_cuMemsetD32Async(CUdeviceptr(m_wave.volumeShadeCount), 0, 1, stream),
                                          "cuMemsetD32Async") ||
                          !launch(m_media, count, &s)))
                return false;
            uint32_t counts[8] = {};
            if (!launch(m_shade, count, &s) || (media && !launch(m_volumes, count, &s)) ||
                !m_device.download(counts, m_counts.ptr(), sizeof(counts)))
                return false;
            // Subsurface walks (pass C''): run through their meshes, then
            // their exits shaded (light samples and continuations: the
            // counts again). Walks where none were expected end their paths
            // in this pass; the next makes room.
            const uint32_t walks = counts[7];
            const bool walked = walks > 0 && m_wave.walks.capacity > 0;
            if (walks > 0 && !walked) {
                std::fprintf(stderr, "[Nova] XPU: subsurface walks from a material not scanned for them\n");
                m_mayWalk = true;
            }
            if (walked) {
                GpuWalks wk = m_wave.walks;
                wk.depth = depth;
                TraceParams walk;
                walk.count = m_wave.walks.count;
                if (!m_device.upload(m_walkParams, &wk, 1))
                    return false;
                walk.walks = m_walkParams.as<GpuWalks>();
                if (!m_accel.launch(Raygen::Walk, walk, walks) || !launch(m_walkExit, walks, &s) ||
                    !m_device.download(counts, m_counts.ptr(), sizeof(counts)))
                    return false;
                rays += walks;
            }
            const uint32_t shadows = counts[2];
            rays += uint64_t(count) + shadows;
            TraceParams occluded;
            occluded.org = m_wave.shadow.org;
            occluded.dir = m_wave.shadow.dir;
            occluded.tMax = m_wave.shadow.tMax;
            occluded.time = m_wave.shadow.time;
            occluded.count = m_wave.shadow.count;
            occluded.occluded = m_wave.occluded;
            occluded.category = m_wave.shadowCategory;
            occluded.filter = anyHit;
            occluded.seed = m_wave.shadow.filterSeed;
            occluded.depth = depth;
            occluded.occlusion = 1;
            occluded.rayType = m_wave.shadow.rayType; // the hit filter's flags (holdout.h)
            ShadowArgs sh;
            sh.scene = view;
            sh.film = gf;
            sh.settings = gs;
            sh.wave = m_wave;
            sh.depth = depth;
            if (!m_accel.launch(Raygen::Occluded, occluded, shadows))
                return false;
            if (m_scene->backgroundOccludesShadows() && m_scene->anyBackground() && shadows > 0) {
                // Karma's bgoccludeshadow (holdout.h): the rays from
                // background points that a background holdout occludes keep
                // their light (kRayBackgroundLit: fake caustics skip them).
                TraceParams probe = occluded;
                probe.category = nullptr;
                probe.probeBackground = 1;
                probe.backgroundLit = m_wave.shadow.rayType;
                if (!m_accel.launch(Raygen::Occluded, probe, shadows))
                    return false;
                rays += shadows;
            }
            if (fake && shadows > 0) {
                // Fake caustics (fakeCausticTransmittance): the unoccluded
                // rays cross the fake-caustic surfaces in front of the light,
                // the closest one per crossing (at most 8), as long as rays
                // are left (their count comes back).
                ShadeArgs fs = s;
                fs.next = m_fakeRays[0];
                if (!setCount(fs.next, 0) || !launch(m_fakeStart, shadows, &fs))
                    return false;
                uint32_t left = shadows;
                for (int crossing = 0, c = 0; crossing < 8 && left > 0; ++crossing, c ^= 1) {
                    const GpuRays &q = m_fakeRays[c];
                    TraceParams cross;
                    cross.org = q.org;
                    cross.dir = q.dir;
                    cross.tMax = q.tMax;
                    cross.time = q.time;
                    cross.count = q.count;
                    cross.mesh = m_wave.fakeMesh;
                    cross.prim = m_wave.fakePrim;
                    cross.uv = m_wave.fakeUv;
                    cross.instance = m_wave.fakeInstance;
                    cross.filter = anyHit;
                    cross.seed = q.filterSeed;
                    cross.rayType = q.rayType;
                    cross.depth = depth;
                    fs.rays = q;
                    fs.next = m_fakeRays[c ^ 1];
                    if (!m_accel.launch(Raygen::Closest, cross, left) || !setCount(fs.next, 0) ||
                        !launch(m_fakeStep, left, &fs) || !m_device.download(&left, CUdeviceptr(fs.next.count), 4))
                        return false;
                    rays += left;
                }
            }
            if (!launch(m_shadow, shadows, &sh))
                return false;
            if (view.eqTable.count > 0) { // the equiangular samples' shadow rays: a second run
                sh.slot = 1;
                if (!launch(m_shadow, shadows, &sh))
                    return false;
            }
            if (walked || view.illumBackground) { // the walk exits' (and pre-lit plates') shadow rays: a third run
                sh.slot = 2;
                if (!launch(m_shadow, shadows, &sh))
                    return false;
            }
            if (view.shadowDomeCount > 0) { // shadow-linked domes' BSDF samples: a fourth run
                sh.slot = 3;
                if (!launch(m_shadow, shadows, &sh))
                    return false;
            }
            if (groundShadows()) { // the sky atmosphere's ground's (shadeGround): a fifth run
                sh.slot = 4;
                if (!launch(m_shadow, shadows, &sh))
                    return false;
            }
            if (counts[3] > 0) { // the ambient occlusion AOV's rays (depth 0)
                TraceParams aoQuery;
                aoQuery.org = m_wave.aoRays.org;
                aoQuery.dir = m_wave.aoRays.dir;
                aoQuery.tMax = m_wave.aoRays.tMax;
                aoQuery.time = m_wave.aoRays.time;
                aoQuery.count = m_wave.aoRays.count;
                aoQuery.occluded = m_wave.aoOccluded;
                aoQuery.filter = anyHit;
                aoQuery.seed = m_wave.aoRays.filterSeed;
                aoQuery.depth = depth;
                aoQuery.occlusion = 1;
                aoQuery.skipFakeCaustics = 0; // AO: fake-caustic objects block it too (launch.h)
                AoArgs ao;
                ao.wave = m_wave;
                if (!m_accel.launch(Raygen::Occluded, aoQuery, counts[3]) || !launch(m_ao, counts[3], &ao))
                    return false;
                rays += counts[3];
            }
            cur ^= 1;
            count = counts[cur];
        }
        FlushArgs f;
        f.wave = m_wave;
        f.film = gf;
        f.pixelCount = pixels;
        f.spp = spp;
        f.outlines = uint8_t(outlines);
        f.outline = settings.outline;
        if (!launch(m_flush, pixels, &f))
            return false;
        if (guideTraining) { // the wave's records into the field's sums
            f.guide = view.guide;
            if (!launch(m_guideRecord, paths, &f))
                return false;
        }
        if (gs.crypto || gs.records) { // the camera hits, accumulated on the host (D9; M11 P6)
            CameraSampleRecords &r = m_records;
            r.reserve(paths);
            m_samplePixel.resize(paths);
            m_sampleIndex.resize(paths);
            const auto get = [&](auto &dst, auto *src) {
                return m_device.download(dst.data(), CUdeviceptr(src), paths * sizeof(dst[0]));
            };
            if (!get(r.mesh, m_wave.cryptoMesh) || !get(r.prim, m_wave.cryptoPrim) ||
                !get(r.instance, m_wave.cryptoInstance))
                return false;
            if (gs.records) {
                m_sampleRadiance.resize(paths);
                if (!get(r.uv, m_wave.camUv) || !get(r.t, m_wave.camT) || !get(r.eventT, m_wave.camEventT) ||
                    !get(r.org, m_wave.camOrg) || !get(r.dir, m_wave.camDir) || !get(r.time, m_wave.camTime) ||
                    !get(r.alpha, m_wave.alpha) || !get(m_sampleRadiance, m_wave.radiance) ||
                    !get(m_sampleIndex, m_wave.sampleIndex))
                    return false;
            }
            for (uint32_t slot = 0; slot < paths; ++slot) {
                const uint32_t pixel = first + slot / spp;
                const int x = region.x0 + int(pixel % uint32_t(region.width())), y = region.y0 + int(pixel / uint32_t(region.width()));
                const uint32_t pix = list ? (*list)[pixel] : uint32_t(y) * uint32_t(film.width) + uint32_t(x);
                m_samplePixel[slot] = pix;
                if (gs.crypto && r.mesh[slot] != kInvalidId)
                    accumulateCrypto(*m_scene, film, pix, r.mesh[slot], r.prim[slot], r.instance[slot]);
            }
            if (gs.records) {
                // The sample numbers the device drew with (novaGenerate: its
                // counts, which the host's only follow after the pass), the
                // film's own as the CPU's records take them (Film::sampleOffset).
                for (uint32_t slot = 0; slot < paths; ++slot)
                    m_sampleIndex[slot] -= film.sampleOffset;
                accumulateCameraSamples(*m_scene, camera, gs.globals, film,
                                        CameraSampleView::of(r, paths, m_samplePixel.data(), m_sampleIndex.data(),
                                                             m_sampleRadiance.data()));
            }
        }
    }

    // The host's counts as the flush made them (each rendered pixel spp
    // more); the sums stay on the device until downloadFilm().
    if (list) {
        for (uint32_t p : *list)
            film.count[p] += spp;
    } else {
        for (int y = region.y0; y < region.y1; ++y) {
            uint32_t *row = film.count.data() + size_t(y) * size_t(film.width);
            for (int x = region.x0; x < region.x1; ++x)
                row[x] += spp;
        }
    }
    if (!m_device.sync())
        return false;
    m_launched = true;
    return true;
}

bool GpuRenderer::downloadFilm(Film &film) {
    if (film.width != m_width || film.height != m_height)
        return false;
    Device::Scope scope(m_device);
    const size_t n = size_t(film.width) * size_t(film.height);
    UtilityPlane *ao = film.findUtility(UtilityKind::Ao);
    // The device buffers keep a larger allocation from an earlier film (the
    // current sums are their prefix, setFilm), so their capacity is tested.
    const bool lpe = !film.lpeSum.empty() && m_lpeSum.size() >= film.lpeSum.size() * sizeof(Spectrum) &&
                     m_lpeLit.size() >= film.lpeLit.size() * sizeof(Spectrum);
    for (int plane = 0; plane < kOutlinePlaneCount; ++plane) { // Karma's outline planes
        UtilityPlane *p = film.findUtility(UtilityKind(int(UtilityKind::Outline) + plane));
        if (p && m_outlinePlanes[plane].size() >= p->data.size() * sizeof(float) &&
            !m_device.download(p->data.data(), m_outlinePlanes[plane].ptr(), p->data.size() * sizeof(float)))
            return false;
    }
    return m_device.download(film.colorSum.data(), m_colorSum.ptr(), n * sizeof(Spectrum)) &&
           m_device.download(film.alphaSum.data(), m_alphaSum.ptr(), n * sizeof(float)) &&
           (!ao || m_aoPlane.size() < n * sizeof(float) ||
            m_device.download(ao->data.data(), m_aoPlane.ptr(), n * sizeof(float))) &&
           (!lpe || (m_device.download(film.lpeSum.data(), m_lpeSum.ptr(), film.lpeSum.size() * sizeof(Spectrum)) &&
                     m_device.download(film.lpeLit.data(), m_lpeLit.ptr(), film.lpeLit.size() * sizeof(Spectrum))));
}

} // namespace nova::gpu
