#include "products.h"

#include <pxr/base/vt/array.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/assetPath.h>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

bool asMap(VtValue const &v, HdAovSettingsMap &out) {
    out.clear();
    if (v.IsHolding<HdAovSettingsMap>()) {
        out = v.UncheckedGet<HdAovSettingsMap>();
        return true;
    }
    if (v.IsHolding<VtDictionary>()) {
        for (auto const &[key, value] : v.UncheckedGet<VtDictionary>())
            out[TfToken(key)] = value;
        return true;
    }
    return false;
}

std::vector<VtValue> asList(VtValue const &v) {
    std::vector<VtValue> out;
    const auto take = [&](auto const &array) {
        for (auto const &item : array)
            out.emplace_back(item);
    };
    if (v.IsHolding<VtArray<HdAovSettingsMap>>())
        take(v.UncheckedGet<VtArray<HdAovSettingsMap>>());
    else if (v.IsHolding<std::vector<HdAovSettingsMap>>())
        take(v.UncheckedGet<std::vector<HdAovSettingsMap>>());
    else if (v.IsHolding<VtArray<VtDictionary>>())
        take(v.UncheckedGet<VtArray<VtDictionary>>());
    else if (v.IsHolding<std::vector<VtDictionary>>())
        take(v.UncheckedGet<std::vector<VtDictionary>>());
    else if (v.IsHolding<std::vector<VtValue>>())
        out = v.UncheckedGet<std::vector<VtValue>>();
    else if (v.IsHolding<VtArray<VtValue>>())
        take(v.UncheckedGet<VtArray<VtValue>>());
    return out;
}

VtValue find(HdAovSettingsMap const &map, std::initializer_list<char const *> keys) {
    for (char const *k : keys) {
        const auto it = map.find(TfToken(k));
        if (it != map.end())
            return it->second;
    }
    return VtValue();
}

std::string text(VtValue const &v) {
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    if (v.IsHolding<SdfAssetPath>())
        return v.UncheckedGet<SdfAssetPath>().GetAssetPath();
    return {};
}

} // namespace

double HdNovaProductNumber(HdAovSettingsMap const &settings, char const *key, double fallback) {
    const auto it = settings.find(TfToken(key));
    if (it == settings.end())
        return fallback;
    if (it->second.IsHolding<bool>())
        return it->second.UncheckedGet<bool>() ? 1.0 : 0.0;
    const VtValue d = VtValue::Cast<double>(it->second);
    return d.IsHolding<double>() ? d.UncheckedGet<double>() : fallback;
}

std::string HdNovaProductString(HdAovSettingsMap const &settings, char const *key) {
    const auto it = settings.find(TfToken(key));
    return it == settings.end() ? std::string() : text(it->second);
}

std::vector<HdNovaDelegateProduct> HdNovaParseDelegateProducts(VtValue const &value) {
    std::vector<HdNovaDelegateProduct> products;
    for (VtValue const &item : asList(value)) {
        HdAovSettingsMap product;
        if (!asMap(item, product))
            continue;
        HdNovaDelegateProduct p;
        if (!asMap(find(product, {"settings"}), p.settings))
            p.settings = product;
        // Keys of the product itself win over its settings' (expanded names).
        for (auto const &[key, v] : product)
            if (key != TfToken("settings") && !v.IsEmpty() && !text(v).empty())
                p.settings[key] = v;
        p.type = HdNovaProductString(p.settings, "productType");
        p.name = HdNovaProductString(p.settings, "expandedProductName");
        if (p.name.empty())
            p.name = HdNovaProductString(p.settings, "productName");
        for (VtValue const &var : asList(find(product, {"orderedVars", "RenderVariables", "renderVars"}))) {
            HdAovSettingsMap v;
            if (!asMap(var, v))
                continue;
            // Measured: per var dataType, sourceName, sourceType and the AOV
            // descriptor (aovDescriptor.format, .clearValue, .multiSampled,
            // .aovSettings: the binding's settings with the driver parameters).
            HdNovaProductVar pv;
            if (!asMap(find(v, {"aovDescriptor.aovSettings", "settings", "aovSettings"}), pv.settings))
                pv.settings = v;
            for (char const *key : {"sourceName", "sourceType", "dataType"})
                if (pv.settings.find(TfToken(key)) == pv.settings.end() && !find(v, {key}).IsEmpty())
                    pv.settings[TfToken(key)] = find(v, {key});
            std::string name = text(find(v, {"AOVName", "aovName", "name"}));
            if (name.empty())
                name = HdNovaProductString(pv.settings, "driver:parameters:aov:husk:name");
            if (name.empty())
                name = HdNovaProductString(pv.settings, "sourceName");
            pv.aovName = TfToken(name);
            p.vars.push_back(std::move(pv));
        }
        products.push_back(std::move(p));
    }
    return products;
}

PXR_NAMESPACE_CLOSE_SCOPE
