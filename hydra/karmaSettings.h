#pragma once

// Karma's render properties as fallbacks of Nova's (docs/design/M11-P1.md).
// Karma's nodes author values in several forms: menus as their labels on the
// render settings ("Rounded Curves") and as ints on prims, switches as bools
// or ints, the pixel oracle as JSON. These helpers read any of them; the
// per-object defaults of the render settings travel to the prims in
// HdNovaObjectDefaults (HdNovaRenderParam).

#include <pxr/base/vt/value.h>
#include <pxr/pxr.h>

#include <initializer_list>
#include <optional>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

// A number: bool, the integer and floating-point types, a number as text.
std::optional<double> HdNovaNumber(VtValue const &value);
// A switch: bools, numbers (non-zero), "true"/"false"/"on"/"off"/"yes"/"no"
// or a number as text.
std::optional<bool> HdNovaFlag(VtValue const &value);
// A string or token (empty otherwise).
std::string HdNovaText(VtValue const &value);
// A string or token, or an array of them joined by spaces (a list such as
// karma:object:portaldomelights).
std::string HdNovaTextList(VtValue const &value);
// A menu entry: its index as a number, or one of `labels` (case and
// surrounding blanks ignored); -1 for neither.
int HdNovaMenuIndex(VtValue const &value, std::initializer_list<const char *> labels);

// karma:global:pixeloracle, JSON ["variance", {"variance": 0.01, "minrays":
// -1, ...}] or a plain name.
struct HdNovaKarmaOracle {
    bool valid = false;
    bool variance = false;
    float threshold = 0.01f;
    int minRays = -1;
};
HdNovaKarmaOracle HdNovaParseKarmaPixelOracle(std::string const &text);
// karma:global:pixelfilter: a name, or JSON ["name", {...}] (the name).
std::string HdNovaKarmaFilterName(std::string const &text);

// The render settings' values of per-object properties (Karma's
// karma:object:*), the defaults of prims that author none of their own (a
// prim's Nova primvar first, then its Karma primvar, then these). Negative:
// not set.
struct HdNovaObjectDefaults {
    int curveStyle = -1;        // karma:object:curve_style (menu index)
    int pointStyle = -1;        // karma:object:point_style
    float widthScale = -1.0f;   // karma:object:width_scale
    int lightSource = -1;       // karma:object:treat_as_lightsource (0 no, 1 yes, 2 auto)
    float lightDiffuse = -1.0f; // karma:object:lightsource_diffusescale
    float lightSpecular = -1.0f;
    float lightQuality = -1.0f;
    float dicingQuality = -1.0f; // karma:object:dicingquality
    int dicingDepth = -2;        // karma:object:dicingdepthmin = max (-1: off, -2: not set)
    int displacement = -1;       // karma:object:truedisplace: 0 Disable Displacement Shader, else 1
    int motionBlur = -1;         // karma:object:mblur
    int isPortal = -1;           // karma:object:isportal (docs/design/M11-P9b.md)
    std::string portalDomeLights; // karma:object:portaldomelights, if portalDomeLightsSet
    bool portalDomeLightsSet = false;

    bool operator==(const HdNovaObjectDefaults &) const = default;
};

PXR_NAMESPACE_CLOSE_SCOPE
