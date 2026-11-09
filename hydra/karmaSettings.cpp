#include "karmaSettings.h"

#include "primvarUtil.h"
#include "renderParam.h"

#include <pxr/base/js/json.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

std::optional<double> parseNumber(std::string const &text) {
    const std::string t = TfStringTrim(text);
    if (t.empty())
        return std::nullopt;
    char *end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v))
        return std::nullopt;
    return v;
}

} // namespace

std::optional<double> HdNovaNumber(VtValue const &value) {
    if (value.IsHolding<bool>())
        return value.UncheckedGet<bool>() ? 1.0 : 0.0;
    if (value.IsHolding<TfToken>() || value.IsHolding<std::string>())
        return parseNumber(HdNovaText(value));
    if (value.IsArrayValued()) { // a one-element array (constant primvars)
        if (value.GetArraySize() != 1)
            return std::nullopt;
        double v = 0.0;
        if (value.IsHolding<VtArray<float>>())
            v = value.UncheckedGet<VtArray<float>>()[0];
        else if (value.IsHolding<VtArray<double>>())
            v = value.UncheckedGet<VtArray<double>>()[0];
        else if (value.IsHolding<VtArray<int>>())
            v = value.UncheckedGet<VtArray<int>>()[0];
        else if (value.IsHolding<VtArray<bool>>())
            v = value.UncheckedGet<VtArray<bool>>()[0] ? 1.0 : 0.0;
        else if (value.IsHolding<VtArray<std::string>>() || value.IsHolding<VtArray<TfToken>>())
            return parseNumber(HdNovaText(value));
        else
            return std::nullopt;
        return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
    }
    const VtValue d = VtValue::Cast<double>(value);
    if (d.IsHolding<double>() && std::isfinite(d.UncheckedGet<double>()))
        return d.UncheckedGet<double>();
    return std::nullopt;
}

std::optional<bool> HdNovaFlag(VtValue const &value) {
    if (value.IsHolding<TfToken>() || value.IsHolding<std::string>()) {
        const std::string t = TfStringToLower(TfStringTrim(HdNovaText(value)));
        if (t == "true" || t == "on" || t == "yes")
            return true;
        if (t == "false" || t == "off" || t == "no")
            return false;
    }
    const std::optional<double> n = HdNovaNumber(value);
    if (!n)
        return std::nullopt;
    return *n != 0.0;
}

std::string HdNovaText(VtValue const &value) {
    if (value.IsHolding<std::string>())
        return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>())
        return value.UncheckedGet<TfToken>().GetString();
    if (value.IsHolding<VtArray<std::string>>() && value.UncheckedGet<VtArray<std::string>>().size() == 1)
        return value.UncheckedGet<VtArray<std::string>>()[0];
    if (value.IsHolding<VtArray<TfToken>>() && value.UncheckedGet<VtArray<TfToken>>().size() == 1)
        return value.UncheckedGet<VtArray<TfToken>>()[0].GetString();
    return {};
}

std::string HdNovaTextList(VtValue const &value) {
    std::string text;
    const auto add = [&](std::string const &s) {
        if (!text.empty())
            text += ' ';
        text += s;
    };
    if (value.IsHolding<VtArray<std::string>>()) {
        for (std::string const &s : value.UncheckedGet<VtArray<std::string>>())
            add(s);
        return text;
    }
    if (value.IsHolding<VtArray<TfToken>>()) {
        for (TfToken const &t : value.UncheckedGet<VtArray<TfToken>>())
            add(t.GetString());
        return text;
    }
    return HdNovaText(value);
}

int HdNovaMenuIndex(VtValue const &value, std::initializer_list<const char *> labels) {
    if (value.IsEmpty())
        return -1;
    if (value.IsHolding<TfToken>() || value.IsHolding<std::string>() || value.IsHolding<VtArray<std::string>>() ||
        value.IsHolding<VtArray<TfToken>>()) {
        const std::string text = TfStringToLower(TfStringTrim(HdNovaText(value)));
        int i = 0;
        for (const char *label : labels) {
            if (text == TfStringToLower(label))
                return i;
            ++i;
        }
    }
    const std::optional<double> n = HdNovaNumber(value);
    if (!n || *n < 0.0 || *n >= double(labels.size()))
        return -1;
    return int(std::lround(*n));
}

HdNovaKarmaOracle HdNovaParseKarmaPixelOracle(std::string const &text) {
    HdNovaKarmaOracle o;
    const std::string t = TfStringTrim(text);
    if (t.empty())
        return o;
    std::string name;
    if (t.front() == '[' || t.front() == '"') {
        JsParseError error;
        const JsValue v = JsParseString(t, &error);
        if (v.IsArray() && !v.GetJsArray().empty()) {
            const JsArray &a = v.GetJsArray();
            if (a[0].IsString())
                name = a[0].GetString();
            if (a.size() > 1 && a[1].IsObject()) {
                const JsObject &args = a[1].GetJsObject();
                const auto number = [&](char const *key) -> std::optional<double> {
                    const auto it = args.find(key);
                    if (it == args.end())
                        return std::nullopt;
                    if (it->second.IsReal())
                        return it->second.GetReal();
                    if (it->second.IsInt())
                        return double(it->second.GetInt64());
                    return std::nullopt;
                };
                if (const auto v = number("variance"); v && std::isfinite(*v) && *v >= 0.0)
                    o.threshold = float(*v);
                if (const auto v = number("minrays"); v && std::isfinite(*v))
                    o.minRays = int(*v);
            }
        } else if (v.IsString()) {
            name = v.GetString();
        }
    } else {
        name = t;
    }
    name = TfStringToLower(TfStringTrim(name));
    if (name == "variance" || name == "uniform") {
        o.valid = true;
        o.variance = name == "variance";
    }
    return o;
}

HdNovaShutter HdNovaObjectShutter(HdSceneDelegate *sd, SdfPath const &id, HdNovaRenderParam const &param) {
    static const TfToken mblur("karma:object:mblur");
    std::optional<bool> on = HdNovaFlag(HdNovaConstantPrimvar(sd, id, mblur));
    if (!on) {
        const int d = param.GetObjectDefaults().motionBlur;
        if (d >= 0)
            on = d != 0;
    }
    return on.value_or(true) ? param.GetShutter() : HdNovaShutter{};
}

std::string HdNovaKarmaFilterName(std::string const &text) {
    const std::string t = TfStringTrim(text);
    if (!t.empty() && t.front() == '[') {
        JsParseError error;
        const JsValue v = JsParseString(t, &error);
        if (v.IsArray() && !v.GetJsArray().empty() && v.GetJsArray()[0].IsString())
            return TfStringToLower(v.GetJsArray()[0].GetString());
        return {};
    }
    return TfStringToLower(t);
}

PXR_NAMESPACE_CLOSE_SCOPE
