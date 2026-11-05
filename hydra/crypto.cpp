#include "crypto.h"

#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/primOriginSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneIndex.h>

PXR_NAMESPACE_OPEN_SCOPE

uint32_t HdNovaCryptoNames::Note(std::string const &source, std::string const &name) {
    const uint32_t id = nova::cryptoHash(name);
    std::lock_guard lock(_mutex);
    _names[source].emplace(id, name);
    return id;
}

std::string HdNovaCryptoNames::Find(std::string const &source, uint32_t id) const {
    std::lock_guard lock(_mutex);
    const auto s = _names.find(source);
    if (s == _names.end())
        return {};
    const auto n = s->second.find(id);
    return n != s->second.end() ? n->second : std::string();
}

std::string HdNovaCryptoObjectName(HdSceneDelegate *sd, SdfPath const &id, SdfPath const &instancerId) {
    HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex();
    if (!si)
        return id.GetString();
    SdfPath path;
    if (HdPrimOriginSchema origin = HdPrimOriginSchema::GetFromParent(si->GetPrim(id).dataSource))
        path = origin.GetOriginPath(HdPrimOriginSchemaTokens->scenePath);
    if (path.IsEmpty())
        return id.GetString();
    if (path.IsAbsolutePath() || instancerId.IsEmpty())
        return path.IsAbsolutePath() ? path.GetString() : id.GetString();
    // A prototype prim of native instancing: relative to its instances.
    if (HdInstancerTopologySchema topo = HdInstancerTopologySchema::GetFromParent(si->GetPrim(instancerId).dataSource))
        if (HdPathArrayDataSourceHandle locations = topo.GetInstanceLocations()) {
            const VtArray<SdfPath> paths = locations->GetTypedValue(0.0f);
            if (!paths.empty())
                return paths[0].AppendPath(path).GetString();
        }
    return id.GetString();
}

namespace {

// The values of a string/token/int primvar as strings (none for other types).
bool primvarStrings(VtValue const &v, std::vector<std::string> &out) {
    out.clear();
    if (v.IsHolding<std::string>())
        out.push_back(v.UncheckedGet<std::string>());
    else if (v.IsHolding<TfToken>())
        out.push_back(v.UncheckedGet<TfToken>().GetString());
    else if (v.IsHolding<int>())
        out.push_back(std::to_string(v.UncheckedGet<int>()));
    else if (v.IsHolding<int64_t>())
        out.push_back(std::to_string(v.UncheckedGet<int64_t>()));
    else if (v.IsHolding<VtArray<std::string>>())
        for (std::string const &s : v.UncheckedGet<VtArray<std::string>>())
            out.push_back(s);
    else if (v.IsHolding<VtArray<TfToken>>())
        for (TfToken const &t : v.UncheckedGet<VtArray<TfToken>>())
            out.push_back(t.GetString());
    else if (v.IsHolding<VtArray<int>>())
        for (int i : v.UncheckedGet<VtArray<int>>())
            out.push_back(std::to_string(i));
    else if (v.IsHolding<VtArray<int64_t>>())
        for (int64_t i : v.UncheckedGet<VtArray<int64_t>>())
            out.push_back(std::to_string(i));
    else
        return false;
    return true;
}

} // namespace

std::vector<nova::CryptoAttribute> HdNovaCryptoAttributes(HdSceneDelegate *sd, SdfPath const &id,
                                                          HdNovaCryptoNames &names) {
    std::vector<nova::CryptoAttribute> out;
    std::vector<std::string> values;
    for (HdInterpolation interp : {HdInterpolationConstant, HdInterpolationUniform}) {
        for (HdPrimvarDescriptor const &d : sd->GetPrimvarDescriptors(id, interp)) {
            if (!primvarStrings(sd->Get(id, d.name), values) || values.empty())
                continue;
            nova::CryptoAttribute a;
            a.name = d.name.GetString();
            a.uniform = interp == HdInterpolationUniform;
            if (!a.uniform)
                values.resize(1);
            a.ids.reserve(values.size());
            std::unordered_map<std::string, uint32_t> seen; // per-face values repeat
            for (std::string const &value : values) {
                auto [it, fresh] = seen.try_emplace(value, 0u);
                if (fresh)
                    it->second = names.Note(a.name, value);
                a.ids.push_back(it->second);
            }
            out.push_back(std::move(a));
        }
    }
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
