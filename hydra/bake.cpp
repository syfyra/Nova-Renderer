#include "bake.h"

#include "renderBuffer.h"
#include "renderParam.h"

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/js/json.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

std::string text(VtValue const &v) {
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    if (v.IsHolding<SdfPath>())
        return v.UncheckedGet<SdfPath>().GetString();
    return {};
}

struct Settings {
    HdNovaSettingGetter const &get;
    VtValue raw(char const *name) const { return get(TfToken(std::string("karma:global:") + name)); }
    std::string str(char const *name, std::string const &fallback = {}) const {
        const VtValue v = raw(name);
        return v.IsEmpty() ? fallback : text(v);
    }
    float num(char const *name, float fallback) const {
        const VtValue v = raw(name);
        for (VtValue c : {VtValue::Cast<float>(v), VtValue::Cast<double>(v), VtValue::Cast<int>(v)}) {
            if (c.IsHolding<float>())
                return c.UncheckedGet<float>();
            if (c.IsHolding<double>())
                return float(c.UncheckedGet<double>());
            if (c.IsHolding<int>())
                return float(c.UncheckedGet<int>());
        }
        return fallback;
    }
    int integer(char const *name, int fallback) const { return int(std::lround(num(name, float(fallback)))); }
    bool flag(char const *name, bool fallback) const {
        const VtValue v = VtValue::Cast<bool>(raw(name));
        return v.IsHolding<bool>() ? v.UncheckedGet<bool>() : num(name, fallback ? 1.0f : 0.0f) != 0.0f;
    }
    void vec3(char const *name, float *out) const {
        const VtValue v = raw(name);
        if (v.IsHolding<GfVec3f>())
            for (int i = 0; i < 3; ++i)
                out[i] = v.UncheckedGet<GfVec3f>()[i];
        else if (v.IsHolding<GfVec3d>())
            for (int i = 0; i < 3; ++i)
                out[i] = float(v.UncheckedGet<GfVec3d>()[i]);
    }
};

// Meshes named by a prim path (the prim and its descendants); empty or
// "*": every mesh except `exclude`.
std::vector<nova::MeshId> meshes(HdNovaRenderParam const &param, std::string const &path,
                                 std::vector<nova::MeshId> const &exclude) {
    if (path.empty() || path == "*") {
        std::vector<nova::MeshId> all = param.MeshesUnder(SdfPath::AbsoluteRootPath());
        all.erase(std::remove_if(all.begin(), all.end(),
                                 [&](nova::MeshId id) {
                                     return std::find(exclude.begin(), exclude.end(), id) != exclude.end();
                                 }),
                  all.end());
        return all;
    }
    if (!SdfPath::IsValidPathString(path))
        return {};
    return param.MeshesUnder(SdfPath(path));
}

} // namespace

bool HdNovaBakeRequested(HdNovaSettingGetter const &get) {
    return !text(get(TfToken("karma:global:bake_low"))).empty();
}

nova::BakeSettings HdNovaParseBakeSettings(HdNovaSettingGetter const &get, HdNovaRenderParam const &param,
                                           std::vector<std::string> &warnings) {
    const Settings s{get};
    nova::BakeSettings b;
    const std::string mode = s.str("bake_mode", "trace");
    b.mode = mode == "single" ? nova::BakeMode::Single : mode == "uvmatch" ? nova::BakeMode::UvMatch : nova::BakeMode::Trace;
    if (mode != "single" && mode != "uvmatch" && mode != "trace")
        warnings.push_back("karma:global:bake_mode \"" + mode + "\" is not a baking mode; Nova traces");
    const std::string low = s.str("bake_low");
    b.low = meshes(param, low, {});
    if (b.low.empty())
        warnings.push_back("the bake mesh " + low + " has no polygon mesh; nothing is baked");
    const std::string cageMode = s.str("bake_cagemode");
    b.cageMode = cageMode == "cagetopology" ? nova::BakeCage::Topology
                 : cageMode == "cageuv"     ? nova::BakeCage::Uv
                 : cageMode == "cageoffset" ? nova::BakeCage::Offset
                                            : nova::BakeCage::None;
    if (b.cageMode == nova::BakeCage::Topology || b.cageMode == nova::BakeCage::Uv) {
        const std::string cage = s.str("bake_cage");
        b.cage = cage.empty() ? std::vector<nova::MeshId>{} : meshes(param, cage, {});
        if (b.cage.empty())
            warnings.push_back("the cage mesh " + cage + " is missing; rays start at the offset cage");
    }
    if (b.mode == nova::BakeMode::UvMatch) {
        const std::string high = s.str("bake_high");
        b.high = meshes(param, high, b.low);
    }
    b.uv = s.str("bake_uv", "st");
    b.udim = s.integer("bake_udim", 1001);
    b.mikkt = s.flag("bake_use_mikkt", true);
    b.traceBias = s.num("bake_trace_bias", b.traceBias);
    b.traceMax = s.num("bake_trace_max", b.traceMax);
    const std::string biasOpt = s.str("bake_trace_bias_opt", "global"), maxOpt = s.str("bake_trace_max_opt", "global");
    if (biasOpt == "attribute")
        b.traceBiasAttribute = s.str("bake_trace_bias_attr");
    else if (biasOpt == "texture")
        warnings.push_back("the trace bias texture is not supported; the global trace bias applies");
    if (maxOpt == "attribute")
        b.traceMaxAttribute = s.str("bake_trace_max_attr");
    else if (maxOpt == "texture")
        warnings.push_back("the trace max distance texture is not supported; the global distance applies");
    if (!s.str("bake_trace_skew_tex").empty())
        warnings.push_back("the trace skew texture is not supported; rays follow the normal");
    if (const std::string ts = s.str("bake_traceset", "*"); ts != "*" && !ts.empty())
        warnings.push_back("bake trace sets (" + ts + ") are not supported; every visible object is baked");
    b.ocSamples = s.integer("bake_oc_samples", b.ocSamples);
    b.ocMaxDist = s.num("bake_oc_maxdist", b.ocMaxDist);
    b.ocBias = s.num("bake_oc_raybias", b.ocBias);
    b.cuSamples = s.integer("bake_cu_samples", b.cuSamples);
    b.cuScale = s.num("bake_cu_scale", b.cuScale);
    b.cuDist = s.num("bake_cu_sdist", b.cuDist);
    b.cuBias = s.num("bake_cu_bias", b.cuBias);
    b.cvSamples = s.integer("bake_cv_samples", b.cvSamples);
    b.cvDist = s.num("bake_cv_maxdist", b.cvDist);
    b.cvBias = s.num("bake_cv_bias", b.cvBias);
    b.thSamples = s.integer("bake_th_samples", b.thSamples);
    b.egSamples = s.integer("bake_eg_samples", b.egSamples);
    b.egRadius = s.num("bake_eg_radius", b.egRadius);
    b.egCusp = s.num("bake_eg_cuspangle", b.egCusp);
    b.egMode = s.integer("bake_eg_mode", b.egMode);
    b.rnSamples = s.integer("bake_rn_samples", b.rnSamples);
    b.rnRadius = s.num("bake_rn_radius", b.rnRadius);
    b.rnCusp = s.num("bake_rn_cuspangle", b.rnCusp);
    b.rnMode = s.integer("bake_rn_mode", b.rnMode);
    const std::string box = s.str("bake_pbox_mode", "world");
    b.box = box == "bakemesh" ? nova::BakeBox::BakeMesh
            : box == "scene"  ? nova::BakeBox::Scene
            : box == "user"   ? nova::BakeBox::User
                              : nova::BakeBox::World;
    b.boxUniform = s.flag("bake_pbox_uniform", true);
    s.vec3("bake_pbox_min", b.boxMin);
    s.vec3("bake_pbox_max", b.boxMax);
    b.lighting = s.flag("bake_use_lighting", false);
    return b;
}

std::vector<HdNovaBakeFilter> HdNovaParseBakeFilters(std::string const &json, std::vector<std::string> &warnings) {
    std::vector<HdNovaBakeFilter> first, last;
    if (json.empty())
        return {};
    const JsValue root = JsParseString(json);
    if (!root.IsArray())
        return {};
    const auto number = [](JsValue const &v, float fallback) {
        return v.IsReal() ? float(v.GetReal()) : v.IsInt() ? float(v.GetInt()) : fallback;
    };
    const auto triple = [&](JsObject const &o, char const *key, float *out) {
        const auto it = o.find(key);
        if (it == o.end())
            return;
        if (it->second.IsArray()) {
            JsArray const &a = it->second.GetJsArray();
            for (size_t i = 0; i < 3 && i < a.size(); ++i)
                out[i] = number(a[i], out[i]);
        } else {
            const float x = number(it->second, out[0]);
            out[0] = out[1] = out[2] = x;
        }
    };
    for (JsValue const &entry : root.GetJsArray()) {
        if (!entry.IsArray() || entry.GetJsArray().empty() || !entry.GetJsArray()[0].IsString())
            continue;
        std::string const &name = entry.GetJsArray()[0].GetString();
        static const JsObject empty;
        JsObject const &o = entry.GetJsArray().size() > 1 && entry.GetJsArray()[1].IsObject()
                                ? entry.GetJsArray()[1].GetJsObject()
                                : empty;
        const auto boolean = [&](char const *key) {
            const auto it = o.find(key);
            return it != o.end() && ((it->second.IsBool() && it->second.GetBool()) || number(it->second, 0.0f) != 0.0f);
        };
        HdNovaBakeFilter f;
        bool runLast = boolean("runlast");
        if (name == "shift") {
            f.kind = HdNovaBakeFilter::Kind::Shift;
            triple(o, "scale", f.shift.scale);
            triple(o, "offset", f.shift.offset);
            f.shift.premultiplied = boolean("ispremultipliedalpha");
            if (const auto it = o.find("swizzle"); it != o.end() && it->second.IsString()) {
                std::string const &sw = it->second.GetString();
                for (size_t i = 0; i < 3 && i < sw.size(); ++i)
                    f.shift.swizzle[i] = sw[i] == 'y' ? 1 : sw[i] == 'z' ? 2 : 0;
            }
            if (const auto it = o.find("aovs"); it != o.end() && it->second.IsArray())
                for (JsValue const &a : it->second.GetJsArray())
                    if (a.IsString())
                        f.shift.aovs.push_back(a.GetString());
        } else if (name == "uv_enlarger") {
            f.kind = HdNovaBakeFilter::Kind::Enlarge;
            if (const auto it = o.find("borderpixels"); it != o.end())
                f.border = std::max(0, int(number(it->second, 4.0f)));
            if (const auto it = o.find("alphaname"); it != o.end() && it->second.IsString())
                f.alpha = it->second.GetString();
            if (const auto it = o.find("scheme"); it != o.end() && it->second.IsString() &&
                                                  it->second.GetString() != "diffusefill")
                warnings.push_back("uv_enlarger scheme \"" + it->second.GetString() + "\" fills like diffusefill");
        } else if (name == "denoise" || name == "background") {
            continue; // the delegate's denoise and background filters
        } else {
            warnings.push_back("image filter \"" + name + "\" is not supported; it is skipped");
            continue;
        }
        (runLast ? last : first).push_back(f);
    }
    first.insert(first.end(), last.begin(), last.end());
    return first;
}

void HdNovaApplyBakeFilters(std::vector<HdNovaBakeFilter> const &filters, std::vector<std::string> const &names,
                            std::vector<HdNovaRenderBuffer *> const &copied, std::vector<float> const &coverage) {
    if (filters.empty())
        return;
    for (size_t i = 0; i < copied.size() && i < names.size(); ++i) {
        HdNovaRenderBuffer *rb = copied[i];
        if (!rb)
            continue;
        std::vector<float> values;
        int channels = 0;
        if (!rb->ReadFloats(values, channels) || channels <= 0 || values.size() != coverage.size() * size_t(channels))
            continue;
        const int width = int(rb->GetWidth()), height = int(rb->GetHeight());
        bool enlarged = false, changed = false;
        for (HdNovaBakeFilter const &f : filters) {
            if (f.kind == HdNovaBakeFilter::Kind::Enlarge) {
                if (names[i] == f.alpha)
                    continue;
                nova::enlargeIslands(values.data(), width, height, channels, coverage.data(), f.border);
                enlarged = changed = true;
            } else if (std::find(f.shift.aovs.begin(), f.shift.aovs.end(), names[i]) != f.shift.aovs.end()) {
                // After the enlarger the texels are no longer premultiplied
                // (measured: the offset applies to the filled texels too).
                nova::applyShift(values.data(), coverage.size(), channels, enlarged ? nullptr : coverage.data(),
                                 f.shift);
                changed = true;
            }
        }
        if (changed)
            rb->WriteFloats(values.data());
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
