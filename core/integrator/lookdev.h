#pragma once

// Karma's look-dev overrides (docs/design/M11-P9a.md), measured with Karma
// CPU 22.0.466:
//
// override_lighting replaces the stage's lights: Emissive Objects keeps only
// emissive geometry; Headlight adds a distant light along the camera's view
// (irradiance 2 at normal incidence, measured 0.3183 on a 0.5 grey facing
// the camera); Dome Light a white dome of radiance 1 (measured 0.5 on 0.5
// grey; husk's dome headlight gives the same). Neither is seen by the
// camera. autoheadlight adds the same headlight when the stage has no light
// (Emissive geometry does not count). The scene applies these
// (Scene::setLightOverride).
//
// disable_lighting (the Karma Render Settings LOP's Simplified Shading)
// shades every camera hit with its display colour (primvar displayColor,
// white without one) times |N.V|, the depth cue fog mixed in by distance,
// all times an ambient occlusion factor, plus the material's emission;
// nothing else (no lights, no bounces). Measured: q = facing exactly
// (0.958, 0.930, 0.707, 0.366, 0.196 at the probe's facings), the fog
// f = clamp((t - near) / (far - near)) x alpha over the hit distance t,
// C = emission + AO (lerp(display x facing, fog colour, f)).
// head_ao_samples 0 turns the occlusion off; any other count only changes
// noise (Nova traces one cosine ray per camera sample); head_ao_distance
// limits the occluders' distance (Karma's default: unlimited).

#include "core/math/spectrum.h"

#include <cstdint>

namespace nova {

enum class LightOverride : uint8_t { Off, EmissiveOnly, Headlight, Dome };

struct LookDevSettings {
    // Scene lights (the scene applies them, Scene::setLightOverride).
    LightOverride lights = LightOverride::Off; // karma:global:override_lighting
    bool autoHeadlight = false;                // karma:global:autoheadlight
    // Simplified shading (karma:global:disable_lighting) and its controls.
    bool simplified = false;
    uint32_t aoSamples = 1;    // head_ao_samples (0: no occlusion)
    float aoDistance = 1e30f;  // head_ao_distance
    float fogNear = 0.0f;      // head_depthcue_z: near, far (far <= near: no fog)
    float fogFar = 0.0f;
    float fogColor[3] = {0.0f, 0.0f, 0.0f}; // head_depthcue_color
    float fogAlpha = 1.0f;     // head_depthcue_alpha

    bool operator==(const LookDevSettings &) const = default;
};

// The headlight's irradiance at normal incidence and the dome's radiance.
constexpr float kHeadlightIrradiance = 2.0f;
constexpr float kOverrideDomeRadiance = 1.0f;

// The depth cue fog's share at hit distance t.
NOVA_INLINE float depthCueFog(const LookDevSettings &s, float t) {
    if (!(s.fogFar > s.fogNear))
        return 0.0f;
    float f = (t - s.fogNear) / (s.fogFar - s.fogNear);
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    const float alpha = s.fogAlpha < 0.0f ? 0.0f : (s.fogAlpha > 1.0f ? 1.0f : s.fogAlpha);
    return f * alpha;
}

// The simplified shading at a camera hit before the occlusion factor:
// `display` the display colour, N the shading normal, wo towards the
// camera, t the hit distance.
NOVA_INLINE Spectrum simplifiedShade(const LookDevSettings &s, const Spectrum &display, Vec3f N, Vec3f wo, float t) {
    const float facing = std::fabs(dot(N, wo));
    const float f = depthCueFog(s, t);
    return display * (facing * (1.0f - f)) + Spectrum{s.fogColor[0], s.fogColor[1], s.fogColor[2]} * f;
}

} // namespace nova
