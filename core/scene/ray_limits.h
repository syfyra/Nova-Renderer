#pragma once

// Per-ray-type path limits (docs/design/M4.md 3.2; Karma's semantics,
// measured in docs/research/m4/nodes-and-limits.md 1): every scattering event
// has a class with its own counter; an event within its class's limit spawns
// a full ray, the event past it a light-only ray (it gathers the lights it
// reaches and ends the path, its hit is never shaded). The limit is read from
// the object where the event happens; a fractional limit L is floor(L) + 1
// for a share frac(L) of the paths. With thin walls at the refract limit,
// refraction events up to twice the limit continue straight through.

#include "core/math/common.h"

#include <cstdint>

namespace nova {

enum class LimitClass : uint8_t { Diffuse, Reflect, Refract, Sss, Volume };
constexpr int kLimitClasses = 5;

// Render-wide limits (the defaults of every object). The core default is no
// limit (RenderSettings::maxDepth caps the path); the Hydra delegate's
// defaults are Karma's: diffuse 1, reflect 4, refract 4, sss 1, volume 0.
struct RayLimits {
    float limit[kLimitClasses] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
    bool thinWallAtRefractLimit = true;
    // Fake caustics (docs/design/M4.md 3.3; Scene::setDefaultFakeCaustics):
    // shadow rays, and rays after a diffuse event, cross the object's
    // transmissive surfaces straight with their transmittance.
    bool fakeCaustics = false;

    bool operator==(const RayLimits &) const = default;
};

// Per-object overrides (primvars:nova:object:*); negative: the render setting.
struct RayLimitOverrides {
    float limit[kLimitClasses] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    int8_t thinWallAtRefractLimit = -1; // -1: the render setting
    int8_t fakeCaustics = -1;           // -1: the render setting

    bool any() const {
        for (float l : limit)
            if (l >= 0.0f)
                return true;
        return thinWallAtRefractLimit >= 0 || fakeCaustics >= 0;
    }
    bool operator==(const RayLimitOverrides &) const = default;
};

// The integer limit of a path whose dither sample is u in [0, 1).
NOVA_INLINE uint32_t pathLimit(float limit, float u) {
    if (!(limit >= 0.0f))
        return 0;
    if (limit >= 255.0f)
        return 255;
    const uint32_t whole = uint32_t(limit);
    return whole + (u < limit - float(whole) ? 1u : 0u);
}

} // namespace nova
