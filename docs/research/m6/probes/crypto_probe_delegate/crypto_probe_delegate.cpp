// Minimal Hydra render delegate "CryptoProbe" (research probe for M6, not part
// of Nova). It renders nothing; it answers three questions with husk:
//   0. Which names can a delegate derive for cryptomatte ids? Every mesh/
//      sphere/cube rprim logs its id, instancer, GetScenePrimPath() per
//      instance, the Hydra 2 primOrigin scenePath, the instancer's
//      instanceLocations, its material id and primvars.
//   1. Which AOV settings does a third-party delegate receive for a Karma
//      Cryptomatte RenderVar?  (printed to stderr per binding)
//   2. Does husk write extra planes and EXR metadata that a delegate hands over
//      through HdRenderBuffer::GetResource() as a UT_HUSDExtraAOVResource
//      (the Apache-2.0 header in $HFS/toolkit/include/UT)?
//
// For every binding whose AOV settings enable cryptomatte, the buffer:
//   - fills its own (preview) buffer with a constant colour,
//   - offers ceil(rank/2) extra RGBA float planes "<aovName>NN" holding
//     first quarter of the rows in memory: (id("/probe/objA"), 1.0, 0, 0, ...)
//     else left half:  (id("/probe/objA"), 0.75, id("/probe/objB"), 0.25, 0, 0, ...)
//          right half: (id("/probe/objB"), 1.0, 0, 0, ...)
//   - offers metadata cryptomatte/<key>/{name,hash,conversion,manifest} and a
//     non-cryptomatte key "probe:extra" (does husk filter it?).
// With CRYPTO_PROBE_BEAUTY_META=1 non-crypto buffers return a resource with no
// planes and the metadata key "cryptomatte/1234567/name" = "fromBeauty".
// With CRYPTO_PROBE_SIDECAR=1 crypto buffers also add "cryptomatte/<key>/manif_file".

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/primOriginSchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rendererCreateArgs.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/tokens.h>

#include <UT/UT_HUSDExtraAOVResource.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// MurmurHash3_x86_32 (public-domain algorithm by Austin Appleby), seed 0.
uint32_t murmur3(const std::string &s) {
    const uint8_t *data = reinterpret_cast<const uint8_t *>(s.data());
    const int len = int(s.size()), nblocks = len / 4;
    uint32_t h = 0;
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    for (int i = 0; i < nblocks; ++i) {
        uint32_t k;
        std::memcpy(&k, data + 4 * i, 4);
        k *= c1; k = (k << 15) | (k >> 17); k *= c2;
        h ^= k; h = (h << 13) | (h >> 19); h = h * 5 + 0xe6546b64;
    }
    const uint8_t *tail = data + 4 * nblocks;
    uint32_t k = 0;
    switch (len & 3) {
    case 3: k ^= uint32_t(tail[2]) << 16; [[fallthrough]];
    case 2: k ^= uint32_t(tail[1]) << 8; [[fallthrough]];
    case 1: k ^= tail[0]; k *= c1; k = (k << 15) | (k >> 17); k *= c2; h ^= k;
    }
    h ^= uint32_t(len);
    h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; h *= 0xc2b2ae35; h ^= h >> 16;
    return h;
}

uint32_t cryptoBits(const std::string &name) {
    uint32_t h = murmur3(name);
    const uint32_t exp = (h >> 23) & 0xff;
    if (exp == 0 || exp == 255)
        h ^= 1u << 23;
    return h;
}

float bitsToFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

std::string hex8(uint32_t u) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", u);
    return buf;
}

std::string valueString(VtValue const &v) {
    std::ostringstream os;
    os << v << " (" << v.GetTypeName() << ")";
    return os.str();
}

bool settingBool(HdAovSettingsMap const &m, const char *suffix, bool &found) {
    for (auto const &kv : m) {
        const std::string &k = kv.first.GetString();
        if (k.size() >= std::strlen(suffix) && k.compare(k.size() - std::strlen(suffix), std::string::npos, suffix) == 0) {
            found = true;
            if (kv.second.IsHolding<bool>()) return kv.second.UncheckedGet<bool>();
            if (kv.second.IsHolding<int>()) return kv.second.UncheckedGet<int>() != 0;
        }
    }
    return false;
}

int settingInt(HdAovSettingsMap const &m, const char *suffix, int fallback) {
    for (auto const &kv : m) {
        const std::string &k = kv.first.GetString();
        if (k.size() >= std::strlen(suffix) && k.compare(k.size() - std::strlen(suffix), std::string::npos, suffix) == 0 &&
            kv.second.IsHolding<int>())
            return kv.second.UncheckedGet<int>();
    }
    return fallback;
}

std::mutex g_log;
#define PROBE_LOG(...)                                \
    do {                                              \
        std::lock_guard<std::mutex> _l(g_log);        \
        std::fprintf(stderr, "[cryptoprobe] " __VA_ARGS__); \
        std::fflush(stderr);                          \
    } while (0)

} // namespace

// ---------------------------------------------------------------------------
class HdCryptoProbeRenderBuffer final : public HdRenderBuffer {
public:
    explicit HdCryptoProbeRenderBuffer(SdfPath const &id) : HdRenderBuffer(id) {}

    bool Allocate(GfVec3i const &dims, HdFormat format, bool multiSampled) override {
        _width = unsigned(dims[0]);
        _height = unsigned(dims[1]);
        _format = format;
        _data.assign(size_t(_width) * _height * HdDataSizeOfFormat(format), 0);
        PROBE_LOG("Allocate %s %ux%u format %d multiSampled %d\n", GetId().GetText(), _width, _height, int(format),
                  int(multiSampled));
        return true;
    }
    unsigned int GetWidth() const override { return _width; }
    unsigned int GetHeight() const override { return _height; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return false; }
    void *Map() override { ++_mapped; return _data.data(); }
    void Unmap() override { --_mapped; }
    bool IsMapped() const override { return _mapped != 0; }
    void Resolve() override {}
    bool IsConverged() const override { return true; }

    // Called from the render pass with the binding's settings.
    void Configure(TfToken const &aovName, HdAovSettingsMap const &settings, bool executed) {
        bool found = false;
        _crypto = settingBool(settings, "cryptomatte", found);
        _aovName = aovName.GetString();
        _rank = settingInt(settings, "cryptomatterank", 6);
        _executed = executed;
        if (!_crypto)
            return;
        const int layers = (_rank + 1) / 2;
        _extra.assign(size_t(layers), std::vector<float>(size_t(_width) * _height * 4, 0.0f));
        const float a = bitsToFloat(cryptoBits("/probe/objA")), b = bitsToFloat(cryptoBits("/probe/objB"));
        for (unsigned y = 0; y < _height; ++y)
            for (unsigned x = 0; x < _width; ++x) {
                float *p = &_extra[0][(size_t(y) * _width + x) * 4];
                if (y < _height / 4) { p[0] = a; p[1] = 1.0f; } // first rows in memory: row-order check
                else if (x < _width / 2) { p[0] = a; p[1] = 0.75f; p[2] = b; p[3] = 0.25f; }
                else                { p[0] = b; p[1] = 1.0f; }
            }
        // Preview/own buffer: a constant so it is recognisable.
        const size_t n = size_t(_width) * _height, comps = HdGetComponentCount(_format);
        if (HdGetComponentFormat(_format) == HdFormatFloat32) {
            float *f = reinterpret_cast<float *>(_data.data());
            for (size_t i = 0; i < n * comps; ++i)
                f[i] = 0.5f;
        }
    }

    VtValue GetResource(bool multiSampled) const override {
        static const bool beautyMeta = std::getenv("CRYPTO_PROBE_BEAUTY_META") != nullptr;
        PROBE_LOG("GetResource(%d) on %s (aov '%s', crypto %d, configured-in-execute %d)\n", int(multiSampled),
                  GetId().GetText(), _aovName.c_str(), int(_crypto), int(_executed));
        if (!_crypto && !beautyMeta)
            return VtValue();
        auto res = std::make_shared<UT_HUSDExtraAOVResource>();
        if (_crypto) {
            const auto *self = this;
            res->myMap = [self](int idx) -> void * {
                PROBE_LOG("map extra %d of %s\n", idx, self->_aovName.c_str());
                return const_cast<float *>(self->_extra[size_t(idx)].data());
            };
            res->myUnmap = [self](int idx) { PROBE_LOG("unmap extra %d of %s\n", idx, self->_aovName.c_str()); };
            for (size_t i = 0; i < _extra.size(); ++i) {
                char suffix[8];
                std::snprintf(suffix, sizeof suffix, "%02d", int(i));
                res->addPlane(_aovName + suffix, int(HdFormatFloat32Vec4));
            }
            const std::string key = "cryptomatte/" + hex8(murmur3(_aovName)).substr(0, 7) + "/";
            res->addMetadata(key + "name", _aovName);
            res->addMetadata(key + "hash", "MurmurHash3_32");
            res->addMetadata(key + "conversion", "uint32_to_float32");
            res->addMetadata(key + "manifest", "{\"/probe/objA\":\"" + hex8(cryptoBits("/probe/objA")) +
                                                   "\",\"/probe/objB\":\"" + hex8(cryptoBits("/probe/objB")) + "\"}");
            res->addMetadata("probe:extra", "non-cryptomatte key from " + _aovName);
            // With CRYPTO_PROBE_SIDECAR=1 also name a sidecar: does husk write it?
            if (std::getenv("CRYPTO_PROBE_SIDECAR"))
                res->addMetadata(key + "manif_file", "probe_" + _aovName + "_manifest.json");
        } else {
            res->myMap = [](int) -> void * { return nullptr; };
            res->myUnmap = [](int) {};
            res->addMetadata("cryptomatte/1234567/name", "fromBeauty");
            res->addMetadata("probe:beauty", "1");
        }
        HdAovSettingsMap map;
        map[TfToken("extra_aov_resource")] = VtValue(std::static_pointer_cast<void>(res));
        return VtValue(map);
    }

protected:
    void _Deallocate() override { _data.clear(); _extra.clear(); }

private:
    unsigned _width = 0, _height = 0;
    HdFormat _format = HdFormatInvalid;
    std::vector<uint8_t> _data;
    std::vector<std::vector<float>> _extra;
    std::atomic<int> _mapped{0};
    bool _crypto = false, _executed = false;
    int _rank = 6;
    std::string _aovName;
};

// ---------------------------------------------------------------------------
// Logs what a delegate can learn about the names Karma hashes: the rprim id,
// its instancer, the scene prim paths of its instances (GetScenePrimPath with
// the instancer context), the material id and the primvar names.
class HdCryptoProbeRprim final : public HdRprim {
public:
    HdCryptoProbeRprim(TfToken const &type, SdfPath const &id) : HdRprim(id), _type(type) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    TfTokenVector const &GetBuiltinPrimvarNames() const override { static TfTokenVector v; return v; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits, TfToken const &) override {
        _UpdateInstancer(sd, bits);
        HdInstancer::_SyncInstancerAndParents(sd->GetRenderIndex(), GetInstancerId());
        std::ostringstream os;
        os << "rprim " << _type << " " << GetId() << " instancer '" << GetInstancerId() << "' material '"
           << sd->GetMaterialId(GetId()) << "'\n";
        std::vector<int> indices;
        if (!GetInstancerId().IsEmpty()) {
            const VtIntArray idx = sd->GetInstanceIndices(GetInstancerId(), GetId());
            for (size_t i = 0; i < idx.size() && i < 4; ++i)
                indices.push_back(idx[i]);
            os << "    instance indices " << idx.size() << "\n";
        } else {
            indices.push_back(0);
        }
        for (int i : indices) {
            HdInstancerContext ctx;
            const SdfPath p = sd->GetScenePrimPath(GetId(), i, &ctx);
            os << "    GetScenePrimPath(" << i << ") = " << p;
            for (auto const &c : ctx)
                os << "  ctx(" << c.first << ", " << c.second << ")";
            os << "\n";
        }
        // Hydra 2: the terminal scene index knows the prim's origin and the
        // instancer's instance locations.
        if (HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex()) {
            const HdSceneIndexPrim prim = si->GetPrim(GetId());
            if (HdPrimOriginSchema origin = HdPrimOriginSchema::GetFromParent(prim.dataSource))
                os << "    primOrigin scenePath = " << origin.GetOriginPath(HdPrimOriginSchemaTokens->scenePath) << "\n";
            else
                os << "    no primOrigin\n";
            if (!GetInstancerId().IsEmpty()) {
                const HdSceneIndexPrim inst = si->GetPrim(GetInstancerId());
                if (HdInstancerTopologySchema topo = HdInstancerTopologySchema::GetFromParent(inst.dataSource)) {
                    if (HdPathArrayDataSourceHandle loc = topo.GetInstanceLocations()) {
                        const VtArray<SdfPath> paths = loc->GetTypedValue(0.0f);
                        os << "    instancer instanceLocations (" << paths.size() << "):";
                        for (size_t i = 0; i < paths.size() && i < 4; ++i)
                            os << " " << paths[i];
                        os << "\n";
                    } else {
                        os << "    instancer has no instanceLocations\n";
                    }
                    if (HdPathArrayDataSourceHandle protos = topo.GetPrototypes()) {
                        os << "    instancer prototypes:";
                        for (SdfPath const &p : protos->GetTypedValue(0.0f))
                            os << " " << p;
                        os << "\n";
                    }
                }
                if (HdPrimOriginSchema origin = HdPrimOriginSchema::GetFromParent(inst.dataSource))
                    os << "    instancer primOrigin scenePath = "
                       << origin.GetOriginPath(HdPrimOriginSchemaTokens->scenePath) << "\n";
            }
        }
        for (HdInterpolation interp : {HdInterpolationConstant, HdInterpolationUniform}) {
            for (HdPrimvarDescriptor const &d : sd->GetPrimvarDescriptors(GetId(), interp))
                os << "    primvar " << d.name << " interp " << int(interp) << " = " << valueString(sd->Get(GetId(), d.name))
                   << "\n";
        }
        PROBE_LOG("%s", os.str().c_str());
        *bits = HdChangeTracker::Clean;
    }

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &, HdDirtyBits *) override {}

private:
    TfToken _type;
};

class HdCryptoProbeInstancer final : public HdInstancer {
public:
    HdCryptoProbeInstancer(HdSceneDelegate *sd, SdfPath const &id) : HdInstancer(sd, id) {}
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits) override {
        std::ostringstream os;
        os << "instancer " << GetId() << " parent '" << GetParentId() << "'\n";
        for (HdInterpolation interp : {HdInterpolationInstance, HdInterpolationConstant})
            for (HdPrimvarDescriptor const &d : sd->GetPrimvarDescriptors(GetId(), interp))
                os << "    primvar " << d.name << " interp " << int(interp) << " = "
                   << valueString(sd->Get(GetId(), d.name)).substr(0, 160) << "\n";
        PROBE_LOG("%s", os.str().c_str());
        _UpdateInstancer(sd, bits);
        *bits = HdChangeTracker::Clean;
    }
};

// ---------------------------------------------------------------------------
class HdCryptoProbeRenderPass final : public HdRenderPass {
public:
    HdCryptoProbeRenderPass(HdRenderIndex *index, HdRprimCollection const &c) : HdRenderPass(index, c) {}
    bool IsConverged() const override { return _done; }

protected:
    void _Execute(HdRenderPassStateSharedPtr const &state, TfTokenVector const &) override {
        HdRenderPassAovBindingVector bindings = state->GetAovBindings();
        for (HdRenderPassAovBinding &b : bindings) {
            if (!b.renderBuffer && !b.renderBufferId.IsEmpty())
                b.renderBuffer = static_cast<HdRenderBuffer *>(
                    GetRenderIndex()->GetBprim(HdPrimTypeTokens->renderBuffer, b.renderBufferId));
            if (!_logged) {
                PROBE_LOG("binding aovName '%s' buffer %s clear %s\n", b.aovName.GetText(),
                          b.renderBufferId.GetText(), valueString(b.clearValue).c_str());
                std::map<std::string, std::string> sorted;
                for (auto const &kv : b.aovSettings)
                    sorted[kv.first.GetString()] = valueString(kv.second);
                for (auto const &kv : sorted)
                    PROBE_LOG("    %s = %s\n", kv.first.c_str(), kv.second.c_str());
            }
            if (auto *rb = dynamic_cast<HdCryptoProbeRenderBuffer *>(b.renderBuffer))
                rb->Configure(b.aovName, b.aovSettings, true);
        }
        _logged = true;
        _done = true;
    }

private:
    bool _logged = false, _done = false;
};

// ---------------------------------------------------------------------------
class HdCryptoProbeRenderDelegate final : public HdRenderDelegate {
public:
    HdCryptoProbeRenderDelegate() = default;
    explicit HdCryptoProbeRenderDelegate(HdRenderSettingsMap const &s) : HdRenderDelegate(s) {}

    const TfTokenVector &GetSupportedRprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->mesh, HdPrimTypeTokens->sphere, HdPrimTypeTokens->cube};
        return v;
    }
    const TfTokenVector &GetSupportedSprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->camera};
        return v;
    }
    const TfTokenVector &GetSupportedBprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->renderBuffer};
        return v;
    }
    HdResourceRegistrySharedPtr GetResourceRegistry() const override { return _registry; }
    HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex *index, HdRprimCollection const &c) override {
        return std::make_shared<HdCryptoProbeRenderPass>(index, c);
    }
    HdInstancer *CreateInstancer(HdSceneDelegate *sd, SdfPath const &id) override {
        return new HdCryptoProbeInstancer(sd, id);
    }
    void DestroyInstancer(HdInstancer *i) override { delete i; }
    HdRprim *CreateRprim(TfToken const &type, SdfPath const &id) override { return new HdCryptoProbeRprim(type, id); }
    void DestroyRprim(HdRprim *r) override { delete r; }
    HdSprim *CreateSprim(TfToken const &type, SdfPath const &id) override {
        return type == HdPrimTypeTokens->camera ? new HdCamera(id) : nullptr;
    }
    HdSprim *CreateFallbackSprim(TfToken const &type) override {
        return type == HdPrimTypeTokens->camera ? new HdCamera(SdfPath::EmptyPath()) : nullptr;
    }
    void DestroySprim(HdSprim *s) override { delete s; }
    HdBprim *CreateBprim(TfToken const &type, SdfPath const &id) override {
        return type == HdPrimTypeTokens->renderBuffer ? new HdCryptoProbeRenderBuffer(id) : nullptr;
    }
    HdBprim *CreateFallbackBprim(TfToken const &) override { return nullptr; }
    void DestroyBprim(HdBprim *b) override { delete b; }
    void CommitResources(HdChangeTracker *) override {}
    HdAovDescriptor GetDefaultAovDescriptor(TfToken const &name) const override {
        PROBE_LOG("GetDefaultAovDescriptor('%s')\n", name.GetText());
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
    }
    VtDictionary GetRenderStats() const override {
        VtDictionary d;
        d["percentDone"] = VtValue(100.0);
        return d;
    }

private:
    HdResourceRegistrySharedPtr _registry = std::make_shared<HdResourceRegistry>();
};

// ---------------------------------------------------------------------------
class HdCryptoProbeRendererPlugin final : public HdRendererPlugin {
public:
    HdRenderDelegate *CreateRenderDelegate() override { return new HdCryptoProbeRenderDelegate(); }
    HdRenderDelegate *CreateRenderDelegate(HdRenderSettingsMap const &s) override {
        PROBE_LOG("CreateRenderDelegate with %zu settings\n", s.size());
        std::map<std::string, std::string> sorted;
        for (auto const &kv : s) {
            const std::string &k = kv.first.GetString();
            if (k.rfind("karma:", 0) != 0) // skip the ~100 Karma settings
                sorted[k] = valueString(kv.second);
        }
        for (auto const &kv : sorted)
            PROBE_LOG("    setting %s = %.200s\n", kv.first.c_str(), kv.second.c_str());
        return new HdCryptoProbeRenderDelegate(s);
    }
    void DeleteRenderDelegate(HdRenderDelegate *d) override { delete d; }
    using HdRendererPlugin::IsSupported;
    bool IsSupported(HdRendererCreateArgs const &, std::string *) const override { return true; }
};

TF_REGISTRY_FUNCTION(TfType) {
    HdRendererPluginRegistry::Define<HdCryptoProbeRendererPlugin>();
}

PXR_NAMESPACE_CLOSE_SCOPE
