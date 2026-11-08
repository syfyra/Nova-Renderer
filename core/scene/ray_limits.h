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

// How a fake-caustic object passes light (docs/design/M11-P1.md): the
// factor per crossing of a straight line through the surface. Nova's is the
// physical transmittance (closureTransmittance); Karma's (karma = true,
// measured) adds the reflection lobe's loss on the side the ray comes from:
// (1 - F) times the transmission, Schlick's F, roughness ignored. Karma's
// artist controls apply to both: the factor from the BSDF or from `color`
// alone (bsdf), tinted by `color`, faded towards 1 by `opacity`, darkened
// towards grazing angles by `fresnelDarken` (0: off, 1: opaque).
struct FakeCausticsStyle {
    bool karma = false;
    bool bsdf = true;
    float color[3] = {1.0f, 1.0f, 1.0f};
    float opacity[3] = {1.0f, 1.0f, 1.0f};
    float fresnelDarken = 0.0f;

    bool operator==(const FakeCausticsStyle &) const = default;
};

// Render-wide limits (the defaults of every object). The core default is no
// limit (RenderSettings::maxDepth caps the path); the Hydra delegate's
// defaults are Karma's: diffuse 1, reflect 4, refract 4, sss 1, volume 0.
struct RayLimits {
    float limit[kLimitClasses] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
    bool thinWallAtRefractLimit = true;
    // Fake caustics (docs/design/M4.md 3.3; Scene::setObjectDefaults):
    // shadow rays, and rays after a diffuse event, cross the object's
    // transmissive surfaces straight with their transmittance.
    bool fakeCaustics = false;
    FakeCausticsStyle fakeStyle;
    // Reflection on the inside of refracting surfaces (Karma's
    // internalreflectenable; off: paths inside a solid dielectric leave it
    // without Fresnel loss, only total internal reflection reflects back in;
    // docs/design/M11-P1.md).
    bool internalReflection = true;
    // The least GGX roughness of true-caustic paths (Karma's
    // causticsroughnessclamp): at hits after a diffuse event on objects
    // without fake caustics.
    float causticsRoughnessClamp = 0.0f;

    bool operator==(const RayLimits &) const = default;
};

// Per-object overrides (primvars:nova:object:*, Karma's as fallbacks);
// negative: the render setting.
struct RayLimitOverrides {
    float limit[kLimitClasses] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    int8_t thinWallAtRefractLimit = -1; // -1: the render setting
    int8_t fakeCaustics = -1;           // -1: the render setting
    // Fake-caustic style (with fakeCaustics: Karma's factor when the switch
    // came from Karma's causticsenable) and Karma's controls.
    int8_t fakeKarma = -1;
    int8_t fakeBsdf = -1;
    float fakeColor[3] = {-1.0f, -1.0f, -1.0f};
    float fakeOpacity[3] = {-1.0f, -1.0f, -1.0f};
    float fakeDarken = -1.0f;
    int8_t internalReflection = -1;
    float causticsRoughnessClamp = -1.0f;

    bool any() const {
        for (float l : limit)
            if (l >= 0.0f)
                return true;
        return thinWallAtRefractLimit >= 0 || fakeCaustics >= 0 || fakeKarma >= 0 || fakeBsdf >= 0 ||
               fakeColor[0] >= 0.0f || fakeOpacity[0] >= 0.0f || fakeDarken >= 0.0f || internalReflection >= 0 ||
               causticsRoughnessClamp >= 0.0f;
    }
    bool operator==(const RayLimitOverrides &) const = default;
};

// What a mesh's overrides and the render-wide defaults give (Scene::commit):
// the shading-side object properties the integrators read per hit.
struct ObjectShading {
    bool fakeCaustics = false;
    bool internalReflection = true;
    float causticsRoughnessClamp = 0.0f;
    FakeCausticsStyle fake;

    bool operator==(const ObjectShading &) const = default;
};

NOVA_INLINE ObjectShading resolveObjectShading(const RayLimits &d, const RayLimitOverrides &o) {
    ObjectShading s;
    s.fakeCaustics = o.fakeCaustics >= 0 ? o.fakeCaustics != 0 : d.fakeCaustics;
    s.internalReflection = o.internalReflection >= 0 ? o.internalReflection != 0 : d.internalReflection;
    s.causticsRoughnessClamp = o.causticsRoughnessClamp >= 0.0f ? o.causticsRoughnessClamp : d.causticsRoughnessClamp;
    s.fake = d.fakeStyle;
    if (o.fakeKarma >= 0)
        s.fake.karma = o.fakeKarma != 0;
    if (o.fakeBsdf >= 0)
        s.fake.bsdf = o.fakeBsdf != 0;
    for (int k = 0; k < 3; ++k) {
        if (o.fakeColor[0] >= 0.0f)
            s.fake.color[k] = o.fakeColor[k];
        if (o.fakeOpacity[0] >= 0.0f)
            s.fake.opacity[k] = o.fakeOpacity[k];
    }
    if (o.fakeDarken >= 0.0f)
        s.fake.fresnelDarken = o.fakeDarken;
    return s;
}

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
