// Research probe for M9 (docs/research/m9/lights.md, section 4.2), not part of Nova.
//
// Minimal HDK-free Hydra render delegate "LinkProbe". It renders nothing and
// logs to stderr what a third-party delegate receives for light linking:
//   - every light sprim (all UsdLux types, meshLight, simpleLight):
//     GetLightParamValue(lightLink | shadowLink | filters), the scene-index
//     prim's top-level data sources, the light schema's lightLink/shadowLink
//     entries and its "collections" (membership expressions);
//   - every mesh/sphere rprim: GetCategories, and for instanced prims the
//     instancer's GetCategories and GetInstanceCategories; the scene-index
//     prim's "categories" schema;
//   - the render settings whose names contain "light".
// LT_LINK_SI selects the light-linking scene index for renderer "LinkProbe":
//   unset/none  nothing (what a delegate gets by default),
//   hdsi        HdsiLightLinkingSceneIndex (pxr_hdsi) through this library's
//               scene-index plugin,
//   houdini     Houdini's HD_LightLinkingSceneIndexPlugin (USD_HD.dll),
//               registered for "LinkProbe" by plugin id.
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/categoriesSchema.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/collectionSchema.h>
#include <pxr/imaging/hd/collectionsSchema.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sceneIndexPlugin.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

std::mutex g_log;
#define PROBE_LOG(...)                                   \
    do {                                                 \
        std::lock_guard<std::mutex> _l(g_log);           \
        std::fprintf(stderr, "[linkprobe] " __VA_ARGS__); \
        std::fflush(stderr);                             \
    } while (0)

std::string str(VtValue const &v, size_t maxlen = 300) {
    std::ostringstream os;
    if (v.IsEmpty())
        return "<empty>";
    os << v << " (" << v.GetTypeName() << ")";
    std::string s = os.str();
    if (s.size() > maxlen)
        s = s.substr(0, maxlen) + "...";
    return s;
}

std::string tokens(VtArray<TfToken> const &a) {
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < a.size(); ++i)
        os << (i ? ", " : "") << "'" << a[i] << "'";
    os << "]";
    return os.str();
}

std::string mode() {
    const char *m = std::getenv("LT_LINK_SI");
    return m ? m : "none";
}

HdSceneIndexPrim siPrim(HdSceneDelegate *sd, SdfPath const &id) {
    if (HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex())
        return si->GetPrim(id);
    return {};
}

std::string names(HdContainerDataSourceHandle const &c) {
    std::ostringstream os;
    if (c)
        for (TfToken const &n : c->GetNames())
            os << " " << n;
    return os.str();
}

} // namespace

// ---------------------------------------------------------------------------
class LinkProbeLight final : public HdLight {
public:
    LinkProbeLight(TfToken const &type, SdfPath const &id) : HdLight(id), _type(type) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits) override {
        const SdfPath &id = GetId();
        if (id.IsEmpty()) {
            *bits = Clean;
            return;
        }
        std::ostringstream os;
        os << "light " << _type << " " << id << " dirty 0x" << std::hex << *bits << std::dec << "\n";
        os << "    instancerId '" << sd->GetInstancerId(id) << "', transform translation "
           << sd->GetTransform(id).ExtractTranslation() << "\n";
        os << "    param lightLink = " << str(sd->GetLightParamValue(id, HdTokens->lightLink)) << "\n";
        os << "    param shadowLink = " << str(sd->GetLightParamValue(id, HdTokens->shadowLink)) << "\n";
        os << "    param filters = " << str(sd->GetLightParamValue(id, HdTokens->filters)) << "\n";
        for (const char *n : {"collection:lightLink:includeRoot", "inputs:karma:light:samplingquality",
                              "inputs:karma:light:samplingmode", "inputs:karma:light:category"})
            os << "    param " << n << " = " << str(sd->GetLightParamValue(id, TfToken(n))) << "\n";
        const HdSceneIndexPrim prim = siPrim(sd, id);
        os << "    scene index prim type '" << prim.primType << "' data sources:" << names(prim.dataSource) << "\n";
        if (HdLightSchema ls = HdLightSchema::GetFromParent(prim.dataSource)) {
            HdContainerDataSourceHandle c = ls.GetContainer();
            os << "    light schema:" << names(c) << "\n";
            for (TfToken const &n : {HdTokens->lightLink, HdTokens->shadowLink, HdTokens->filters}) {
                if (auto d = HdSampledDataSource::Cast(c->Get(n)))
                    os << "    light." << n << " = " << str(d->GetValue(0.0f)) << "\n";
            }
        }
        if (HdCollectionsSchema cs = HdCollectionsSchema::GetFromParent(prim.dataSource)) {
            for (TfToken const &n : cs.GetCollectionNames()) {
                HdCollectionSchema c = cs.GetCollection(n);
                std::string expr = "<none>";
                if (auto e = c.GetMembershipExpression())
                    expr = e->GetTypedValue(0.0f).GetText();
                os << "    collection " << n << " membershipExpression = " << expr << "\n";
            }
        }
        PROBE_LOG("%s", os.str().c_str());
        *bits = Clean;
    }

private:
    TfToken _type;
};

// ---------------------------------------------------------------------------
class LinkProbeMesh final : public HdMesh {
public:
    explicit LinkProbeMesh(SdfPath const &id) : HdMesh(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *bits, TfToken const &) override {
        const SdfPath &id = GetId();
        _UpdateInstancer(sd, bits);
        std::ostringstream os;
        os << "rprim " << id << " dirty 0x" << std::hex << *bits << std::dec << " categories "
           << tokens(sd->GetCategories(id)) << "\n";
        const HdSceneIndexPrim prim = siPrim(sd, id);
        os << "    scene index prim type '" << prim.primType << "'";
        if (HdCategoriesSchema cat = HdCategoriesSchema::GetFromParent(prim.dataSource))
            os << ", categories schema " << tokens(cat.GetIncludedCategoryNames());
        else
            os << ", no categories schema";
        os << "\n";
        SdfPath inst = GetInstancerId();
        while (!inst.IsEmpty()) {
            os << "    instancer " << inst << " categories " << tokens(sd->GetCategories(inst));
            const std::vector<VtArray<TfToken>> per = sd->GetInstanceCategories(inst);
            os << ", instance categories (" << per.size() << "):";
            for (size_t i = 0; i < per.size() && i < 6; ++i)
                os << " " << tokens(per[i]);
            os << "\n";
            const HdSceneIndexPrim ip = siPrim(sd, inst);
            os << "    instancer scene index prim '" << ip.primType << "' data sources:" << names(ip.dataSource)
               << "\n";
            inst = sd->GetInstancerId(inst);
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
};

class LinkProbeInstancer final : public HdInstancer {
public:
    LinkProbeInstancer(HdSceneDelegate *sd, SdfPath const &id) : HdInstancer(sd, id) {}
    void Sync(HdSceneDelegate *, HdRenderParam *, HdDirtyBits *bits) override { *bits = HdChangeTracker::Clean; }
};

// ---------------------------------------------------------------------------
class LinkProbeRenderBuffer final : public HdRenderBuffer {
public:
    explicit LinkProbeRenderBuffer(SdfPath const &id) : HdRenderBuffer(id) {}
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

class LinkProbeRenderPass final : public HdRenderPass {
public:
    LinkProbeRenderPass(HdRenderIndex *index, HdRprimCollection const &c) : HdRenderPass(index, c) {}
    bool IsConverged() const override { return true; }

protected:
    void _Execute(HdRenderPassStateSharedPtr const &, TfTokenVector const &) override {
        if (!_logged) {
            PROBE_LOG("render pass executed (LT_LINK_SI=%s)\n", mode().c_str());
            _logged = true;
        }
    }

private:
    bool _logged = false;
};

// ---------------------------------------------------------------------------
class LinkProbeRenderDelegate final : public HdRenderDelegate {
public:
    LinkProbeRenderDelegate() = default;
    explicit LinkProbeRenderDelegate(HdRenderSettingsMap const &s) : HdRenderDelegate(s) {}

    const TfTokenVector &GetSupportedRprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->mesh};
        return v;
    }
    const TfTokenVector &GetSupportedSprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->camera,       HdPrimTypeTokens->material,
                               HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight,
                               HdPrimTypeTokens->sphereLight,  HdPrimTypeTokens->rectLight,
                               HdPrimTypeTokens->diskLight,    HdPrimTypeTokens->cylinderLight,
                               HdPrimTypeTokens->meshLight,    HdPrimTypeTokens->simpleLight};
        return v;
    }
    const TfTokenVector &GetSupportedBprimTypes() const override {
        static TfTokenVector v{HdPrimTypeTokens->renderBuffer};
        return v;
    }
    HdResourceRegistrySharedPtr GetResourceRegistry() const override { return _registry; }
    HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex *index, HdRprimCollection const &c) override {
        return std::make_shared<LinkProbeRenderPass>(index, c);
    }
    HdInstancer *CreateInstancer(HdSceneDelegate *sd, SdfPath const &id) override {
        return new LinkProbeInstancer(sd, id);
    }
    void DestroyInstancer(HdInstancer *i) override { delete i; }
    HdRprim *CreateRprim(TfToken const &type, SdfPath const &id) override {
        return type == HdPrimTypeTokens->mesh ? new LinkProbeMesh(id) : nullptr;
    }
    void DestroyRprim(HdRprim *r) override { delete r; }
    HdSprim *CreateSprim(TfToken const &type, SdfPath const &id) override {
        if (type == HdPrimTypeTokens->camera)
            return new HdCamera(id);
        if (type == HdPrimTypeTokens->material)
            return nullptr;
        return new LinkProbeLight(type, id);
    }
    HdSprim *CreateFallbackSprim(TfToken const &type) override {
        if (type == HdPrimTypeTokens->camera)
            return new HdCamera(SdfPath::EmptyPath());
        if (type == HdPrimTypeTokens->material)
            return nullptr;
        return new LinkProbeLight(type, SdfPath::EmptyPath());
    }
    void DestroySprim(HdSprim *s) override { delete s; }
    HdBprim *CreateBprim(TfToken const &type, SdfPath const &id) override {
        return type == HdPrimTypeTokens->renderBuffer ? new LinkProbeRenderBuffer(id) : nullptr;
    }
    HdBprim *CreateFallbackBprim(TfToken const &) override { return nullptr; }
    void DestroyBprim(HdBprim *b) override { delete b; }
    void CommitResources(HdChangeTracker *) override {}
    HdAovDescriptor GetDefaultAovDescriptor(TfToken const &) const override {
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

class HdLinkProbeRendererPlugin final : public HdRendererPlugin {
public:
    HdRenderDelegate *CreateRenderDelegate() override { return new LinkProbeRenderDelegate(); }
    HdRenderDelegate *CreateRenderDelegate(HdRenderSettingsMap const &s) override {
        std::map<std::string, std::string> sorted;
        for (auto const &kv : s) {
            const std::string &k = kv.first.GetString();
            if (k.find("light") != std::string::npos || k.find("Light") != std::string::npos)
                sorted[k] = str(kv.second);
        }
        PROBE_LOG("CreateRenderDelegate with %zu settings (LT_LINK_SI=%s); light-related:\n", s.size(),
                  mode().c_str());
        for (auto const &kv : sorted)
            PROBE_LOG("    setting %s = %s\n", kv.first.c_str(), kv.second.c_str());
        return new LinkProbeRenderDelegate(s);
    }
    void DeleteRenderDelegate(HdRenderDelegate *d) override { delete d; }
    using HdRendererPlugin::IsSupported;
    bool IsSupported(HdRendererCreateArgs const &, std::string *) const override { return true; }
};

// ---------------------------------------------------------------------------
// Scene indices for this renderer. A plugInfo "loadWithRenderer" entry
// alone already appends a scene-index plugin for that renderer in this USD
// version (first probe run), so the hdsi mode uses the callback form, which
// needs no plugInfo type.
TF_REGISTRY_FUNCTION(TfType) {
    HdRendererPluginRegistry::Define<HdLinkProbeRendererPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    const std::string m = mode();
    if (m == "hdsi") {
        // Light and geometry types as HdPrman passes them (lights incl. mesh
        // lights; geometry: the rprim types; instancers get instanceCategories).
        VtArray<TfToken> lights{HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight,
                                HdPrimTypeTokens->sphereLight,  HdPrimTypeTokens->rectLight,
                                HdPrimTypeTokens->diskLight,    HdPrimTypeTokens->cylinderLight,
                                HdPrimTypeTokens->meshLight,    HdPrimTypeTokens->simpleLight};
        VtArray<TfToken> geometry{HdPrimTypeTokens->mesh, HdPrimTypeTokens->basisCurves, HdPrimTypeTokens->points,
                                  HdPrimTypeTokens->volume, HdPrimTypeTokens->sphere};
        const HdContainerDataSourceHandle args = HdRetainedContainerDataSource::New(
            HdsiLightLinkingSceneIndexTokens->lightPrimTypes,
            HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(lights),
            HdsiLightLinkingSceneIndexTokens->geometryPrimTypes,
            HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(geometry));
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            "LinkProbe",
            [](const std::string &, const HdSceneIndexBaseRefPtr &input, const HdContainerDataSourceHandle &a) {
                PROBE_LOG("appending HdsiLightLinkingSceneIndex\n");
                return HdSceneIndexBaseRefPtr(HdsiLightLinkingSceneIndex::New(input, a));
            },
            args, 0, HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
    } else if (m == "houdini") {
        PROBE_LOG("registering HD_LightLinkingSceneIndexPlugin for LinkProbe\n");
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            "LinkProbe", TfToken("HD_LightLinkingSceneIndexPlugin"), nullptr, 0,
            HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
