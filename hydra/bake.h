#pragma once

// The Karma Texture Baker in the delegate (docs/design/M11-P8.md): the
// karma:global:bake_* render settings the LOP authors become Nova's bake
// settings (core/bake/bake.h), and the image filters it lists in
// karma:global:imagefilter ("shift", "uv_enlarger") run on the bound buffers.

#include "core/bake/bake.h"
#include "core/bake/image_filter.h"

#include <pxr/base/vt/value.h>
#include <pxr/base/tf/token.h>
#include <pxr/pxr.h>

#include <functional>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaRenderParam;
class HdNovaRenderBuffer;
struct HdNovaAovSource;

using HdNovaSettingGetter = std::function<VtValue(TfToken const &)>;

// Whether the settings ask for a bake: karma:global:bake_low names a prim.
bool HdNovaBakeRequested(HdNovaSettingGetter const &get);

// The bake settings, the meshes resolved by path through the render param;
// what Nova does not support goes to `warnings`.
nova::BakeSettings HdNovaParseBakeSettings(HdNovaSettingGetter const &get, HdNovaRenderParam const &param,
                                           std::vector<std::string> &warnings);

// An image filter of a bake, in Karma's order: the filters without
// "runlast" first, then those with it, each group in list order.
struct HdNovaBakeFilter {
    enum class Kind { Shift, Enlarge } kind = Kind::Shift;
    nova::ShiftFilter shift;
    int border = 4;            // Enlarge
    std::string alpha = "Af";  // Enlarge: the coverage AOV, left as it is
};
std::vector<HdNovaBakeFilter> HdNovaParseBakeFilters(std::string const &json, std::vector<std::string> &warnings);

// Runs the filters on the bound buffers that are in `copied` (indexed like
// `names`, null: not written this pass); `coverage` is the film's alpha
// (the samples that found a shading point), per film pixel.
void HdNovaApplyBakeFilters(std::vector<HdNovaBakeFilter> const &filters, std::vector<std::string> const &names,
                            std::vector<HdNovaRenderBuffer *> const &copied, std::vector<float> const &coverage);

PXR_NAMESPACE_CLOSE_SCOPE
