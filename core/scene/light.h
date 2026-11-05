#pragma once

#include "core/math/mat.h"
#include "core/math/spectrum.h"
#include "core/scene/image.h"

#include <cstdint>
#include <memory>
#include <string>

namespace nova {

using LightId = uint32_t;

// Volume: an emissive volume treated as a light source (docs/design/M8.md
// D13); Triangle: a triangle of an emissive mesh treated as one (M9 D14).
// The scene makes these, never a LightDesc.
enum class LightType : uint32_t { Distant, Dome, Sphere, Rect, Disk, Cylinder, Volume, Triangle };

// Orientation of a rect light's texture (Karma's recttextureflip). With the
// default FlipX the image's top-left corner sits at the light's (+X, +Y)
// corner, as UsdLux describes, and the image reads unmirrored from the
// emitting side.
enum class RectTextureFlip : uint32_t { None, FlipX, FlipY, FlipXY };

// Host-side light description, following UsdLux semantics. The integrator
// works on the derived LightData (core/lights/lights.h).
struct LightDesc {
    LightType type = LightType::Distant;
    Spectrum color{1.0f, 1.0f, 1.0f}; // includes the color temperature tint
    float intensity = 1.0f;
    float exposure = 0.0f;
    float diffuse = 1.0f;              // multiplier on diffuse reflection
    float specular = 1.0f;             // multiplier on specular (non-diffuse) reflection
    bool normalize = false;
    float angleDeg = 0.53f;            // distant light: angular diameter
    float radius = 0.5f;               // sphere, disk and cylinder lights
    float width = 1.0f;                // rect light, along local X
    float height = 1.0f;               // rect light, along local Y
    float length = 1.0f;               // cylinder light, along local X
    bool treatAsPoint = false;         // sphere light as a point light

    // UsdLuxShapingAPI, around the light's -Z axis. Only applied if `shaped`.
    // A cone angle of exactly 90 degrees (the schema fallback) means no cone.
    bool shaped = false;
    float coneAngleDeg = 90.0f;
    float coneSoftness = 0.0f;
    float focus = 0.0f;
    Spectrum focusTint{0.0f, 0.0f, 0.0f};

    // Karma light properties (Nova follows the Karma conventions):
    bool cameraVisible = false; // karma:light:renderlightgeo: camera rays see the light
    bool twoSided = false;      // karma:light:singlesided = 0: rect and disk emit from both sides
    RectTextureFlip rectFlip = RectTextureFlip::FlipX; // karma:light:recttextureflip
    std::string lpeTag;         // karma:light:lpetag: the light's tag in light path expressions
    float samplingQuality = 1.0f; // karma:light:samplingquality: multiplies the light's selection weight (M9 D18)
    bool equiangular = false;     // karma:light:equiangular: equiangular distance sampling in volumes (M9 D17)
    bool outsideTree = false;     // karma:light:samplingmode Uniform: chosen with the lights at infinity (M9 D18)
    // Light linking (M9 D15): the category of the objects the light lights,
    // and of those that shadow it (empty: every object; UsdLux collections
    // through HdsiLightLinkingSceneIndex).
    std::string lightLink, shadowLink;

    // Dome light: latlong environment (UsdLux/OpenEXR orientation: +Y is the
    // pole, the image center looks down +Z, u = 0.25 down +X). Rect light:
    // image on the emitting side. Multiplies the emission; null if untextured.
    std::shared_ptr<const Image> texture;

    Mat4 transform = Mat4::identity(); // light -> world
    bool visible = true;
};

} // namespace nova
