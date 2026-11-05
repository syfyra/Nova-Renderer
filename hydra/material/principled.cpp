#include "principled.h"

#include "vexShaders.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/usd/sdf/assetPath.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// A value of the network being built: a constant, or a node's "out".
struct Val {
    SdfPath node;  // empty: the constant
    VtValue value; // float or GfVec3f / GfVec2f
    bool linked() const { return !node.IsEmpty(); }
};

Val constant(float f) { return {SdfPath(), VtValue(f)}; }
Val constant(GfVec3f const &c) { return {SdfPath(), VtValue(c)}; }

float asFloat(VtValue const &v, float fallback) {
    if (v.IsHolding<float>())
        return v.UncheckedGet<float>();
    if (v.IsHolding<double>())
        return float(v.UncheckedGet<double>());
    if (v.IsHolding<int>())
        return float(v.UncheckedGet<int>());
    if (v.IsHolding<bool>())
        return v.UncheckedGet<bool>() ? 1.0f : 0.0f;
    return fallback;
}

GfVec3f asVec3(VtValue const &v, GfVec3f const &fallback) {
    if (v.IsHolding<GfVec3f>())
        return v.UncheckedGet<GfVec3f>();
    if (v.IsHolding<GfVec3d>())
        return GfVec3f(v.UncheckedGet<GfVec3d>());
    if (v.IsHolding<float>() || v.IsHolding<double>() || v.IsHolding<int>()) {
        const float f = asFloat(v, 0.0f);
        return GfVec3f(f, f, f);
    }
    return fallback;
}

GfVec2f asVec2(VtValue const &v, GfVec2f const &fallback) {
    if (v.IsHolding<GfVec2f>())
        return v.UncheckedGet<GfVec2f>();
    if (v.IsHolding<GfVec2d>())
        return GfVec2f(v.UncheckedGet<GfVec2d>());
    return fallback;
}

std::string asString(VtValue const &v) {
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    if (v.IsHolding<SdfAssetPath>())
        return v.UncheckedGet<SdfAssetPath>().GetAssetPath();
    return {};
}

constexpr float kLumR = 0.299f, kLumG = 0.587f, kLumB = 0.114f; // VEX luminance()

class Builder {
public:
    Builder(HdNovaNetworkSource const &src, SdfPath const &shader, HdNovaNetworkSource &out,
            std::vector<std::string> &warnings)
        : _params(src.net.nodes.at(shader).parameters), _out(out), _warnings(warnings),
          _prefix(shader.GetName() + "_nova_") {}

    // ---- shader parameters (unauthored: the Principled Shader's defaults)
    VtValue const *raw(char const *name) const {
        const auto it = _params.find(TfToken(name));
        return it != _params.end() && !it->second.IsEmpty() ? &it->second : nullptr;
    }
    float f(char const *name, float def) const {
        VtValue const *v = raw(name);
        return v ? asFloat(*v, def) : def;
    }
    int i(char const *name, int def) const { return int(std::lround(f(name, float(def)))); }
    GfVec3f c(char const *name, GfVec3f const &def) const {
        VtValue const *v = raw(name);
        return v ? asVec3(*v, def) : def;
    }
    GfVec2f v2(char const *name, GfVec2f const &def) const {
        VtValue const *v = raw(name);
        return v ? asVec2(*v, def) : def;
    }
    std::string s(char const *name, std::string const &def = std::string()) const {
        VtValue const *v = raw(name);
        return v ? asString(*v) : def;
    }
    void warn(std::string const &w) { _warnings.push_back("Principled Shader: " + w); }

    // ---- nodes
    SdfPath node(char const *id) {
        const SdfPath p(_prefix + std::to_string(_next++));
        _out.net.nodes[p].nodeTypeId = TfToken(id);
        return p;
    }
    void input(SdfPath const &n, char const *name, Val const &v) {
        HdMaterialNode2 &node = _out.net.nodes[n];
        if (v.linked())
            node.inputConnections[TfToken(name)] = {{v.node, TfToken("out")}};
        else
            node.parameters[TfToken(name)] = v.value;
    }
    void param(SdfPath const &n, char const *name, VtValue const &v) { _out.net.nodes[n].parameters[TfToken(name)] = v; }
    Val op(char const *id, char const *a, Val const &va, char const *b, Val const &vb) {
        const SdfPath n = node(id);
        input(n, a, va);
        input(n, b, vb);
        return {n, VtValue()};
    }

    // ---- math with constant folding
    Val mulF(Val const &a, Val const &b) {
        if (!a.linked() && !b.linked())
            return constant(asFloat(a.value, 0.0f) * asFloat(b.value, 0.0f));
        return op("ND_multiply_float", "in1", a, "in2", b);
    }
    Val mulC(Val const &a, Val const &b) {
        if (!a.linked() && !b.linked()) {
            const GfVec3f x = asVec3(a.value, GfVec3f(0.0f)), y = asVec3(b.value, GfVec3f(0.0f));
            return constant(GfVec3f(x[0] * y[0], x[1] * y[1], x[2] * y[2]));
        }
        return op("ND_multiply_color3", "in1", a, "in2", b);
    }
    Val mulCF(Val const &c, Val const &s) {
        if (!c.linked() && !s.linked())
            return constant(asVec3(c.value, GfVec3f(0.0f)) * asFloat(s.value, 0.0f));
        return op("ND_multiply_color3FA", "in1", c, "in2", s);
    }
    // bg * (1 - t) + fg * t
    Val mixC(Val const &bg, Val const &fg, Val const &t) {
        if (!t.linked() && asFloat(t.value, 0.0f) == 0.0f)
            return bg;
        if (!bg.linked() && !fg.linked() && !t.linked()) {
            const float k = asFloat(t.value, 0.0f);
            return constant(asVec3(bg.value, GfVec3f(0.0f)) * (1.0f - k) + asVec3(fg.value, GfVec3f(0.0f)) * k);
        }
        const SdfPath n = node("ND_mix_color3");
        input(n, "bg", bg);
        input(n, "fg", fg);
        input(n, "mix", t);
        return {n, VtValue()};
    }
    // base / luminance(max(base, 0)) (the tint direction of reflect/sheen tints)
    Val tintOf(Val const &base) {
        if (!base.linked()) {
            const GfVec3f b = asVec3(base.value, GfVec3f(0.0f));
            const GfVec3f m(std::max(b[0], 0.0f), std::max(b[1], 0.0f), std::max(b[2], 0.0f));
            const float l = m[0] * kLumR + m[1] * kLumG + m[2] * kLumB;
            return constant(l > 0.0f ? m / l : GfVec3f(0.0f));
        }
        const Val m = op("ND_max_color3", "in1", base, "in2", constant(GfVec3f(0.0f)));
        return op("ND_divide_color3FA", "in1", m, "in2", luminance(m));
    }
    Val luminance(Val const &rgb) {
        const SdfPath v = node("ND_convert_color3_vector3");
        input(v, "in", rgb);
        const SdfPath d = node("ND_dotproduct_vector3");
        input(d, "in1", {v, VtValue()});
        param(d, "in2", VtValue(GfVec3f(kLumR, kLumG, kLumB)));
        return {d, VtValue()};
    }

    // ---- primvars and texture coordinates
    // The first of `names` ("a|b") the mesh has, else `fallback` (bound per
    // mesh: core/scene/scene.cpp).
    Val primvar(char const *type, char const *names, VtValue const &fallback) {
        const SdfPath n = node(type);
        param(n, "geomprop", VtValue(std::string(names)));
        param(n, "default", fallback);
        return {n, VtValue()};
    }
    // uv primvar, else st; transformed by the inverse of uvtrans/uvrot/uvscale.
    Val texcoord() {
        if (_uv.linked())
            return _uv;
        Val t = primvar("ND_geompropvalue_vector2", "uv|st", VtValue(GfVec2f(0.0f)));
        const GfVec2f trans = v2("uvtrans", GfVec2f(0.0f)), scale = v2("uvscale", GfVec2f(1.0f));
        const float rot = f("uvrot", 0.0f);
        if (trans != GfVec2f(0.0f))
            t = op("ND_subtract_vector2", "in1", t, "in2", {SdfPath(), VtValue(trans)});
        if (rot != 0.0f) {
            const SdfPath r = node("ND_rotate2d_vector2");
            input(r, "in", t);
            param(r, "amount", VtValue(-rot));
            t = {r, VtValue()};
        }
        if (scale != GfVec2f(1.0f))
            t = op("ND_divide_vector2", "in1", t, "in2", {SdfPath(), VtValue(scale)});
        _uv = t;
        return _uv;
    }
    // An image's rgba at the texture coordinates (white where it is missing).
    // The texture parameter `name` (its USD value keeps the resolved path), or
    // nullptr when it is empty.
    VtValue const *file(std::string const &name) const {
        VtValue const *v = raw(name.c_str());
        return v && !asString(*v).empty() ? v : nullptr;
    }
    Val image(VtValue const &file, std::string const &wrap, std::string const &colorSpace) {
        const SdfPath n = node("ND_image_color4");
        param(n, "file", file.IsHolding<std::string>() ? VtValue(SdfAssetPath(file.UncheckedGet<std::string>())) : file);
        input(n, "texcoord", texcoord());
        // decal: Houdini's black border (the white default is only for
        // missing files); the translator reads "black" as Karma does.
        const std::string mode = wrap == "streak" ? "clamp" : wrap == "decal" ? "black" : "periodic";
        param(n, "uaddressmode", VtValue(mode));
        param(n, "vaddressmode", VtValue(mode));
        param(n, "default", VtValue(GfVec4f(1.0f)));
        // "auto" (and unknown values): Nova's rules, as Houdini's texture();
        // "linear" means no conversion.
        const std::string cs = TfStringToLower(colorSpace);
        HdNovaParamMeta &meta = _out.meta[n][TfToken("file")];
        if (cs == "linear" || cs == "raw")
            meta.colorSpace = TfToken("raw");
        else if (cs == "srgb")
            meta.colorSpace = TfToken("srgb_texture");
        return {n, VtValue()};
    }
    Val rgbOf(Val const &rgba) {
        const SdfPath n = node("ND_convert_color4_color3");
        input(n, "in", rgba);
        return {n, VtValue()};
    }
    // A float from an rgba texel: 0 luminance, 1-3 r, g, b.
    Val channelOf(Val const &rgba, int channel) {
        if (channel <= 0 || channel > 3)
            return luminance(rgbOf(rgba));
        const SdfPath n = node("ND_extract_color4");
        input(n, "in", rgba);
        param(n, "index", VtValue(channel - 1));
        return {n, VtValue()};
    }

    // ---- texturable parameters (value * lerp(1, tex, clamp(I, 0, 1)) * clamp(I, 1, 1000))
    Val color(char const *name, GfVec3f const &def, bool intensity) {
        const std::string n(name);
        Val v = constant(c(name, def));
        if (!i((n + "_useTexture").c_str(), 0))
            return v;
        VtValue const *tex = file(n + "_texture");
        if (!tex)
            return v;
        Val t = rgbOf(image(*tex, s((n + "_textureWrap").c_str(), "repeat"),
                            s((n + "_textureColorSpace").c_str(), "auto")));
        if (intensity) {
            const float k = f((n + "_textureIntensity").c_str(), 1.0f);
            const float a = std::clamp(k, 0.0f, 1.0f), b = std::clamp(k, 1.0f, 1000.0f);
            if (a != 1.0f)
                t = mixC(constant(GfVec3f(1.0f)), t, constant(a));
            if (b != 1.0f)
                t = mulCF(t, constant(b));
        }
        return mulC(v, t);
    }
    Val scalar(char const *name, float def) {
        const std::string n(name);
        Val v = constant(f(name, def));
        if (!i((n + "_useTexture").c_str(), 0))
            return v;
        VtValue const *tex = file(n + "_texture");
        if (!tex)
            return v;
        const Val t = channelOf(image(*tex, s((n + "_textureWrap").c_str(), "repeat"),
                                      s((n + "_textureColorSpace").c_str(), "linear")),
                                i((n + "_monoChannel").c_str(), 0));
        return mulF(v, t);
    }

    // A normal for a bump or normal map group ("baseBump"/"baseNormal"), or none.
    Val shadingNormal(char const *bump, char const *normal, char const *typeParm) {
        const std::string b(bump), n(normal);
        if (s(typeParm, "normal") == "bump") {
            VtValue const *tex = file(b + "_bumpTexture");
            if (!tex)
                return {};
            const Val height = channelOf(image(*tex, s((b + "_wrap").c_str(), "repeat"),
                                               s((b + "_colorSpace").c_str(), "linear")),
                                         i((b + "_channel").c_str(), 0));
            const SdfPath h = node("ND_add_float");
            input(h, "in1", height);
            param(h, "in2", VtValue(-0.5f));
            const SdfPath out = node("ND_bump_vector3");
            input(out, "height", {h, VtValue()});
            param(out, "scale", VtValue(f((b + "_bumpScale").c_str(), 0.05f)));
            return {out, VtValue()};
        }
        VtValue const *tex = file(n + "_texture");
        if (!tex)
            return {};
        if (s((n + "_vectorSpace").c_str(), "uvtangent") != "uvtangent")
            warn(n + "_vectorSpace '" + s((n + "_vectorSpace").c_str()) + "' is read as uvtangent");
        Val rgb = rgbOf(image(*tex, s((n + "_wrap").c_str(), "repeat"), s((n + "_colorspace").c_str(), "linear")));
        // Normal Space "-1 to 1" (1): signed texels; x * 0.5 + 0.5 undoes the
        // normal map's 2x - 1, before the flips so they negate the signed value.
        const std::string spaceText = s((n + "_space").c_str()); // a menu: int or its token
        const int space = spaceText.empty() ? i((n + "_space").c_str(), 0) : std::atoi(spaceText.c_str());
        if (space == 1)
            rgb = op("ND_add_color3", "in1", mulCF(rgb, constant(0.5f)), "in2", constant(GfVec3f(0.5f)));
        else if (space != 0)
            warn(n + "_space " + std::to_string(space) + " is read as '0 to 1'");
        Val in;
        const bool fx = i((n + "_flipX").c_str(), 0) != 0, fy = i((n + "_flipY").c_str(), 0) != 0;
        if (fx || fy) {
            // (1 - x) for flipped channels.
            const SdfPath m = node("ND_multiply_color3");
            input(m, "in1", rgb);
            param(m, "in2", VtValue(GfVec3f(fx ? -1.0f : 1.0f, fy ? -1.0f : 1.0f, 1.0f)));
            const SdfPath a = node("ND_add_color3");
            input(a, "in1", {m, VtValue()});
            param(a, "in2", VtValue(GfVec3f(fx ? 1.0f : 0.0f, fy ? 1.0f : 0.0f, 0.0f)));
            in = {a, VtValue()};
        } else {
            in = rgb;
        }
        const SdfPath v = node("ND_convert_color3_vector3");
        input(v, "in", in);
        const SdfPath out = node("ND_normalmap_float");
        input(out, "in", {v, VtValue()});
        param(out, "scale", VtValue(f((n + "_scale").c_str(), 1.0f)));
        return {out, VtValue()};
    }

private:
    std::map<TfToken, VtValue> const &_params;
    HdNovaNetworkSource &_out;
    std::vector<std::string> &_warnings;
    std::string _prefix;
    int _next = 0;
    Val _uv;
};

// The IOR whose facing reflectance is `k` times that of `ior` (reflectivity
// scales the facing reflectance, grazing stays at 1).
float scaledReflectanceIor(float ior, float k) {
    const float r = (ior - 1.0f) / (ior + 1.0f);
    const float f0 = std::clamp(k * r * r, 0.0f, 0.999f);
    const float s = std::sqrt(f0);
    return (1.0f + s) / (1.0f - s);
}

void setInput(Builder &b, SdfPath const &n, char const *name, Val const &v) { b.input(n, name, v); }

SdfPath surface(Builder &b) {
    const SdfPath ss = b.node("ND_standard_surface_surfaceshader");
    // Base colour: value x point colour x texture, x albedomult.
    Val base = b.color("basecolor", GfVec3f(0.2f), true);
    if (b.i("basecolor_usePointColor", 1))
        base = b.mulC(base, b.primvar("ND_geompropvalue_color3", "Cd|displayColor", VtValue(GfVec3f(1.0f))));
    if (b.i("basecolor_usePackedColor", 0))
        b.warn("packed colour is not supported (white)");
    base = b.mulCF(base, b.scalar("albedomult", 1.0f));
    setInput(b, ss, "base", constant(1.0f));
    setInput(b, ss, "base_color", base);

    const Val rough = b.scalar("rough", 0.3f);
    setInput(b, ss, "diffuse_roughness", rough);
    setInput(b, ss, "specular_roughness", rough);
    setInput(b, ss, "metalness", b.scalar("metallic", 0.0f));

    // Specular: reflectivity scales the facing reflectance only.
    const Val reflect = b.scalar("reflect", 1.0f);
    const Val ior = b.scalar("ior", 1.5f);
    const Val transparency = b.scalar("transparency", 0.0f);
    const bool refracts = transparency.linked() || asFloat(transparency.value, 0.0f) > 0.0f;
    if (!reflect.linked() && !ior.linked() && !refracts) {
        setInput(b, ss, "specular", constant(asFloat(reflect.value, 1.0f) > 0.0f ? 1.0f : 0.0f));
        setInput(b, ss, "specular_IOR",
                 constant(scaledReflectanceIor(asFloat(ior.value, 1.5f), asFloat(reflect.value, 1.0f))));
    } else {
        setInput(b, ss, "specular", reflect);
        setInput(b, ss, "specular_IOR", ior);
    }
    // Every input below is set whatever its value, so value edits stay
    // parameter edits (no recompile).
    const Val tint = b.scalar("reflecttint", 0.0f);
    const Val baseTint = b.tintOf(base);
    setInput(b, ss, "specular_color", b.mixC(constant(GfVec3f(1.0f)), baseTint, tint));

    // Anisotropy: aspect sqrt(1 - 0.9 |aniso|) (Standard Surface: sqrt(1 -
    // a)), turned by pi * anisodir the other way round; negative swaps the axes.
    const Val aniso = b.scalar("aniso", 0.0f);
    const Val dir = b.mulF(b.scalar("anisodir", 0.0f), constant(-0.5f));
    if (aniso.linked()) {
        setInput(b, ss, "specular_anisotropy", b.mulF(aniso, constant(0.9f)));
        setInput(b, ss, "specular_rotation", dir);
    } else {
        const float a = asFloat(aniso.value, 0.0f);
        setInput(b, ss, "specular_anisotropy", constant(0.9f * std::fabs(a)));
        setInput(b, ss, "specular_rotation",
                 a < 0.0f ? (dir.linked() ? b.op("ND_add_float", "in1", dir, "in2", constant(0.25f))
                                          : constant(asFloat(dir.value, 0.0f) + 0.25f))
                          : dir);
    }

    // Transmission.
    setInput(b, ss, "transmission", transparency);
    setInput(b, ss, "transmission_color", b.color("transcolor", GfVec3f(1.0f), true));
    setInput(b, ss, "transmission_depth", b.scalar("transdist", 0.1f));
    const Val dispersion = b.scalar("dispersion", 0.0f);
    if (dispersion.linked() || asFloat(dispersion.value, 0.0f) != 0.0f)
        b.warn("dispersion is not supported");

    // Subsurface: albedo = base colour, mean free path = ssscolor * sssdist.
    setInput(b, ss, "subsurface", b.scalar("sss", 0.0f));
    setInput(b, ss, "subsurface_color", base);
    setInput(b, ss, "subsurface_radius", b.color("ssscolor", GfVec3f(1.0f), false));
    setInput(b, ss, "subsurface_scale", b.scalar("sssdist", 0.1f));

    // The Principled Shader's sheen (Disney's) and Schlick metal
    // (docs/design/M5.md 3.3; core/bsdf/surface_models.h).
    b.param(ss, "nova:houdiniPrincipled", VtValue(true));
    setInput(b, ss, "sheen", b.scalar("sheen", 0.0f));
    setInput(b, ss, "sheen_color", b.mixC(constant(GfVec3f(1.0f)), baseTint, b.scalar("sheentint", 0.0f)));

    // Coat: Disney's clearcoat, IOR 1.5 with a quarter of its facing
    // reflectance times `coat` (grazing stays 1), roughness 0.1 .. 0.55.
    const Val coat = b.scalar("coat", 0.0f);
    if (coat.linked()) {
        setInput(b, ss, "coat", coat);
        setInput(b, ss, "coat_IOR", constant(scaledReflectanceIor(1.5f, 0.25f)));
    } else {
        const float k = asFloat(coat.value, 0.0f);
        setInput(b, ss, "coat", constant(k > 0.0f ? 1.0f : 0.0f));
        setInput(b, ss, "coat_IOR", constant(scaledReflectanceIor(1.5f, 0.25f * std::fmin(k, 1.0f))));
    }
    const Val coatRough = b.scalar("coatrough", 0.0f);
    setInput(b, ss, "coat_roughness",
             coatRough.linked()
                 ? b.op("ND_add_float", "in1", b.mulF(coatRough, constant(0.45f)), "in2", constant(0.1f))
                 : constant(0.1f + 0.45f * std::clamp(asFloat(coatRough.value, 0.0f), 0.0f, 1.0f)));

    // Emission.
    Val emitColor = b.color("emitcolor", GfVec3f(1.0f), true);
    if (b.i("emitcolor_usePointColor", 0))
        emitColor = b.mulC(emitColor, b.primvar("ND_geompropvalue_color3", "Cd|displayColor", VtValue(GfVec3f(1.0f))));
    setInput(b, ss, "emission", b.scalar("emitint", 0.0f));
    setInput(b, ss, "emission_color", emitColor);

    // Opacity: opac x opacity colour x point alpha.
    Val opacity = b.mulCF(b.color("opaccolor", GfVec3f(1.0f), true), b.scalar("opac", 1.0f));
    if (b.i("opacpointalpha", 1))
        opacity = b.mulCF(opacity, b.primvar("ND_geompropvalue_float", "Alpha|displayOpacity", VtValue(1.0f)));
    if (b.s("alphablendmode", "blend") != "blend")
        b.warn("alpha blend mode '" + b.s("alphablendmode") + "' is read as blend");
    setInput(b, ss, "opacity", opacity);

    // Bump and normal maps.
    if (b.i("baseBumpAndNormal_enable", 0)) {
        const Val n = b.shadingNormal("baseBump", "baseNormal", "baseBumpAndNormal_type");
        if (n.linked()) {
            setInput(b, ss, "normal", n);
            if (!b.i("separateCoatNormals", 0))
                setInput(b, ss, "coat_normal", n);
        }
    }
    if (b.i("separateCoatNormals", 0) && b.i("coatBumpAndNormal_enable", 1)) {
        const Val n = b.shadingNormal("coatBump", "coatNormal", "coatBumpAndNormal_type");
        if (n.linked())
            setInput(b, ss, "coat_normal", n);
    }
    if (b.i("roundedEdge_enable", 0))
        b.warn("rounded edges are not supported");
    return ss;
}

// The displacement shader: texture (value + offset) * scale along N or
// texel * scale as a tangent-space vector, plus the disp/vdisp inputs.
// Empty when nothing displaces.
SdfPath displacement(Builder &b) {
    Val height;
    Val vector;
    if (b.i("dispTex_enable", 0)) {
        VtValue const *tex = b.file("dispTex_texture");
        const std::string type = b.s("dispTex_type", "disp");
        if (tex) {
            const Val rgba = b.image(*tex, b.s("dispTex_wrap", "repeat"), b.s("dispTex_colorSpace", "linear"));
            const float offset = b.f("dispTex_offset", -0.5f), scale = b.f("dispTex_scale", 0.05f);
            // The menu's token is "vectordisp" ("vector" kept for older networks).
            if (type == "vectordisp" || type == "vector") {
                if (b.s("dispTex_vectorSpace", "uvtangent") != "uvtangent")
                    b.warn("vector displacement space '" + b.s("dispTex_vectorSpace") + "' is read as uvtangent");
                // texel * scale, as Houdini: no offset (it only applies along N).
                const SdfPath v = b.node("ND_convert_color3_vector3");
                b.input(v, "in", b.rgbOf(rgba));
                Val texel{v, VtValue()};
                const std::string order = b.s("dispTex_channelOrder", "xyz");
                if (order == "xzy") { // y and z swapped
                    auto channel = [&](int index) {
                        const SdfPath e = b.node("ND_extract_vector3");
                        b.input(e, "in", texel);
                        b.param(e, "index", VtValue(index));
                        return Val{e, VtValue()};
                    };
                    const SdfPath s = b.node("ND_combine3_vector3");
                    b.input(s, "in1", channel(0));
                    b.input(s, "in2", channel(2));
                    b.input(s, "in3", channel(1));
                    texel = {s, VtValue()};
                } else if (order != "xyz") {
                    b.warn("vector displacement channel order '" + order + "' is read as xyz");
                }
                vector = b.op("ND_multiply_vector3FA", "in1", texel, "in2", constant(scale));
            } else {
                const Val shifted = b.op("ND_add_float", "in1", b.channelOf(rgba, b.i("dispTex_channel", 0)), "in2",
                                         constant(offset));
                height = b.mulF(shifted, constant(scale));
            }
        }
    }
    if (b.i("dispInput_enable", 0)) {
        const SdfPath p = b.node("ND_geompropvalue_float");
        b.param(p, "geomprop", VtValue(std::string("disp")));
        b.param(p, "default", VtValue(b.f("disp", 0.0f)));
        const Val d{p, VtValue()};
        height = height.linked() ? b.op("ND_add_float", "in1", height, "in2", d) : d;
        if (b.raw("vdisp"))
            b.warn("the vdisp input is not supported");
    }
    if (b.i("dispNoise_enable", 0))
        b.warn("noise displacement is not supported");
    if (vector.linked()) {
        if (height.linked())
            b.warn("texture vector displacement and the disp input do not combine; the vector wins");
        const SdfPath d = b.node("ND_displacement_vector3");
        b.input(d, "displacement", vector);
        return d;
    }
    if (!height.linked())
        return SdfPath();
    const SdfPath d = b.node("ND_displacement_float");
    b.input(d, "displacement", height);
    return d;
}

} // namespace

bool HdNovaRewritePrincipled(HdNovaNetworkSource const &src, HdNovaNetworkSource *out,
                             std::vector<std::string> *warnings) {
    auto vexOf = [&](TfToken const &terminal, SdfPath *path) {
        const auto t = src.net.terminals.find(terminal);
        if (t == src.net.terminals.end() || !src.net.nodes.count(t->second.upstreamNode))
            return HdNovaVexShader{};
        *path = t->second.upstreamNode;
        const auto info = src.nodeTypeInfo.find(*path);
        return info != src.nodeTypeInfo.end() ? HdNovaVexShaderOf(info->second) : HdNovaVexShader{};
    };
    SdfPath surfacePath, dispPath;
    if (vexOf(HdMaterialTerminalTokens->surface, &surfacePath).kind != HdNovaVexKind::PrincipledSurface)
        return false;
    *out = HdNovaNetworkSource{};
    out->context = src.context;
    out->tangentUv = "uv|st"; // Houdini's uv, else st (the Principled Shader's tangents follow uv)
    Builder surfaceBuilder(src, surfacePath, *out, *warnings);
    out->net.terminals[HdMaterialTerminalTokens->surface] = {surface(surfaceBuilder), TfToken("out")};
    if (vexOf(HdMaterialTerminalTokens->displacement, &dispPath).kind == HdNovaVexKind::PrincipledDisplacement) {
        Builder dispBuilder(src, dispPath, *out, *warnings);
        const SdfPath d = displacement(dispBuilder);
        if (!d.IsEmpty())
            out->net.terminals[HdMaterialTerminalTokens->displacement] = {d, TfToken("out")};
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
