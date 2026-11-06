#pragma once

// Per-object ray visibility and holdouts (docs/design/M11-P2.md): Karma's
// karma:object:rendervisibility and karma:object:holdoutmode.
//
// Visibility is a mask over the ray classes (RayClass, core/bsdf/closure.h):
// a hit on an object counts only for rays whose class bit is set; occlusion
// queries are shadow rays. Objects with a partial mask go through the hit
// filter (Embree argument filters, OptiX any-hit), so the default costs
// nothing.

#include "core/math/common.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace nova {

// Bit RayClass c of a visibility mask (Camera 0, Diffuse 1, Reflect 2,
// Refract 3, Sss 4, Volume 5, Shadow 6; core/scene/visibility.cpp checks).
constexpr uint8_t kVisibleCamera = 1u << 0;
constexpr uint8_t kVisibleDiffuse = 1u << 1;
constexpr uint8_t kVisibleReflect = 1u << 2;
constexpr uint8_t kVisibleRefract = 1u << 3;
constexpr uint8_t kVisibleSss = 1u << 4;
constexpr uint8_t kVisibleVolume = 1u << 5;
constexpr uint8_t kVisibleShadow = 1u << 6;
constexpr uint8_t kVisibleAll = 0x7F;

NOVA_INLINE bool visibleTo(uint8_t mask, uint8_t rayClass) { return ((mask >> rayClass) & 1u) != 0; }

// A ray's class byte (RayBatch::rayType) holds the RayClass in its low bits
// and, on shadow rays, flags for the hit filter: the ray leaves a
// background-holdout point, so other background holdouts do not shadow it
// (Karma's default), or it probes for background holdouts alone (Karma's
// bgoccludeshadow: a point they occlude keeps its light).
constexpr uint8_t kRayClassBits = 0x0F;
constexpr uint8_t kRayFromBackground = 0x80;
constexpr uint8_t kRayProbeBackground = 0x40;

// Karma's holdout modes (primvars:karma:object:holdoutmode).
enum class HoldoutMode : uint8_t {
    None,       // an ordinary object
    Matte,      // a cut-out: colour and alpha 0 for the camera, its light to holdout outputs
    Background, // a background plate stand-in: alpha = the shadow CG objects cast on it
};

// An object's own settings; negative: the render settings' (the defaults of
// every object, Scene::setDefaultVisibility).
struct ObjectVisibility {
    int16_t mask = -1;   // visibility mask (kVisible*), -1: unset
    int8_t holdout = -1; // HoldoutMode, -1: unset

    bool operator==(const ObjectVisibility &) const = default;
};

// Parses a rendervisibility expression into a mask. Ray types: primary,
// shadow, diffuse, reflect, refract, volume, sss and * (all); a leading -
// (or ! or ^) takes the complement; & intersects, | unions (& binds
// tighter); parentheses group; whitespace-separated terms combine as a
// Houdini pattern does (a plain term adds, a negated one removes; a list
// that starts with a negated term starts from all). An empty expression is
// visible to no ray. False with `error` on a syntax error or an unknown
// ray type.
bool parseRenderVisibility(std::string_view text, uint8_t &mask, std::string *error = nullptr);

// HoldoutMode from Karma's values: 0/1/2 or the menu labels None, Matte,
// Background (case-insensitive). False if neither.
bool parseHoldoutMode(std::string_view text, HoldoutMode &mode);

} // namespace nova
