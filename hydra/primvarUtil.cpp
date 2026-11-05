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

#include <cmath>

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

nova::MeshLightSource HdNovaMeshLightSource(HdSceneDelegate *sd, SdfPath const &id) {
    static const TfToken nova("nova:object:lightSource"), karma("karma:object:treat_as_lightsource"),
        diffuse("karma:object:lightsource_diffusescale"), specular("karma:object:lightsource_specularscale"),
        quality("karma:object:lightsource_samplingquality");
    nova::MeshLightSource s;
    bool own = false;
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant)) {
        if (d.name != nova && d.name != karma && d.name != diffuse && d.name != specular && d.name != quality)
            continue;
        int width = 0;
        std::vector<float> v;
        if (!HdNovaPrimvarToFloats(sd->Get(id, d.name), &width, &v) || v.size() != 1 || !std::isfinite(v[0]))
            continue;
        if (d.name == nova) {
            s.mode = int(std::lround(v[0]));
            own = true;
        } else if (d.name == karma) {
            if (!own)
                s.mode = int(std::lround(v[0]));
        } else if (d.name == diffuse) {
            s.diffuse = v[0];
        } else if (d.name == specular) {
            s.specular = v[0];
        } else {
            s.samplingQuality = v[0];
        }
    }
    s.mode = s.mode < 0 ? 0 : s.mode;
    return s;
}

bool HdNovaIsGeometryPrimvar(TfToken const &name) {
    return name == HdTokens->points || name == HdTokens->normals || name == HdInstancerTokens->instanceTranslations ||
           name == HdInstancerTokens->instanceRotations || name == HdInstancerTokens->instanceScales ||
           name == HdInstancerTokens->instanceTransforms;
}

PXR_NAMESPACE_CLOSE_SCOPE
