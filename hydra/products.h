#pragma once

// Non-raster render products (docs/design/M11-P6.md 5, 6). husk writes the
// raster products itself and hands the others to the delegate in the
// render setting "delegateRenderProducts" (measured: a VtArray of
// HdAovSettingsMap, one per product, with productName, productType,
// sourcePrim, the product's own attributes such as karma:product:dcm* or
// the checkpoint's resume and saveFrequency, and orderedVars: per var a map
// of its settings as in an AOV binding). Nova writes Karma's
// karma:deep products (deep camera maps) and karma:checkpoint products
// (its own checkpoint files, from which a render resumes).

#include <pxr/imaging/hd/aov.h>
#include <pxr/pxr.h>

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

struct HdNovaProductVar {
    TfToken aovName;
    HdAovSettingsMap settings;
};

struct HdNovaDelegateProduct {
    std::string type; // productType, e.g. karma:deep
    std::string name; // productName (the file)
    HdAovSettingsMap settings;
    std::vector<HdNovaProductVar> vars;
};

// The products in husk's delegateRenderProducts value (empty if it holds
// none Nova can read).
std::vector<HdNovaDelegateProduct> HdNovaParseDelegateProducts(VtValue const &value);

// A product setting's value as a double / bool / string (fallback if unset
// or of another type).
double HdNovaProductNumber(HdAovSettingsMap const &settings, char const *key, double fallback);
std::string HdNovaProductString(HdAovSettingsMap const &settings, char const *key);

PXR_NAMESPACE_CLOSE_SCOPE
