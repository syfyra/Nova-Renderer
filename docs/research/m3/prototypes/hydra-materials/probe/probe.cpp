// hdProbe: a do-nothing Hydra render delegate that logs what materials and
// meshes receive (research probe for Nova M3; throwaway).
#include <pxr/pxr.h>

#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/schemaTypeDefs.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rendererCreateArgs.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>
#include <pxr/usd/sdf/assetPath.h>

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/File.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

std::mutex g_logMutex;

void logStr(std::string const &s) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    static FILE *f = nullptr;
    static bool init = false;
    if (!init) {
        init = true;
        const std::string path = TfGetenv("PROBE_LOG");
        if (!path.empty())
            f = std::fopen(path.c_str(), "a");
    }
    FILE *out = f ? f : stderr;
    std::fputs(s.c_str(), out);
    std::fflush(out);
}

std::string valueStr(VtValue const &v) {
    std::ostringstream os;
    if (v.IsHolding<SdfAssetPath>()) {
        SdfAssetPath const &a = v.UncheckedGet<SdfAssetPath>();
        os << "@" << a.GetAssetPath() << "@ (resolved '" << a.GetResolvedPath() << "')";
    } else {
        os << v;
    }
    std::string s = os.str();
    if (s.size() > 400)
        s = s.substr(0, 400) + "...";
    return s + "  <" + v.GetTypeName() + ">";
}

std::string rprimBits(HdDirtyBits b) {
    static const std::pair<HdDirtyBits, const char *> names[] = {
        {HdChangeTracker::InitRepr, "InitRepr"},       {HdChangeTracker::Varying, "Varying"},
        {HdChangeTracker::DirtyPrimID, "PrimID"},      {HdChangeTracker::DirtyExtent, "Extent"},
        {HdChangeTracker::DirtyDisplayStyle, "DisplayStyle"}, {HdChangeTracker::DirtyPoints, "Points"},
        {HdChangeTracker::DirtyPrimvar, "Primvar"},    {HdChangeTracker::DirtyMaterialId, "MaterialId"},
        {HdChangeTracker::DirtyTopology, "Topology"},  {HdChangeTracker::DirtyTransform, "Transform"},
        {HdChangeTracker::DirtyVisibility, "Visibility"}, {HdChangeTracker::DirtyNormals, "Normals"},
        {HdChangeTracker::DirtyDoubleSided, "DoubleSided"}, {HdChangeTracker::DirtyCullStyle, "CullStyle"},
        {HdChangeTracker::DirtySubdivTags, "SubdivTags"}, {HdChangeTracker::DirtyWidths, "Widths"},
        {HdChangeTracker::DirtyInstancer, "Instancer"}, {HdChangeTracker::DirtyInstanceIndex, "InstanceIndex"},
        {HdChangeTracker::DirtyRepr, "Repr"},          {HdChangeTracker::DirtyRenderTag, "RenderTag"},
        {HdChangeTracker::DirtyComputationPrimvarDesc, "CompPrimvarDesc"},
        {HdChangeTracker::DirtyCategories, "Categories"}, {HdChangeTracker::DirtyVolumeField, "VolumeField"},
        {HdChangeTracker::NewRepr, "NewRepr"}};
    std::string s;
    for (auto const &[bit, name] : names)
        if (b & bit)
            s += std::string(s.empty() ? "" : "|") + name;
    return s.empty() ? "Clean" : s;
}

std::string materialBits(HdDirtyBits b) {
    std::string s;
    if (b & HdMaterial::DirtyParams) s += "Params|";
    if (b & HdMaterial::DirtyResource) s += "Resource|";
    if (b & HdMaterial::DirtySurface) s += "Surface|";
    if (b & HdMaterial::DirtyDisplacement) s += "Displacement|";
    if (b & HdMaterial::DirtyVolume) s += "Volume|";
    if (!s.empty()) s.pop_back();
    return s.empty() ? "Clean" : s;
}

TfTokenVector envTokens(const char *name, TfTokenVector fallback) {
    const std::string v = TfGetenv(name, "\x01");
    if (v == "\x01")
        return fallback;
    TfTokenVector out;
    for (std::string const &s : TfStringSplit(v, ","))
        out.emplace_back(s == "<universal>" ? std::string() : s);
    return out;
}

void dumpNetworkMap(std::ostringstream &os, HdMaterialNetworkMap const &map) {
    os << "  HdMaterialNetworkMap: terminals = [";
    for (SdfPath const &t : map.terminals) os << " '" << t.GetString() << "'";
    os << " ]  config = {";
    for (auto const &kv : map.config) os << " " << kv.first << "=" << valueStr(kv.second);
    os << " }\n";
    for (auto const &[terminal, net] : map.map) {
        os << "  network['" << terminal << "']: " << net.nodes.size() << " nodes, " << net.relationships.size()
           << " relationships, primvars = [";
        for (TfToken const &p : net.primvars) os << " " << p;
        os << " ]\n";
        for (HdMaterialNode const &n : net.nodes) {
            os << "    node '" << n.path.GetString() << "' (absolute=" << n.path.IsAbsolutePath()
               << ") id='" << n.identifier << "'\n";
            for (auto const &[name, v] : n.parameters) os << "      " << name << " = " << valueStr(v) << "\n";
        }
        for (HdMaterialRelationship const &r : net.relationships)
            os << "    rel " << r.inputId.GetString() << "." << r.inputName << " -> " << r.outputId.GetString() << "."
               << r.outputName << "\n";
    }
}

void dumpNetwork2(std::ostringstream &os, HdMaterialNetwork2 const &net) {
    os << "  HdMaterialNetwork2: terminals:";
    for (auto const &[name, c] : net.terminals) os << " " << name << "<-" << c.upstreamNode << "." << c.upstreamOutputName;
    os << "  primvars:[";
    for (TfToken const &p : net.primvars) os << " " << p;
    os << " ]  config:{";
    for (auto const &kv : net.config) os << " " << kv.first << "=" << valueStr(kv.second);
    os << " }\n";
    for (auto const &[path, node] : net.nodes) {
        os << "    node " << path << " : " << node.nodeTypeId << "  (" << node.parameters.size() << " params, "
           << node.inputConnections.size() << " connected inputs)\n";
        for (auto const &[in, conns] : node.inputConnections)
            for (HdMaterialConnection2 const &c : conns)
                os << "      in " << in << " <- " << c.upstreamNode << "." << c.upstreamOutputName << "\n";
    }
}

void dumpPrimDataSource(std::ostringstream &os, HdSceneDelegate *sd, SdfPath const &id, TfToken const &child) {
    HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex();
    if (!si) {
        os << "  (no terminal scene index)\n";
        return;
    }
    HdSceneIndexPrim prim = si->GetPrim(id);
    if (!prim.dataSource) {
        os << "  (no prim data source)\n";
        return;
    }
    if (child.IsEmpty()) {
        os << "  prim type '" << prim.primType << "', data source names:";
        for (TfToken const &n : prim.dataSource->GetNames()) os << " " << n;
        os << "\n";
        return;
    }
    os << "  data source '" << child << "':\n";
    HdDebugPrintDataSource(os, prim.dataSource->Get(child), 2);
}

// --- Candidate code for Nova: read one render context's network straight
// from the terminal scene index (keeps colorSpace/typeName separate and the
// nodeTypeInfo that GetMaterialResource drops).
struct ParamMeta {
    TfToken colorSpace;
    TfToken typeName;
};
struct NetworkFromSI {
    HdMaterialNetwork2 net;
    std::map<SdfPath, std::map<TfToken, ParamMeta>> meta;
    std::map<SdfPath, VtDictionary> nodeTypeInfo;
};

bool ReadNetworkFromSceneIndex(HdSceneIndexBaseRefPtr const &si, SdfPath const &materialId, TfToken const &context,
                               NetworkFromSI *out) {
    const HdSceneIndexPrim prim = si->GetPrim(materialId);
    const HdMaterialNetworkSchema ns = HdMaterialSchema::GetFromParent(prim.dataSource).GetMaterialNetwork(context);
    if (!ns)
        return false;
    const HdMaterialNodeContainerSchema nodes = ns.GetNodes();
    for (TfToken const &name : nodes.GetNames()) {
        const HdMaterialNodeSchema node = nodes.Get(name);
        const SdfPath path(name.GetString());
        HdMaterialNode2 &n2 = out->net.nodes[path];
        if (HdTokenDataSourceHandle ds = node.GetNodeIdentifier())
            n2.nodeTypeId = ds->GetTypedValue(0.0f);
        const HdMaterialNodeParameterContainerSchema params = node.GetParameters();
        for (TfToken const &p : params.GetNames()) {
            const HdMaterialNodeParameterSchema ps = params.Get(p);
            if (HdSampledDataSourceHandle v = ps.GetValue())
                n2.parameters[p] = v->GetValue(0.0f);
            ParamMeta m;
            if (HdTokenDataSourceHandle cs = ps.GetColorSpace()) m.colorSpace = cs->GetTypedValue(0.0f);
            if (HdTokenDataSourceHandle tn = ps.GetTypeName()) m.typeName = tn->GetTypedValue(0.0f);
            if (!m.colorSpace.IsEmpty() || !m.typeName.IsEmpty())
                out->meta[path][p] = m;
        }
        const HdMaterialConnectionVectorContainerSchema conns = node.GetInputConnections();
        for (TfToken const &in : conns.GetNames()) {
            const HdMaterialConnectionVectorSchema vec = conns.Get(in);
            for (size_t i = 0; i < vec.GetNumElements(); ++i) {
                const HdMaterialConnectionSchema c = vec.GetElement(i);
                HdTokenDataSourceHandle up = c.GetUpstreamNodePath(), upOut = c.GetUpstreamNodeOutputName();
                if (up)
                    n2.inputConnections[in].push_back(
                        {SdfPath(up->GetTypedValue(0.0f).GetString()), upOut ? upOut->GetTypedValue(0.0f) : TfToken()});
            }
        }
        if (HdContainerDataSourceHandle info = node.GetNodeTypeInfo()) {
            VtDictionary d;
            for (TfToken const &k : info->GetNames())
                if (HdSampledDataSourceHandle v = HdSampledDataSource::Cast(info->Get(k)))
                    d[k.GetString()] = v->GetValue(0.0f);
            out->nodeTypeInfo[path] = d;
        }
    }
    const HdMaterialConnectionContainerSchema terms = ns.GetTerminals();
    for (TfToken const &t : terms.GetNames()) {
        const HdMaterialConnectionSchema c = terms.Get(t);
        HdTokenDataSourceHandle up = c.GetUpstreamNodePath(), upOut = c.GetUpstreamNodeOutputName();
        if (up && !up->GetTypedValue(0.0f).IsEmpty())
            out->net.terminals[t] = {SdfPath(up->GetTypedValue(0.0f).GetString()),
                                     upOut ? upOut->GetTypedValue(0.0f) : TfToken()};
    }
    if (const HdSampledDataSourceContainerSchema cfg = ns.GetConfig())
        for (TfToken const &k : cfg.GetNames())
            if (HdSampledDataSourceHandle v = cfg.Get(k))
                out->net.config[k.GetString()] = v->GetValue(0.0f);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
class HdProbeMaterial final : public HdMaterial {
public:
    explicit HdProbeMaterial(SdfPath const &id) : HdMaterial(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }

    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *dirtyBits) override {
        std::ostringstream os;
        os << "=== Material Sync " << GetId() << " bits=" << materialBits(*dirtyBits) << " (0x" << std::hex
           << *dirtyBits << std::dec << ")\n";
        if (GetId().IsEmpty()) {
            *dirtyBits = Clean;
            logStr(os.str() + "  (fallback material)\n");
            return;
        }
        if (TfGetenv("PROBE_SI_READ") == "1") {
            HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex();
            for (const char *ctx : {"nova", "kma", "mtlx", "vex", ""}) {
                NetworkFromSI r;
                if (!si || !ReadNetworkFromSceneIndex(si, GetId(), TfToken(ctx), &r))
                    continue;
                os << "  [SI read] context '" << ctx << "':\n";
                dumpNetwork2(os, r.net);
                for (auto const &[path, params] : r.meta)
                    for (auto const &[p, m] : params)
                        if (!m.colorSpace.IsEmpty())
                            os << "    meta " << path << "." << p << " colorSpace=" << m.colorSpace << "\n";
                for (auto const &[path, d] : r.nodeTypeInfo) {
                    os << "    nodeTypeInfo " << path << ":";
                    for (auto const &kv : d) os << " " << kv.first << "=" << valueStr(kv.second);
                    os << "\n";
                }
            }
        }
        if (TfGetenv("PROBE_DUMP_SI") == "1") {
            dumpPrimDataSource(os, sd, GetId(), TfToken());
            HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex();
            if (si) {
                HdSceneIndexPrim prim = si->GetPrim(GetId());
                HdMaterialSchema ms = HdMaterialSchema::GetFromParent(prim.dataSource);
                os << "  scene-index render contexts:";
                for (TfToken const &c : ms.GetRenderContexts()) os << " '" << c << "'";
                os << "\n";
                if (TfGetenv("PROBE_DUMP_SI_FULL") == "1")
                    dumpPrimDataSource(os, sd, GetId(), HdMaterialSchema::GetSchemaToken());
            }
        }
        const VtValue res = sd->GetMaterialResource(GetId());
        os << "  GetMaterialResource -> " << res.GetTypeName() << "\n";
        if (res.IsHolding<HdMaterialNetworkMap>()) {
            HdMaterialNetworkMap const &map = res.UncheckedGet<HdMaterialNetworkMap>();
            dumpNetworkMap(os, map);
            bool isVolume = false;
            HdMaterialNetwork2 net2 = HdConvertToHdMaterialNetwork2(map, &isVolume);
            os << "  isVolume=" << isVolume << "\n";
            dumpNetwork2(os, net2);
            if (TfGetenv("PROBE_MTLX") == "1") {
                auto it = net2.terminals.find(HdMaterialTerminalTokens->surface);
                if (it != net2.terminals.end()) {
                    auto nit = net2.nodes.find(it->second.upstreamNode);
                    if (nit != net2.nodes.end() && TfStringStartsWith(nit->second.nodeTypeId.GetString(), "ND_")) {
                        try {
                            HdMtlxTexturePrimvarData data;
                            MaterialX::DocumentPtr doc = HdMtlxCreateMtlxDocumentFromHdNetwork(
                                net2, nit->second, it->second.upstreamNode, GetId(), HdMtlxStdLibraries(), &data);
                            if (doc) {
                                // Write without the library content.
                                MaterialX::XmlWriteOptions wo;
                                wo.elementPredicate = [](MaterialX::ConstElementPtr e) {
                                    return !e->hasSourceUri();
                                };
                                os << "  --- hdMtlx document ---\n" << MaterialX::writeToXmlString(doc, &wo);
                                os << "  hdTextureNodes:";
                                for (SdfPath const &p : data.hdTextureNodes) os << " " << p;
                                os << "\n  hdPrimvarNodes:";
                                for (SdfPath const &p : data.hdPrimvarNodes) os << " " << p;
                                os << "\n  mxHdTextureMap:";
                                for (auto const &[k, v] : data.mxHdTextureMap) {
                                    os << " " << k << "->{";
                                    for (auto const &s : v) os << s << ",";
                                    os << "}";
                                }
                                os << "\n";
                                std::string msg;
                                os << "  doc->validate(): " << doc->validate(&msg) << " " << msg << "\n";
                            } else {
                                os << "  hdMtlx returned null\n";
                            }
                        } catch (std::exception const &e) {
                            os << "  hdMtlx exception: " << e.what() << "\n";
                        }
                    }
                }
            }
        }
        *dirtyBits = Clean;
        logStr(os.str());
    }
};

// ---------------------------------------------------------------------------
class HdProbeMesh final : public HdMesh {
public:
    explicit HdProbeMesh(SdfPath const &id) : HdMesh(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllSceneDirtyBits; }

    void Sync(HdSceneDelegate *sd, HdRenderParam *, HdDirtyBits *dirtyBits, TfToken const &) override {
        std::ostringstream os;
        const SdfPath &id = GetId();
        os << "=== Mesh Sync " << id << " bits=" << rprimBits(*dirtyBits) << "\n";
        if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
            SetMaterialId(sd->GetMaterialId(id));
            os << "  GetMaterialId -> '" << GetMaterialId() << "'\n";
        }
        if (*dirtyBits & HdChangeTracker::DirtyTopology) {
            const HdMeshTopology topo = GetMeshTopology(sd);
            for (HdGeomSubset const &s : topo.GetGeomSubsets())
                os << "  geomSubset id=" << s.id << " type=" << int(s.type) << " materialId='" << s.materialId
                   << "' faces=" << s.indices.size() << "\n";
        }
        if (*dirtyBits & HdChangeTracker::DirtyPrimvar) {
            for (int i = 0; i < HdInterpolationCount; ++i) {
                for (HdPrimvarDescriptor const &d : sd->GetPrimvarDescriptors(id, HdInterpolation(i))) {
                    os << "  primvar " << d.name << " interp=" << TfEnum::GetName(HdInterpolation(i))
                       << " role='" << d.role << "' indexed=" << d.indexed;
                    if (d.name != HdTokens->points) {
                        VtIntArray indices;
                        const VtValue v = d.indexed ? sd->GetIndexedPrimvar(id, d.name, &indices) : sd->Get(id, d.name);
                        os << " value=" << valueStr(v);
                        if (d.indexed) os << " indices=" << indices.size();
                    }
                    os << "\n";
                }
            }
        }
        if (TfGetenv("PROBE_DUMP_SI") == "1")
            dumpPrimDataSource(os, sd, id, HdMaterialBindingsSchema::GetSchemaToken());
        *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
        logStr(os.str());
    }

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *) override {
        auto it = std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken));
        if (it == _reprs.end())
            _reprs.emplace_back(reprToken, std::make_shared<HdRepr>());
    }
};

// ---------------------------------------------------------------------------
class HdProbeCamera final : public HdCamera {
public:
    explicit HdProbeCamera(SdfPath const &id) : HdCamera(id) {}
};

class HdProbeRenderBuffer final : public HdRenderBuffer {
public:
    explicit HdProbeRenderBuffer(SdfPath const &id) : HdRenderBuffer(id) {}
    bool Allocate(GfVec3i const &dim, HdFormat format, bool) override {
        _w = dim[0];
        _h = dim[1];
        _format = format;
        _data.assign(size_t(_w) * _h * HdDataSizeOfFormat(format), 0);
        return true;
    }
    unsigned int GetWidth() const override { return _w; }
    unsigned int GetHeight() const override { return _h; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return false; }
    void *Map() override { return _data.data(); }
    void Unmap() override {}
    bool IsMapped() const override { return false; }
    void Resolve() override {}
    bool IsConverged() const override { return true; }

protected:
    void _Deallocate() override { _data.clear(); }

private:
    unsigned _w = 0, _h = 0;
    HdFormat _format = HdFormatInvalid;
    std::vector<uint8_t> _data;
};

class HdProbeRenderPass final : public HdRenderPass {
public:
    using HdRenderPass::HdRenderPass;
    bool IsConverged() const override { return true; }

protected:
    void _Execute(HdRenderPassStateSharedPtr const &, TfTokenVector const &) override {}
};

// ---------------------------------------------------------------------------
class HdProbeRenderDelegate final : public HdRenderDelegate {
public:
    HdProbeRenderDelegate() { _Init(); }
    explicit HdProbeRenderDelegate(HdRenderSettingsMap const &s) : HdRenderDelegate(s) { _Init(); }

    const TfTokenVector &GetSupportedRprimTypes() const override {
        static const TfTokenVector t{HdPrimTypeTokens->mesh};
        return t;
    }
    const TfTokenVector &GetSupportedSprimTypes() const override {
        static const TfTokenVector t{HdPrimTypeTokens->camera, HdPrimTypeTokens->material};
        return t;
    }
    const TfTokenVector &GetSupportedBprimTypes() const override {
        static const TfTokenVector t{HdPrimTypeTokens->renderBuffer};
        return t;
    }
    HdRenderParam *GetRenderParam() const override { return nullptr; }
    HdResourceRegistrySharedPtr GetResourceRegistry() const override { return _registry; }
    HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex *index, HdRprimCollection const &c) override {
        return std::make_shared<HdProbeRenderPass>(index, c);
    }
    HdInstancer *CreateInstancer(HdSceneDelegate *, SdfPath const &) override { return nullptr; }
    void DestroyInstancer(HdInstancer *i) override { delete i; }
    HdRprim *CreateRprim(TfToken const &t, SdfPath const &id) override {
        return t == HdPrimTypeTokens->mesh ? new HdProbeMesh(id) : nullptr;
    }
    void DestroyRprim(HdRprim *p) override { delete p; }
    HdSprim *CreateSprim(TfToken const &t, SdfPath const &id) override {
        if (t == HdPrimTypeTokens->camera) return new HdProbeCamera(id);
        if (t == HdPrimTypeTokens->material) return new HdProbeMaterial(id);
        return nullptr;
    }
    HdSprim *CreateFallbackSprim(TfToken const &t) override { return CreateSprim(t, SdfPath::EmptyPath()); }
    void DestroySprim(HdSprim *p) override { delete p; }
    HdBprim *CreateBprim(TfToken const &t, SdfPath const &id) override {
        return t == HdPrimTypeTokens->renderBuffer ? new HdProbeRenderBuffer(id) : nullptr;
    }
    HdBprim *CreateFallbackBprim(TfToken const &t) override { return CreateBprim(t, SdfPath::EmptyPath()); }
    void DestroyBprim(HdBprim *p) override { delete p; }
    void CommitResources(HdChangeTracker *) override {}
    HdAovDescriptor GetDefaultAovDescriptor(TfToken const &name) const override {
        if (name == HdAovTokens->color) return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
    }
    TfToken GetMaterialBindingPurpose() const override { return _purpose; }
    TfTokenVector GetMaterialRenderContexts() const override { return _contexts; }
    TfTokenVector GetShaderSourceTypes() const override { return {TfToken("mtlx")}; }

private:
    void _Init() {
        _registry = std::make_shared<HdResourceRegistry>();
        _contexts = envTokens("PROBE_CONTEXTS", {TfToken("mtlx")});
        const std::string p = TfGetenv("PROBE_PURPOSE", "full");
        _purpose = TfToken(p == "<all>" ? "" : p);
        std::ostringstream os;
        os << "=== HdProbeRenderDelegate: contexts=[";
        for (TfToken const &c : _contexts) os << " '" << c << "'";
        os << " ] purpose='" << _purpose << "'\n";
        os << "  env PXR_MTLX_STDLIB_SEARCH_PATHS='" << TfGetenv("PXR_MTLX_STDLIB_SEARCH_PATHS") << "'\n";
        os << "  env PXR_MTLX_PLUGIN_SEARCH_PATHS='" << TfGetenv("PXR_MTLX_PLUGIN_SEARCH_PATHS") << "'\n";
        os << "  HdMtlxSearchPaths: " << HdMtlxSearchPaths().asString() << "\n";
        os << "  render settings:";
        for (auto const &kv : _settingsMap) os << "\n    " << kv.first << " = " << valueStr(kv.second);
        os << "\n";
        logStr(os.str());
    }
    HdResourceRegistrySharedPtr _registry;
    TfTokenVector _contexts;
    TfToken _purpose;
};

class HdProbeRendererPlugin final : public HdRendererPlugin {
public:
    HdRenderDelegate *CreateRenderDelegate() override { return new HdProbeRenderDelegate(); }
    HdRenderDelegate *CreateRenderDelegate(HdRenderSettingsMap const &s) override {
        return new HdProbeRenderDelegate(s);
    }
    void DeleteRenderDelegate(HdRenderDelegate *d) override { delete d; }
    using HdRendererPlugin::IsSupported;
    bool IsSupported(HdRendererCreateArgs const &, std::string *) const override { return true; }
};

TF_REGISTRY_FUNCTION(TfType) { HdRendererPluginRegistry::Define<HdProbeRendererPlugin>(); }

PXR_NAMESPACE_CLOSE_SCOPE
