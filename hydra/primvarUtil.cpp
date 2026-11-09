#include "primvarUtil.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>

#include <pxr/base/tf/stringUtils.h>

#include "diagnostics.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

template <class T> struct Components {
    static constexpr int width = 1;
    static void write(T const &v, float *out) { out[0] = float(v); }
};
template <> struct Components<GfHalf> {
    static constexpr int width = 1;
    static void write(GfHalf const &v, float *out) { out[0] = float(v); }
};
template <> struct Components<bool> {
    static constexpr int width = 1;
    static void write(bool v, float *out) { out[0] = v ? 1.0f : 0.0f; }
};
template <class V, int N> struct VecComponents {
    static constexpr int width = N;
    static void write(V const &v, float *out) {
        for (int k = 0; k < N; ++k)
            out[k] = float(v[k]);
    }
};
template <> struct Components<GfVec2f> : VecComponents<GfVec2f, 2> {};
template <> struct Components<GfVec2d> : VecComponents<GfVec2d, 2> {};
template <> struct Components<GfVec2h> : VecComponents<GfVec2h, 2> {};
template <> struct Components<GfVec2i> : VecComponents<GfVec2i, 2> {};
template <> struct Components<GfVec3f> : VecComponents<GfVec3f, 3> {};
template <> struct Components<GfVec3d> : VecComponents<GfVec3d, 3> {};
template <> struct Components<GfVec3h> : VecComponents<GfVec3h, 3> {};
template <> struct Components<GfVec3i> : VecComponents<GfVec3i, 3> {};
template <> struct Components<GfVec4f> : VecComponents<GfVec4f, 4> {};
template <> struct Components<GfVec4d> : VecComponents<GfVec4d, 4> {};
template <> struct Components<GfVec4h> : VecComponents<GfVec4h, 4> {};
template <> struct Components<GfVec4i> : VecComponents<GfVec4i, 4> {};

template <class T> bool convert(VtValue const &value, int *width, std::vector<float> *out) {
    using C = Components<T>;
    if (value.IsHolding<T>()) {
        *width = C::width;
        out->resize(C::width);
        C::write(value.UncheckedGet<T>(), out->data());
        return true;
    }
    if (value.IsHolding<VtArray<T>>()) {
        VtArray<T> const &a = value.UncheckedGet<VtArray<T>>();
        *width = C::width;
        out->resize(a.size() * C::width);
        for (size_t i = 0; i < a.size(); ++i)
            C::write(a[i], out->data() + i * C::width);
        return true;
    }
    return false;
}

} // namespace

bool HdNovaPrimvarToFloats(VtValue const &value, int *width, std::vector<float> *out) {
    return convert<float>(value, width, out) || convert<GfVec3f>(value, width, out) ||
           convert<GfVec2f>(value, width, out) || convert<GfVec4f>(value, width, out) ||
           convert<double>(value, width, out) || convert<int>(value, width, out) ||
           convert<GfHalf>(value, width, out) || convert<bool>(value, width, out) ||
           convert<unsigned int>(value, width, out) || convert<int64_t>(value, width, out) ||
           convert<unsigned char>(value, width, out) || convert<GfVec2d>(value, width, out) ||
           convert<GfVec2h>(value, width, out) || convert<GfVec2i>(value, width, out) ||
           convert<GfVec3d>(value, width, out) || convert<GfVec3h>(value, width, out) ||
           convert<GfVec3i>(value, width, out) || convert<GfVec4d>(value, width, out) ||
           convert<GfVec4h>(value, width, out) || convert<GfVec4i>(value, width, out);
}

std::string HdNovaObjectLpeTag(HdSceneDelegate *sd, SdfPath const &id) {
    static const TfToken name("karma:object:lpetag");
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant)) {
        if (d.name != name)
            continue;
        const VtValue v = sd->Get(id, name);
        if (v.IsHolding<std::string>())
            return v.UncheckedGet<std::string>();
        if (v.IsHolding<TfToken>())
            return v.UncheckedGet<TfToken>().GetString();
        if (v.IsHolding<VtArray<std::string>>() && v.UncheckedGet<VtArray<std::string>>().size() == 1)
            return v.UncheckedGet<VtArray<std::string>>()[0];
    }
    return {};
}

nova::MeshLightSource HdNovaMeshLightSource(HdSceneDelegate *sd, SdfPath const &id, HdNovaObjectDefaults const &defaults) {
    static const TfToken nova("nova:object:lightSource"), karma("karma:object:treat_as_lightsource"),
        diffuse("karma:object:lightsource_diffusescale"), specular("karma:object:lightsource_specularscale"),
        quality("karma:object:lightsource_samplingquality");
    nova::MeshLightSource s;
    // The render settings' Karma values first; the prim's own replace them.
    if (defaults.lightSource >= 0)
        s.mode = defaults.lightSource;
    if (defaults.lightDiffuse >= 0.0f)
        s.diffuse = defaults.lightDiffuse;
    if (defaults.lightSpecular >= 0.0f)
        s.specular = defaults.lightSpecular;
    if (defaults.lightQuality >= 0.0f)
        s.samplingQuality = defaults.lightQuality;
    bool own = false;
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant)) {
        if (d.name != nova && d.name != karma && d.name != diffuse && d.name != specular && d.name != quality)
            continue;
        const VtValue value = sd->Get(id, d.name);
        if (d.name == nova || d.name == karma) {
            const int mode = HdNovaMenuIndex(value, {"No", "Yes", "Auto"});
            if (mode < 0)
                continue;
            if (d.name == nova) {
                s.mode = mode;
                own = true;
            } else if (!own) {
                s.mode = mode;
            }
            continue;
        }
        int width = 0;
        std::vector<float> v;
        if (!HdNovaPrimvarToFloats(value, &width, &v) || v.size() != 1 || !std::isfinite(v[0]))
            continue;
        if (d.name == diffuse)
            s.diffuse = v[0];
        else if (d.name == specular)
            s.specular = v[0];
        else
            s.samplingQuality = v[0];
    }
    s.mode = s.mode < 0 ? 0 : s.mode;
    return s;
}

VtValue HdNovaConstantPrimvar(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name) {
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant))
        if (d.name == name)
            return sd->Get(id, name);
    return VtValue();
}

nova::RayLimitOverrides HdNovaObjectOverrides(HdSceneDelegate *sd, SdfPath const &id) {
    static const TfToken novaLimits[nova::kLimitClasses] = {
        TfToken("nova:object:diffuseLimit"), TfToken("nova:object:reflectLimit"), TfToken("nova:object:refractLimit"),
        TfToken("nova:object:sssLimit"), TfToken("nova:object:volumeLimit")};
    static const TfToken karmaLimits[nova::kLimitClasses] = {
        TfToken("karma:object:diffuselimit"), TfToken("karma:object:reflectlimit"),
        TfToken("karma:object:refractlimit"), TfToken("karma:object:ssslimit"), TfToken("karma:object:volumelimit")};
    static const TfToken thinWall("nova:object:thinWallAtRefractLimit"),
        karmaThinWall("karma:object:thinwallatrefractlimit"), fake("nova:object:fakeCaustics"),
        caustics("karma:object:causticsenable"), fakeBsdf("karma:object:fakecausticsbsdfenable"),
        fakeColor("karma:object:fakecausticscolor"), fakeOpacity("karma:object:fakecausticsopacity"),
        fakeDarken("karma:object:fakecausticsfresneldarken"), internal("nova:object:internalReflection"),
        karmaInternal("karma:object:internalreflectenable"), clamp("nova:object:causticsRoughnessClamp"),
        karmaClamp("karma:object:causticsroughnessclamp");
    // One pass over the constant primvars; values fetched on demand.
    std::vector<TfToken> names;
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant))
        if (TfStringStartsWith(d.name.GetString(), "nova:object:") || TfStringStartsWith(d.name.GetString(), "karma:object:"))
            names.push_back(d.name);
    const auto value = [&](TfToken const &name) {
        return std::find(names.begin(), names.end(), name) != names.end() ? sd->Get(id, name) : VtValue();
    };
    const auto number = [&](TfToken const &nova, TfToken const &karma) -> std::optional<double> {
        if (const auto v = HdNovaNumber(value(nova)))
            return v;
        return HdNovaNumber(value(karma));
    };
    const auto flag = [&](TfToken const &nova, TfToken const &karma) -> std::optional<bool> {
        if (const auto v = HdNovaFlag(value(nova)))
            return v;
        return HdNovaFlag(value(karma));
    };
    nova::RayLimitOverrides o;
    if (names.empty())
        return o;
    for (int k = 0; k < nova::kLimitClasses; ++k)
        if (const auto v = number(novaLimits[k], karmaLimits[k]))
            o.limit[k] = float(std::max(*v, 0.0));
    if (const auto v = flag(thinWall, karmaThinWall))
        o.thinWallAtRefractLimit = *v ? 1 : 0;
    if (const auto v = HdNovaFlag(value(fake))) {
        o.fakeCaustics = *v ? 1 : 0;
        o.fakeKarma = 0;
    } else if (const auto k = HdNovaFlag(value(caustics))) {
        o.fakeCaustics = *k ? 0 : 1;
        o.fakeKarma = 1;
    }
    if (const auto v = HdNovaFlag(value(fakeBsdf)))
        o.fakeBsdf = *v ? 1 : 0;
    const auto color = [&](TfToken const &name, float out[3], float hi) {
        const VtValue v = value(name);
        int width = 0;
        std::vector<float> c;
        if (!HdNovaPrimvarToFloats(v, &width, &c) || c.empty())
            return;
        for (int i = 0; i < 3; ++i) {
            const float x = c[size_t(width >= 3 ? i : 0)];
            out[i] = std::isfinite(x) ? std::clamp(x, 0.0f, hi) : 1.0f;
        }
    };
    color(fakeColor, o.fakeColor, 1e30f);
    color(fakeOpacity, o.fakeOpacity, 1.0f);
    if (const auto v = HdNovaNumber(value(fakeDarken)))
        o.fakeDarken = float(std::clamp(*v, 0.0, 1.0));
    if (const auto v = flag(internal, karmaInternal))
        o.internalReflection = *v ? 1 : 0;
    if (const auto v = number(clamp, karmaClamp))
        o.causticsRoughnessClamp = float(std::clamp(*v, 0.0, 1.0));
    return o;
}

namespace {

// A string or token value (single-element string arrays too); false otherwise.
bool textOf(VtValue const &v, std::string &text) {
    if (v.IsHolding<std::string>())
        text = v.UncheckedGet<std::string>();
    else if (v.IsHolding<TfToken>())
        text = v.UncheckedGet<TfToken>().GetString();
    else if (v.IsHolding<VtArray<std::string>>() && v.UncheckedGet<VtArray<std::string>>().size() == 1)
        text = v.UncheckedGet<VtArray<std::string>>()[0];
    else if (v.IsHolding<VtArray<TfToken>>() && v.UncheckedGet<VtArray<TfToken>>().size() == 1)
        text = v.UncheckedGet<VtArray<TfToken>>()[0].GetString();
    else
        return false;
    return true;
}

// Once per distinct message and process.
void warnOnce(std::string const &message) {
    static std::mutex mutex;
    static std::set<std::string> warned;
    std::lock_guard lock(mutex);
    if (warned.insert(message).second)
        HD_NOVA_WARN("%s", message.c_str());
}

} // namespace

int HdNovaRenderVisibilityValue(VtValue const &value, std::string const &where) {
    std::string text;
    if (value.IsEmpty() || !textOf(value, text)) {
        if (!value.IsEmpty())
            warnOnce(where + ": karma:object:rendervisibility is not a string; it is ignored");
        return -1;
    }
    uint8_t mask = nova::kVisibleAll;
    std::string error;
    if (!nova::parseRenderVisibility(text, mask, &error)) {
        warnOnce(where + ": karma:object:rendervisibility \"" + text + "\" (" + error + ") is ignored");
        return -1;
    }
    return mask;
}

int HdNovaHoldoutModeValue(VtValue const &value, std::string const &where) {
    if (value.IsEmpty())
        return -1;
    std::string text;
    if (!textOf(value, text)) {
        int width = 0;
        std::vector<float> v;
        if (HdNovaPrimvarToFloats(value, &width, &v) && v.size() == 1 && std::isfinite(v[0]))
            text = std::to_string(std::lround(v[0]));
    }
    nova::HoldoutMode mode;
    if (!nova::parseHoldoutMode(text, mode)) {
        warnOnce(where + ": karma:object:holdoutmode \"" + text + "\" is not None, Matte or Background; it is ignored");
        return -1;
    }
    return int(mode);
}

nova::ObjectVisibility HdNovaObjectVisibility(HdSceneDelegate *sd, SdfPath const &id) {
    static const TfToken visibility("karma:object:rendervisibility"), holdout("karma:object:holdoutmode");
    nova::ObjectVisibility o;
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant)) {
        if (d.name == visibility)
            o.mask = int16_t(HdNovaRenderVisibilityValue(sd->Get(id, d.name), id.GetString()));
        else if (d.name == holdout)
            o.holdout = int8_t(HdNovaHoldoutModeValue(sd->Get(id, d.name), id.GetString()));
    }
    return o;
}

nova::OutlineOverrides HdNovaOutlineValues(std::function<VtValue(TfToken const &)> const &get) {
    static const TfToken enable("karma:object:outline_enable"), radius("karma:object:outline_radius"),
        contour("karma:object:outline_contour"), crease("karma:object:outline_crease"),
        creaseThreshold("karma:object:outline_creasethreshold"), depth("karma:object:outline_depth"),
        depthThreshold("karma:object:outline_depththreshold");
    nova::OutlineOverrides o;
    const auto flag = [&](TfToken const &name, int8_t &out) {
        if (const auto v = HdNovaFlag(get(name)))
            out = *v ? 1 : 0;
    };
    const auto number = [&](TfToken const &name, float &out) {
        if (const auto v = HdNovaNumber(get(name)); v && std::isfinite(*v))
            out = float(std::max(*v, 0.0));
    };
    flag(enable, o.enable);
    flag(contour, o.contour);
    flag(crease, o.crease);
    flag(depth, o.depth);
    number(radius, o.radius);
    number(creaseThreshold, o.creaseThreshold);
    number(depthThreshold, o.depthThreshold);
    return o;
}

nova::OutlineOverrides HdNovaObjectOutline(HdSceneDelegate *sd, SdfPath const &id) {
    std::unordered_map<TfToken, VtValue, TfToken::HashFunctor> values;
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant))
        if (TfStringStartsWith(d.name.GetString(), "karma:object:outline_"))
            values[d.name] = sd->Get(id, d.name);
    if (values.empty())
        return {};
    return HdNovaOutlineValues([&](TfToken const &name) {
        const auto it = values.find(name);
        return it == values.end() ? VtValue() : it->second;
    });
}

bool HdNovaIsGeometryPrimvar(TfToken const &name) {
    return name == HdTokens->points || name == HdTokens->normals || name == HdInstancerTokens->instanceTranslations ||
           name == HdInstancerTokens->instanceRotations || name == HdInstancerTokens->instanceScales ||
           name == HdInstancerTokens->instanceTransforms;
}

PXR_NAMESPACE_CLOSE_SCOPE
