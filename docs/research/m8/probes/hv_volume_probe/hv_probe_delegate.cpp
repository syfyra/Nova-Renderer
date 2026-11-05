// Research probe for M8 (docs/research/m8/houdini-volumes.md), not part of Nova.
//
// Minimal HDK-free Hydra render delegate "VolumeProbe". It renders nothing; it
// logs to stderr what a third-party delegate receives for volumes:
//   - render settings whose name contains "volume" (Karma Render Settings LOP),
//   - every volume rprim: field bindings (GetVolumeFieldDescriptors), material
//     id, constant primvars (Render Geometry Settings), transform,
//   - every field bprim (openvdbAsset, houdiniFieldAsset, field3dAsset): the
//     scene delegate values filePath / fieldName / fieldIndex / fieldDataType,
//     the scene-index volumeField schema and the transform,
//   - every material sprim: terminals and node identifiers.
// With HV_PROBE_HELPER=<path to hv_volume_helper.dll> each field is also
// resolved through the HDK helper (LoadLibrary at run time, like Nova's
// nova_cvex.dll) and the grid statistics and NanoVDB size are logged.
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/field.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetwork2Interface.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/volume.h>
#include <pxr/imaging/hd/volumeFieldSchema.h>
#include <pxr/imaging/hd/volumeFieldBindingSchema.h>
#include <pxr/usd/sdf/assetPath.h>

#include "hv_volume_helper.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

std::mutex g_log;
#define PROBE_LOG(...)                                       \
    do {                                                     \
        std::lock_guard<std::mutex> _l(g_log);               \
        std::fprintf(stderr, "[volprobe] " __VA_ARGS__);     \
        std::fflush(stderr);                                 \
    } while (0)

std::string str(VtValue const &v, size_t maxlen = 300) {
    std::ostringstream os;
    if (v.IsHolding<SdfAssetPath>()) {
        SdfAssetPath const &a = v.UncheckedGet<SdfAssetPath>();
        os << "SdfAssetPath(asset='" << a.GetAssetPath() << "', resolved='" << a.GetResolvedPath() << "')";
    } else {
        os << v << " (" << v.GetTypeName() << ")";
    }
    std::string s = os.str();
    if (s.size() > maxlen)
        s = s.substr(0, maxlen) + "...";
    return s;
}

HvQueryFieldFn helper() {
    static HvQueryFieldFn fn = []() -> HvQueryFieldFn {
        const char *p = std::getenv("HV_PROBE_HELPER");
        if (!p)
            return nullptr;
        HMODULE m = LoadLibraryExA(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) {
            PROBE_LOG("cannot load helper %s (error %lu)\n", p, GetLastError());
            return nullptr;
        }
        return reinterpret_cast<HvQueryFieldFn>(GetProcAddress(m, "hvQueryField"));
    }();
    return fn;
}

} // namespace

// ---------------------------------------------------------------------------
class VolProbeField final : public HdField {
public:
    VolProbeField(TfToken const &type, SdfPath const &id) : HdField(id), _type(type) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits) override {
        std::ostringstream os;
        os << "field " << _type << " " << GetId() << " dirty 0x" << std::hex << *bits << std::dec << "\n";
        const VtValue fp = sd->Get(GetId(), HdFieldTokens->filePath);
        const VtValue fn = sd->Get(GetId(), HdFieldTokens->fieldName);
        const VtValue fi = sd->Get(GetId(), TfToken("fieldIndex"));
        const VtValue ft = sd->Get(GetId(), TfToken("fieldDataType"));
        const VtValue fc = sd->Get(GetId(), TfToken("fieldClass"));
        os << "    Get filePath = " << str(fp, 600) << "\n";
        os << "    Get fieldName = " << str(fn) << "\n";
        os << "    Get fieldIndex = " << str(fi) << "\n";
        os << "    Get fieldDataType = " << str(ft) << "\n";
        os << "    Get fieldClass = " << str(fc) << "\n";
        os << "    transform = " << sd->GetTransform(GetId()) << "\n";
        if (HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex()) {
            const HdSceneIndexPrim prim = si->GetPrim(GetId());
            os << "    scene index prim type '" << prim.primType << "' data sources:";
            if (prim.dataSource)
                for (TfToken const &n : prim.dataSource->GetNames())
                    os << " " << n;
            os << "\n";
            if (HdVolumeFieldSchema vf = HdVolumeFieldSchema::GetFromParent(prim.dataSource)) {
                if (auto d = vf.GetFilePath())
                    os << "    volumeField.filePath = " << str(VtValue(d->GetTypedValue(0.0f)), 600) << "\n";
                if (auto d = vf.GetFieldName())
                    os << "    volumeField.fieldName = " << d->GetTypedValue(0.0f) << "\n";
                if (auto d = vf.GetFieldIndex())
                    os << "    volumeField.fieldIndex = " << d->GetTypedValue(0.0f) << "\n";
                if (auto d = vf.GetFieldDataType())
                    os << "    volumeField.fieldDataType = " << d->GetTypedValue(0.0f) << "\n";
            } else {
                os << "    no volumeField schema\n";
            }
        }
        if (HvQueryFieldFn q = helper()) {
            std::string path;
            if (fp.IsHolding<SdfAssetPath>()) {
                SdfAssetPath const &a = fp.UncheckedGet<SdfAssetPath>();
                path = a.GetResolvedPath().empty() ? a.GetAssetPath() : a.GetResolvedPath();
            }
            const std::string name = fn.IsHolding<TfToken>() ? fn.UncheckedGet<TfToken>().GetString() : std::string();
            const int index = fi.IsHolding<int>() ? fi.UncheckedGet<int>() : -1;
            HvFieldInfo info;
            const int kind = q(path.c_str(), name.c_str(), index, _type == TfToken("houdiniFieldAsset") ? 1 : 0, &info);
            char buf[1024];
            std::snprintf(buf, sizeof buf,
                          "    helper: kind %d gt %d type %s class '%s' name '%s' active %lld bbox (%d %d %d)-(%d %d %d) "
                          "voxel %.4f range [%g, %g] origin (%.3f %.3f %.3f) nano %lld bytes lookup %.2f ms convert %.2f ms %s\n",
                          kind, info.gtType, info.valueType, info.gridClass, info.gridName, (long long)info.activeVoxels,
                          info.bboxMin[0], info.bboxMin[1], info.bboxMin[2], info.bboxMax[0], info.bboxMax[1],
                          info.bboxMax[2], info.voxelSize[0], info.valueMin, info.valueMax, info.indexToWorld[12],
                          info.indexToWorld[13], info.indexToWorld[14], (long long)info.nanoBytes, info.msLookup,
                          info.msConvert, info.error);
            os << buf;
        }
        PROBE_LOG("%s", os.str().c_str());
        *bits = Clean;
    }

private:
    TfToken _type;
};

// ---------------------------------------------------------------------------
class VolProbeVolume final : public HdVolume {
public:
    explicit VolProbeVolume(SdfPath const &id) : HdVolume(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits, TfToken const &) override {
        std::ostringstream os;
        os << "volume " << GetId() << " dirty 0x" << std::hex << *bits << std::dec << " material '"
           << sd->GetMaterialId(GetId()) << "' visible " << sd->GetVisible(GetId()) << "\n";
        for (HdVolumeFieldDescriptor const &d : sd->GetVolumeFieldDescriptors(GetId()))
            os << "    field binding '" << d.fieldName << "' -> " << d.fieldId << " (" << d.fieldPrimType << ")\n";
        os << "    transform = " << sd->GetTransform(GetId()) << "\n";
        os << "    extent = " << sd->GetExtent(GetId()) << "\n";
        for (HdInterpolation interp : {HdInterpolationConstant, HdInterpolationUniform, HdInterpolationVertex})
            for (HdPrimvarDescriptor const &d : sd->GetPrimvarDescriptors(GetId(), interp))
                os << "    primvar " << d.name << " interp " << int(interp) << " = " << str(sd->Get(GetId(), d.name), 160)
                   << "\n";
        PROBE_LOG("%s", os.str().c_str());
        *bits = HdChangeTracker::Clean;
    }
    TfTokenVector const &GetBuiltinPrimvarNames() const override {
        static TfTokenVector v;
        return v;
    }

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &, HdDirtyBits *) override {}
};

// ---------------------------------------------------------------------------
class VolProbeMaterial final : public HdMaterial {
public:
    explicit VolProbeMaterial(SdfPath const &id) : HdMaterial(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits) override {
        std::ostringstream os;
        os << "material " << GetId() << "\n";
        const VtValue res = sd->GetMaterialResource(GetId());
        if (res.IsHolding<HdMaterialNetworkMap>()) {
            HdMaterialNetworkMap const &map = res.UncheckedGet<HdMaterialNetworkMap>();
            for (auto const &kv : map.map) {
                std::map<std::string, int> ids;
                for (HdMaterialNode const &n : kv.second.nodes)
                    ++ids[n.identifier.GetString()];
                os << "    terminal '" << kv.first << "': " << kv.second.nodes.size() << " nodes, "
                   << kv.second.relationships.size() << " connections; ids:";
                for (auto const &i : ids)
                    os << " " << i.first << "x" << i.second;
                os << "\n";
                if (!kv.second.nodes.empty()) {
                    HdMaterialNode const &t = kv.second.nodes.back();
                    os << "    last node " << t.path << " (" << t.identifier << ") params:";
                    for (auto const &p : t.parameters)
                        os << " " << p.first << "=" << str(p.second, 60);
                    os << "\n";
                }
            }
            for (SdfPath const &t : map.terminals)
                os << "    terminal path " << t << "\n";
        } else {
            os << "    resource type " << res.GetTypeName() << "\n";
        }
        PROBE_LOG("%s", os.str().c_str());
        *bits = Clean;
    }
};

// ---------------------------------------------------------------------------
class VolProbeRenderBuffer final : public HdRenderBuffer {
public:
    explicit VolProbeRenderBuffer(SdfPath const &id) : HdRenderBuffer(id) {}
    bool Allocate(GfVec3i const &dims, HdFormat format, bool) override {
        _w = unsigned(dims[0]);
        _h = unsigned(dims[1]);
        _format = format;
        _data.assign(size_t(_w) * _h * HdDataSizeOfFormat(format), 0);
        return true;
    }
    unsigned int GetWidth() const override { return _w; }
    unsigned int GetHeight() const override { return _h; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return false; }
    void *Map() override { ++_mapped; return _data.data(); }
    void Unmap() override { --_mapped; }
    bool IsMapped() const override { return _mapped != 0; }
    void Resolve() override {}
    bool IsConverged() const override { return true; }

protected:
    void _Deallocate() override { _data.clear(); }

private:
    unsigned _w = 0, _h = 0;
    HdFormat _format = HdFormatInvalid;
    std::vector<uint8_t> _data;
    std::atomic<int> _mapped{0};
};

class VolProbeRenderPass final : public HdRenderPass {
public:
    VolProbeRenderPass(HdRenderIndex *index, HdRprimCollection const &c) : HdRenderPass(index, c) {}
    bool IsConverged() const override { return true; }

protected:
    void _Execute(HdRenderPassStateSharedPtr const &, TfTokenVector const &) override {
        if (!_logged) {
            PROBE_LOG("render pass executed\n");
            _logged = true;
        }
    }

private:
    bool _logged = false;
};

// ---------------------------------------------------------------------------
class VolProbeRenderDelegate final : public HdRenderDelegate {
public:
    VolProbeRenderDelegate() = default;
    explicit VolProbeRenderDelegate(HdRenderSettingsMap const &s) : HdRenderDelegate(s) {}

    const TfTokenVector &GetSupportedRprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->volume};
        return v;
    }
    const TfTokenVector &GetSupportedSprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->camera, HdPrimTypeTokens->material};
        return v;
    }
    const TfTokenVector &GetSupportedBprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->renderBuffer, TfToken("openvdbAsset"), TfToken("houdiniFieldAsset"),
                               TfToken("field3dAsset")};
        return v;
    }
    HdResourceRegistrySharedPtr GetResourceRegistry() const override { return _registry; }
    HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex *index, HdRprimCollection const &c) override {
        return std::make_shared<VolProbeRenderPass>(index, c);
    }
    HdInstancer *CreateInstancer(HdSceneDelegate *, SdfPath const &) override { return nullptr; }
    void DestroyInstancer(HdInstancer *i) override { delete i; }
    HdRprim *CreateRprim(TfToken const &type, SdfPath const &id) override {
        return type == HdPrimTypeTokens->volume ? new VolProbeVolume(id) : nullptr;
    }
    void DestroyRprim(HdRprim *r) override { delete r; }
    HdSprim *CreateSprim(TfToken const &type, SdfPath const &id) override {
        if (type == HdPrimTypeTokens->camera)
            return new HdCamera(id);
        if (type == HdPrimTypeTokens->material)
            return new VolProbeMaterial(id);
        return nullptr;
    }
    HdSprim *CreateFallbackSprim(TfToken const &type) override { return CreateSprim(type, SdfPath::EmptyPath()); }
    void DestroySprim(HdSprim *s) override { delete s; }
    HdBprim *CreateBprim(TfToken const &type, SdfPath const &id) override {
        if (type == HdPrimTypeTokens->renderBuffer)
            return new VolProbeRenderBuffer(id);
        if (id.IsEmpty())
            return nullptr;
        return new VolProbeField(type, id);
    }
    HdBprim *CreateFallbackBprim(TfToken const &) override { return nullptr; }
    void DestroyBprim(HdBprim *b) override { delete b; }
    void CommitResources(HdChangeTracker *) override {}
    HdAovDescriptor GetDefaultAovDescriptor(TfToken const &) const override {
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
    }
    TfTokenVector GetMaterialRenderContexts() const override {
        return {TfToken("kma"), TfToken("mtlx"), TfToken("")};
    }
    VtDictionary GetRenderStats() const override {
        VtDictionary d;
        d["percentDone"] = VtValue(100.0);
        return d;
    }

private:
    HdResourceRegistrySharedPtr _registry = std::make_shared<HdResourceRegistry>();
};

class HdVolumeProbeRendererPlugin final : public HdRendererPlugin {
public:
    HdRenderDelegate *CreateRenderDelegate() override { return new VolProbeRenderDelegate(); }
    HdRenderDelegate *CreateRenderDelegate(HdRenderSettingsMap const &s) override {
        std::map<std::string, std::string> sorted;
        for (auto const &kv : s) {
            const std::string &k = kv.first.GetString();
            if (k.find("olume") != std::string::npos || k.find("screendoor") != std::string::npos)
                sorted[k] = str(kv.second);
        }
        PROBE_LOG("CreateRenderDelegate with %zu settings; volume-related:\n", s.size());
        for (auto const &kv : sorted)
            PROBE_LOG("    setting %s = %s\n", kv.first.c_str(), kv.second.c_str());
        return new VolProbeRenderDelegate(s);
    }
    void DeleteRenderDelegate(HdRenderDelegate *d) override { delete d; }
    using HdRendererPlugin::IsSupported;
    bool IsSupported(HdRendererCreateArgs const &, std::string *) const override { return true; }
};

TF_REGISTRY_FUNCTION(TfType) {
    HdRendererPluginRegistry::Define<HdVolumeProbeRendererPlugin>();
}

PXR_NAMESPACE_CLOSE_SCOPE
