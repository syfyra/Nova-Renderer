#include "networkSource.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>

#include <sstream>

PXR_NAMESPACE_OPEN_SCOPE

TfTokenVector const &HdNovaMaterialRenderContexts() {
    // Karma's order (docs/design/M5.md D1). A VEX network Nova cannot run
    // fails its translation, so the universal preview still renders.
    static const TfTokenVector contexts{TfToken("nova"), TfToken("kma"), TfToken("mtlx"), TfToken("vex")};
    return contexts;
}

bool HdNovaReadNetwork(HdSceneIndexBaseRefPtr const &sceneIndex, SdfPath const &id, TfToken const &context,
                       HdNovaNetworkSource *out) {
    if (!sceneIndex)
        return false;
    const HdSceneIndexPrim prim = sceneIndex->GetPrim(id);
    const HdMaterialSchema material = HdMaterialSchema::GetFromParent(prim.dataSource);
    if (!material)
        return false;
    const HdMaterialNetworkSchema network = material.GetMaterialNetwork(context);
    if (!network)
        return false;
    *out = HdNovaNetworkSource{};
    out->context = context;

    const HdMaterialNodeContainerSchema nodes = network.GetNodes();
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
            HdNovaParamMeta m;
            if (HdTokenDataSourceHandle cs = ps.GetColorSpace())
                m.colorSpace = cs->GetTypedValue(0.0f);
            if (HdTokenDataSourceHandle tn = ps.GetTypeName())
                m.typeName = tn->GetTypedValue(0.0f);
            if (!m.colorSpace.IsEmpty() || !m.typeName.IsEmpty())
                out->meta[path][p] = m;
        }
        const HdMaterialConnectionVectorContainerSchema connections = node.GetInputConnections();
        for (TfToken const &input : connections.GetNames()) {
            const HdMaterialConnectionVectorSchema vec = connections.Get(input);
            for (size_t i = 0; i < vec.GetNumElements(); ++i) {
                const HdMaterialConnectionSchema c = vec.GetElement(i);
                HdTokenDataSourceHandle up = c.GetUpstreamNodePath(), o = c.GetUpstreamNodeOutputName();
                if (up)
                    n2.inputConnections[input].push_back(
                        {SdfPath(up->GetTypedValue(0.0f).GetString()), o ? o->GetTypedValue(0.0f) : TfToken()});
            }
        }
        if (HdContainerDataSourceHandle info = node.GetNodeTypeInfo()) {
            VtDictionary d;
            for (TfToken const &k : info->GetNames())
                if (HdSampledDataSourceHandle v = HdSampledDataSource::Cast(info->Get(k)))
                    d[k] = v->GetValue(0.0f);
            out->nodeTypeInfo[path] = d;
        }
    }
    const HdMaterialConnectionContainerSchema terminals = network.GetTerminals();
    for (TfToken const &t : terminals.GetNames()) {
        const HdMaterialConnectionSchema c = terminals.Get(t);
        HdTokenDataSourceHandle up = c.GetUpstreamNodePath(), o = c.GetUpstreamNodeOutputName();
        if (up && !up->GetTypedValue(0.0f).IsEmpty()) // an empty path disables a terminal
            out->net.terminals[t] = {SdfPath(up->GetTypedValue(0.0f).GetString()),
                                     o ? o->GetTypedValue(0.0f) : TfToken()};
    }
    if (const HdSampledDataSourceContainerSchema config = network.GetConfig())
        for (TfToken const &k : config.GetNames())
            if (HdSampledDataSourceHandle v = config.Get(k))
                out->net.config[k] = v->GetValue(0.0f);
    return true;
}

bool HdNovaReadNetworkFromDelegate(HdSceneDelegate *sd, SdfPath const &id, HdNovaNetworkSource *out) {
    const VtValue resource = sd->GetMaterialResource(id);
    if (!resource.IsHolding<HdMaterialNetworkMap>())
        return false;
    *out = HdNovaNetworkSource{};
    bool isVolume = false;
    out->net = HdConvertToHdMaterialNetwork2(resource.UncheckedGet<HdMaterialNetworkMap>(), &isVolume);
    // Pseudo-parameters "colorSpace:<p>" and "typeName:<p>" become metadata.
    for (auto &[path, node] : out->net.nodes) {
        for (auto it = node.parameters.begin(); it != node.parameters.end();) {
            const std::string name = it->first.GetString();
            const bool cs = TfStringStartsWith(name, "colorSpace:"), tn = TfStringStartsWith(name, "typeName:");
            if (!cs && !tn) {
                ++it;
                continue;
            }
            const TfToken param(name.substr(name.find(':') + 1));
            HdNovaParamMeta &m = out->meta[path][param];
            const TfToken value = it->second.IsHolding<TfToken>() ? it->second.UncheckedGet<TfToken>()
                                  : it->second.IsHolding<std::string>() ? TfToken(it->second.UncheckedGet<std::string>())
                                                                        : TfToken();
            (cs ? m.colorSpace : m.typeName) = value;
            it = node.parameters.erase(it);
        }
    }
    return !out->net.terminals.empty();
}

std::string HdNovaDescribeNetwork(HdNovaNetworkSource const &src) {
    std::ostringstream s;
    s << "context '" << src.context.GetString() << "'\n";
    for (auto const &[name, c] : src.net.terminals)
        s << "  terminal " << name.GetString() << " <- " << c.upstreamNode.GetString() << "."
          << c.upstreamOutputName.GetString() << "\n";
    for (auto const &[path, node] : src.net.nodes) {
        s << "  node '" << path.GetString() << "' id='" << node.nodeTypeId.GetString() << "'\n";
        for (auto const &[p, v] : node.parameters)
            s << "    " << p.GetString() << " = " << TfStringify(v) << "\n";
        for (auto const &[input, conns] : node.inputConnections)
            for (HdMaterialConnection2 const &c : conns)
                s << "    " << input.GetString() << " <- " << c.upstreamNode.GetString() << "."
                  << c.upstreamOutputName.GetString() << "\n";
        const auto info = src.nodeTypeInfo.find(path);
        if (info != src.nodeTypeInfo.end())
            for (auto const &[k, v] : info->second) {
                std::string text = TfStringify(v);
                if (text.size() > 120)
                    text = text.substr(0, 120) + "... (" + std::to_string(text.size()) + " chars)";
                s << "    [typeInfo] " << k << " = " << text << "\n";
            }
    }
    return s.str();
}

PXR_NAMESPACE_CLOSE_SCOPE
