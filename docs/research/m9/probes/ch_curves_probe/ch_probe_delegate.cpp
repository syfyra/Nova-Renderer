// Research probe for M9 (docs/research/m9/curves-hair.md), not part of Nova.
//
// Minimal HDK-free Hydra render delegate "CurvesProbe". It renders nothing; it
// logs to stderr what a third-party delegate receives for curves and points:
//   - render settings whose name mentions curves, points, widths or bias,
//   - every basisCurves rprim: HdBasisCurvesTopology (type, basis, wrap,
//     vertex counts, indices), primvar descriptors of every interpolation with
//     role and a value summary, time samples of points / widths / normals
//     over the shutter, material id, transform, instancer id,
//   - the same prim through the scene index (Hydra 2): data source names and
//     the basisCurves topology schema,
//   - points and nurbsCurves rprims (if Hydra hands them over),
//   - every material: terminals and node identifiers.
// Built like docs/research/m8/probes/hv_volume_probe (CMakeLists.txt next to it).
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/basisCurves.h>
#include <pxr/imaging/hd/basisCurvesSchema.h>
#include <pxr/imaging/hd/basisCurvesTopologySchema.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/points.h>
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

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

std::mutex g_log;
#define PROBE_LOG(...)                                       \
    do {                                                     \
        std::lock_guard<std::mutex> _l(g_log);               \
        std::fprintf(stderr, "[curveprobe] " __VA_ARGS__);   \
        std::fflush(stderr);                                 \
    } while (0)

std::string str(VtValue const &v, size_t maxlen = 220) {
    std::ostringstream os;
    os << v << " (" << v.GetTypeName();
    if (v.IsArrayValued())
        os << ", " << v.GetArraySize() << " elements";
    os << ")";
    std::string s = os.str();
    if (s.size() > maxlen)
        s = s.substr(0, maxlen) + "...";
    return s;
}

const char *interpName(HdInterpolation i) {
    switch (i) {
    case HdInterpolationConstant: return "constant";
    case HdInterpolationUniform: return "uniform";
    case HdInterpolationVarying: return "varying";
    case HdInterpolationVertex: return "vertex";
    case HdInterpolationFaceVarying: return "faceVarying";
    case HdInterpolationInstance: return "instance";
    default: return "?";
    }
}

void logPrimvars(std::ostringstream &os, HdSceneDelegate *sd, SdfPath const &id) {
    for (int i = 0; i < HdInterpolationCount; ++i) {
        const HdInterpolation interp = HdInterpolation(i);
        for (HdPrimvarDescriptor const &d : sd->GetPrimvarDescriptors(id, interp)) {
            os << "    primvar " << d.name << " [" << interpName(interp) << ", role '" << d.role << "'"
               << (d.indexed ? ", indexed" : "") << "] = " << str(sd->Get(id, d.name)) << "\n";
            // Time samples over the shutter (motion blur): only the moving ones.
            HdTimeSampleArray<VtValue, 8> samples;
            sd->SamplePrimvar(id, d.name, &samples);
            if (samples.count > 1) {
                os << "        " << samples.count << " samples at";
                for (size_t k = 0; k < samples.count; ++k)
                    os << " " << samples.times[k];
                os << "\n";
            }
        }
    }
}

void logSceneIndex(std::ostringstream &os, HdSceneDelegate *sd, SdfPath const &id) {
    HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex();
    if (!si)
        return;
    const HdSceneIndexPrim prim = si->GetPrim(id);
    os << "    scene index prim type '" << prim.primType << "' data sources:";
    if (prim.dataSource)
        for (TfToken const &n : prim.dataSource->GetNames())
            os << " " << n;
    os << "\n";
    if (HdBasisCurvesSchema bc = HdBasisCurvesSchema::GetFromParent(prim.dataSource)) {
        if (HdBasisCurvesTopologySchema t = bc.GetTopology()) {
            os << "    basisCurves.topology:";
            if (auto d = t.GetType())
                os << " type=" << d->GetTypedValue(0.0f);
            if (auto d = t.GetBasis())
                os << " basis=" << d->GetTypedValue(0.0f);
            if (auto d = t.GetWrap())
                os << " wrap=" << d->GetTypedValue(0.0f);
            if (auto d = t.GetCurveVertexCounts())
                os << " counts=" << d->GetTypedValue(0.0f).size();
            os << "\n";
        }
    }
}

} // namespace

class ProbeCurves final : public HdBasisCurves {
public:
    explicit ProbeCurves(SdfPath const &id) : HdBasisCurves(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits, TfToken const &) override {
        std::ostringstream os;
        const HdBasisCurvesTopology topo = GetBasisCurvesTopology(sd);
        os << "basisCurves " << GetId() << " dirty 0x" << std::hex << *bits << std::dec << " material '"
           << sd->GetMaterialId(GetId()) << "' visible " << sd->GetVisible(GetId()) << " instancer '"
           << sd->GetInstancerId(GetId()) << "'\n";
        os << "    topology type=" << topo.GetCurveType() << " basis=" << topo.GetCurveBasis()
           << " wrap=" << topo.GetCurveWrap() << " curves=" << topo.GetNumCurves() << " points=" << topo.GetNumPoints()
           << " indices=" << topo.GetCurveIndices().size() << " counts=" << VtValue(topo.GetCurveVertexCounts()) << "\n";
        os << "    transform = " << sd->GetTransform(GetId()) << "\n";
        os << "    extent = " << sd->GetExtent(GetId()) << "\n";
        os << "    refineLevel = " << sd->GetDisplayStyle(GetId()).refineLevel << "\n";
        logPrimvars(os, sd, GetId());
        logSceneIndex(os, sd, GetId());
        PROBE_LOG("%s", os.str().c_str());
        *bits = HdChangeTracker::Clean;
    }

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &, HdDirtyBits *) override {}
};

class ProbePoints final : public HdPoints {
public:
    explicit ProbePoints(SdfPath const &id) : HdPoints(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits, TfToken const &) override {
        std::ostringstream os;
        os << "points " << GetId() << " material '" << sd->GetMaterialId(GetId()) << "'\n";
        logPrimvars(os, sd, GetId());
        logSceneIndex(os, sd, GetId());
        PROBE_LOG("%s", os.str().c_str());
        *bits = HdChangeTracker::Clean;
    }

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &, HdDirtyBits *) override {}
};

// Anything else (nurbsCurves, mesh): logged as a generic rprim.
class ProbeOther final : public HdRprim {
public:
    ProbeOther(TfToken const &type, SdfPath const &id) : HdRprim(id), _type(type) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits, TfToken const &) override {
        std::ostringstream os;
        os << "rprim '" << _type << "' " << GetId() << "\n";
        if (_type != HdPrimTypeTokens->mesh) {
            logPrimvars(os, sd, GetId());
            logSceneIndex(os, sd, GetId());
        }
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

private:
    TfToken _type;
};

class ProbeInstancer final : public HdInstancer {
public:
    ProbeInstancer(HdSceneDelegate *sd, SdfPath const &id) : HdInstancer(sd, id) {}
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits) override {
        std::ostringstream os;
        os << "instancer " << GetId() << "\n";
        logPrimvars(os, sd, GetId());
        PROBE_LOG("%s", os.str().c_str());
        _UpdateInstancer(sd, bits);
        *bits = HdChangeTracker::Clean;
    }
};

class ProbeMaterial final : public HdMaterial {
public:
    explicit ProbeMaterial(SdfPath const &id) : HdMaterial(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits) override {
        std::ostringstream os;
        os << "material " << GetId() << "\n";
        const VtValue res = sd->GetMaterialResource(GetId());
        if (res.IsHolding<HdMaterialNetworkMap>()) {
            HdMaterialNetworkMap const &map = res.UncheckedGet<HdMaterialNetworkMap>();
            for (auto const &kv : map.map) {
                os << "    terminal '" << kv.first << "': " << kv.second.nodes.size() << " nodes:";
                for (HdMaterialNode const &n : kv.second.nodes)
                    os << " " << n.identifier;
                os << "\n";
                if (!kv.second.nodes.empty()) {
                    HdMaterialNode const &t = kv.second.nodes.back();
                    os << "    last node " << t.path << " (" << t.identifier << ") params:";
                    for (auto const &p : t.parameters)
                        os << " " << p.first << "=" << str(p.second, 60);
                    os << "\n";
                }
            }
        }
        PROBE_LOG("%s", os.str().c_str());
        *bits = Clean;
    }
};

class ProbeRenderBuffer final : public HdRenderBuffer {
public:
    explicit ProbeRenderBuffer(SdfPath const &id) : HdRenderBuffer(id) {}
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

class ProbeRenderPass final : public HdRenderPass {
public:
    ProbeRenderPass(HdRenderIndex *index, HdRprimCollection const &c) : HdRenderPass(index, c) {}
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

class ProbeRenderDelegate final : public HdRenderDelegate {
public:
    ProbeRenderDelegate() = default;
    explicit ProbeRenderDelegate(HdRenderSettingsMap const &s) : HdRenderDelegate(s) {}

    const TfTokenVector &GetSupportedRprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->basisCurves, HdPrimTypeTokens->points, HdPrimTypeTokens->nurbsCurves,
                               HdPrimTypeTokens->mesh};
        return v;
    }
    const TfTokenVector &GetSupportedSprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->camera, HdPrimTypeTokens->material};
        return v;
    }
    const TfTokenVector &GetSupportedBprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->renderBuffer};
        return v;
    }
    HdResourceRegistrySharedPtr GetResourceRegistry() const override { return _registry; }
    HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex *index, HdRprimCollection const &c) override {
        return std::make_shared<ProbeRenderPass>(index, c);
    }
    HdInstancer *CreateInstancer(HdSceneDelegate *sd, SdfPath const &id) override { return new ProbeInstancer(sd, id); }
    void DestroyInstancer(HdInstancer *i) override { delete i; }
    HdRprim *CreateRprim(TfToken const &type, SdfPath const &id) override {
        if (type == HdPrimTypeTokens->basisCurves)
            return new ProbeCurves(id);
        if (type == HdPrimTypeTokens->points)
            return new ProbePoints(id);
        return new ProbeOther(type, id);
    }
    void DestroyRprim(HdRprim *r) override { delete r; }
    HdSprim *CreateSprim(TfToken const &type, SdfPath const &id) override {
        if (type == HdPrimTypeTokens->camera)
            return new HdCamera(id);
        if (type == HdPrimTypeTokens->material)
            return new ProbeMaterial(id);
        return nullptr;
    }
    HdSprim *CreateFallbackSprim(TfToken const &type) override { return CreateSprim(type, SdfPath::EmptyPath()); }
    void DestroySprim(HdSprim *s) override { delete s; }
    HdBprim *CreateBprim(TfToken const &type, SdfPath const &id) override {
        return type == HdPrimTypeTokens->renderBuffer ? new ProbeRenderBuffer(id) : nullptr;
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

class HdCurvesProbeRendererPlugin final : public HdRendererPlugin {
public:
    HdRenderDelegate *CreateRenderDelegate() override { return new ProbeRenderDelegate(); }
    HdRenderDelegate *CreateRenderDelegate(HdRenderSettingsMap const &s) override {
        std::map<std::string, std::string> sorted;
        for (auto const &kv : s) {
            const std::string &k = kv.first.GetString();
            if (k.find("curve") != std::string::npos || k.find("point") != std::string::npos ||
                k.find("width") != std::string::npos || k.find("bias") != std::string::npos)
                sorted[k] = str(kv.second);
        }
        PROBE_LOG("CreateRenderDelegate with %zu settings; curve-related:\n", s.size());
        for (auto const &kv : sorted)
            PROBE_LOG("    setting %s = %s\n", kv.first.c_str(), kv.second.c_str());
        return new ProbeRenderDelegate(s);
    }
    void DeleteRenderDelegate(HdRenderDelegate *d) override { delete d; }
    using HdRendererPlugin::IsSupported;
    bool IsSupported(HdRendererCreateArgs const &, std::string *) const override { return true; }
};

TF_REGISTRY_FUNCTION(TfType) {
    HdRendererPluginRegistry::Define<HdCurvesProbeRendererPlugin>();
}

PXR_NAMESPACE_CLOSE_SCOPE
