#include "lightFilter.h"

#include "diagnostics.h"
#include "renderParam.h"

#include "core/scene/scene.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    ((blockerId, "kma_lfilter_blocker"))
    ((shaderId, "lightFilter:shaderId"))
    ((kmaShaderId, "kma:lightFilter:shaderId"))
    (sphere)
    (capsule)
    (box));

// The filter's parameters: those Hydra hands over as light parameters (the
// attribute name, with or without "inputs:"), else those of the node of its
// material network (UsdImaging's light filter adapter puts the inputs there).
struct FilterParams {
    HdSceneDelegate *sd;
    SdfPath id;
    std::map<TfToken, VtValue> node; // the network's filter node, by input name
    TfToken nodeType;                // its shader id

    explicit FilterParams(HdSceneDelegate *sceneDelegate, SdfPath const &path) : sd(sceneDelegate), id(path) {
        const VtValue resource = sd->GetMaterialResource(id);
        if (!resource.IsHolding<HdMaterialNetworkMap>())
            return;
        const HdMaterialNetworkMap &map = resource.UncheckedGet<HdMaterialNetworkMap>();
        const auto it = map.map.find(HdMaterialTerminalTokens->lightFilter);
        if (it == map.map.end() || it->second.nodes.empty())
            return;
        const HdMaterialNode &n = it->second.nodes.back(); // the terminal
        nodeType = n.identifier;
        node = n.parameters;
    }

    VtValue get(char const *name) const {
        VtValue v = sd->GetLightParamValue(id, TfToken(std::string("inputs:") + name));
        if (v.IsEmpty())
            v = sd->GetLightParamValue(id, TfToken(name));
        if (v.IsEmpty()) {
            const auto it = node.find(TfToken(name));
            if (it != node.end())
                v = it->second;
        }
        return v;
    }
    template <typename T> T value(char const *name, T fallback) const {
        return VtValue::Cast<T>(get(name)).template GetWithDefault<T>(fallback);
    }
    // A string- or token-valued parameter (empty if unset).
    std::string text(VtValue const &v) const {
        if (v.IsHolding<std::string>())
            return v.UncheckedGet<std::string>();
        if (v.IsHolding<TfToken>())
            return v.UncheckedGet<TfToken>().GetString();
        return {};
    }
    std::string text(char const *name) const { return text(get(name)); }
};

// The filter's kind from its shader id: the Karma context's, the universal
// one, else the network node's.
std::string shaderId(FilterParams const &params) {
    for (TfToken const &key : {_tokens->kmaShaderId, _tokens->shaderId}) {
        const std::string id = params.text(params.sd->GetLightParamValue(params.id, key));
        if (!id.empty())
            return id;
    }
    return params.nodeType.GetString();
}

nova::Mat4 toNova(GfMatrix4d const &m) {
    nova::Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = m[i][j];
    return r;
}

void warnOnce(std::string const &what, std::string const &message) {
    static std::mutex mutex;
    static std::set<std::string> warned;
    std::lock_guard lock(mutex);
    if (warned.insert(what).second)
        HdNovaWarn(message);
}

// World -> the blocker's space (docs/design/M11-P4.md, measured with Karma):
// its own prim or another light filter: the inverse of that prim's
// transform; a camera: the inverse of the camera's; a light: the light's
// transform itself (Karma maps world points by it, not by its inverse);
// anything else (geometry, an Xform, an unknown path, "" or Karma's named
// spaces): world space.
GfMatrix4d spaceFromWorld(HdSceneDelegate *sd, SdfPath const &id, GfMatrix4d const &ownXf, std::string const &space) {
    if (space.empty())
        return GfMatrix4d(1.0);
    if (space == id.GetString())
        return ownXf.GetInverse();
    if (!SdfPath::IsValidPathString(space) || space[0] != '/') {
        warnOnce("space:" + space,
                 TfStringPrintf("light filter %s: space '%s' is not supported; Nova uses world space as Karma does "
                                "for spaces it does not know",
                                id.GetText(), space.c_str()));
        return GfMatrix4d(1.0);
    }
    const SdfPath path(space);
    HdRenderIndex &index = sd->GetRenderIndex();
    const auto is = [&](TfToken const &type) { return index.IsSprimTypeSupported(type) && index.GetSprim(type, path); };
    if (is(HdPrimTypeTokens->lightFilter) || is(HdPrimTypeTokens->camera))
        return sd->GetTransform(path).GetInverse();
    for (TfToken const &type : HdLightPrimTypeTokens())
        if (is(type))
            return sd->GetTransform(path);
    return GfMatrix4d(1.0);
}

} // namespace

HdNovaLightFilter::HdNovaLightFilter(SdfPath const &id) : HdSprim(id) {}

HdDirtyBits HdNovaLightFilter::GetInitialDirtyBitsMask() const { return HdLight::AllDirty; }

void HdNovaLightFilter::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) {
    const SdfPath &id = GetId();
    if (id.IsEmpty()) { // fallback prim
        *dirtyBits = HdLight::Clean;
        return;
    }
    auto &param = *static_cast<HdNovaRenderParam *>(renderParam);
    nova::Scene &scene = param.AcquireSceneForEdit();
    const FilterParams params(sd, id);
    const std::string kind = shaderId(params);
    if (kind != _tokens->blockerId.GetString()) {
        // Karma's gobo, barn door, gel and attenuation filters (and others)
        // are not supported yet: the lights they are on stay unfiltered.
        warnOnce("kind:" + kind, TfStringPrintf("light filter %s (%s) is not supported yet and is ignored", id.GetText(),
                                                kind.empty() ? "no shader id" : kind.c_str()));
        if (_inScene)
            scene.removeLightFilter(id.GetString());
        _inScene = false;
        param.NoteSpaceFilter(this, false);
        *dirtyBits = HdLight::Clean;
        return;
    }

    // Karma's blocker; unauthored inputs take the schema's fallbacks.
    nova::LightFilterDesc desc;
    desc.kind = nova::LightFilterKind::Blocker;
    const TfToken shape(params.text("shape"));
    if (shape == _tokens->capsule)
        desc.shape = nova::BlockerShape::Capsule;
    else if (shape == _tokens->box)
        desc.shape = nova::BlockerShape::Box;
    else {
        if (!shape.IsEmpty() && shape != _tokens->sphere)
            warnOnce("shape:" + shape.GetString(),
                     TfStringPrintf("light filter %s: blocker shape '%s' is unknown; using a sphere", id.GetText(),
                                    shape.GetText()));
        desc.shape = nova::BlockerShape::Sphere;
    }
    desc.radius = params.value<float>("radius", 0.5f);
    desc.width = params.value<float>("width", 1.0f);
    desc.height = params.value<float>("height", 1.0f);
    desc.depth = params.value<float>("depth", 1.0f);
    desc.roundedCorner = params.value<float>("roundedcorner", 0.1f);
    desc.falloff = params.value<float>("falloff", 0.0f);
    desc.intensity = params.value<float>("intensity", 1.0f);
    desc.exposure = params.value<float>("exposure", 0.0f);
    const GfVec3f tint = params.value<GfVec3f>("tint", GfVec3f(1.0f));
    desc.tint = {tint[0], tint[1], tint[2]};
    const VtValue invert = params.get("invert");
    desc.invert = invert.IsHolding<bool>() ? invert.UncheckedGet<bool>() : VtValue::Cast<int>(invert).GetWithDefault(0) != 0;

    // The space; unauthored it is the filter's own prim (the LOP's default).
    const GfMatrix4d xf = sd->GetTransform(id);
    const VtValue spaceValue = params.get("space");
    const std::string space = spaceValue.IsEmpty() ? id.GetString() : params.text(spaceValue);
    desc.spaceFromWorld = toNova(spaceFromWorld(sd, id, xf, space));

    // filterLink: ignored, as Karma 22.0.466 ignores it (measured; docs/design/
    // M11-P4.md F6): the filter acts on every object (desc.link stays empty).
    // The core supports a category; honouring UsdLux's collection would add
    // lightFilter to the light-linking scene index's filter types and set
    // desc.link from the category it gives (HdTokens->lightFilterLink).

    scene.setLightFilter(id.GetString(), desc);
    _inScene = true;
    // Another prim's space: re-resolved before every render (RefreshSpace).
    _sceneDelegate = sd;
    _desc = desc;
    _space = space;
    param.NoteSpaceFilter(this, !space.empty() && space[0] == '/' && space != id.GetString());
    *dirtyBits = HdLight::Clean;
}

bool HdNovaLightFilter::RefreshSpace(HdNovaRenderParam &param) {
    if (!_inScene || !_sceneDelegate)
        return false;
    const SdfPath &id = GetId();
    const nova::Mat4 m = toNova(spaceFromWorld(_sceneDelegate, id, _sceneDelegate->GetTransform(id), _space));
    if (std::memcmp(&m, &_desc.spaceFromWorld, sizeof(m)) == 0)
        return false;
    _desc.spaceFromWorld = m;
    param.AcquireSceneForEdit().setLightFilter(id.GetString(), _desc);
    return true;
}

void HdNovaLightFilter::Finalize(HdRenderParam *renderParam) {
    auto &param = *static_cast<HdNovaRenderParam *>(renderParam);
    param.NoteSpaceFilter(this, false);
    if (_inScene) {
        param.AcquireSceneForEdit().removeLightFilter(GetId().GetString());
        _inScene = false;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
