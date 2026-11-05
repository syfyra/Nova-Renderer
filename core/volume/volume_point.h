#pragma once

// A volume's coefficients at a point (docs/design/M8.md D4-D6, D10): what
// the volume evaluators of the CPU (core/integrator/volume_eval.h) and the
// GPU give the trackers.

#include "core/math/spectrum.h"
#include "core/volume/shader.h"

#include <cmath>

namespace nova {

struct VolumePoint {
    Spectrum scattering{0.0f, 0.0f, 0.0f};
    Spectrum absorption{0.0f, 0.0f, 0.0f};
    Spectrum emission{0.0f, 0.0f, 0.0f};
    float g = 0.0f;   // Henyey-Greenstein
    float g2 = 0.0f;  // the secondary lobe's
    float mix = 0.0f; // the secondary lobe's share

    NOVA_INLINE Spectrum extinction() const { return scattering + absorption; }
};

// A volume shader's outputs as a point (D10): scattering and absorption
// clamped to 0 from below, anisotropies to (-0.99, 0.99). Karma's
// multiple-scattering modifiers (measured, research H 4.7): on rays with
// `depth` events before them, from the start level on, the extinction
// scales by exp(-modifier * (depth - start + 1)) and the albedo by the
// contribution modifier (scattering becomes absorption). Both only lower
// the extinction, so the majorants hold.
NOVA_INLINE VolumePoint volumePointOf(const VolumeInputs &in, uint32_t depth) {
    const auto nonNegative = [](Spectrum s) {
        return Spectrum{s.x < 0.0f ? 0.0f : s.x, s.y < 0.0f ? 0.0f : s.y, s.z < 0.0f ? 0.0f : s.z}; // as std::max
    };
    const auto clampTo = [](float x, float lo, float hi) { return x < lo ? lo : (hi < x ? hi : x); };
    VolumePoint out;
    out.scattering = nonNegative(in.c(volume_field::scattering));
    out.absorption = nonNegative(in.c(volume_field::absorption));
    out.emission = in.c(volume_field::emission);
    const int start = int(std::lround(in.f(volume_field::modifierStartLevel)));
    if (int(depth) >= start) {
        const float m = in.f(volume_field::extinctionModifier), e = m < 0.0f ? 0.0f : m;
        if (e > 0.0f) {
            const float scale = std::exp(-e * float(int(depth) - start + 1));
            out.scattering *= scale;
            out.absorption *= scale;
        }
        const float c = clampTo(in.f(volume_field::contributionModifier), 0.0f, 1.0f);
        if (c < 1.0f) {
            out.absorption += out.scattering * (1.0f - c);
            out.scattering *= c;
        }
    }
    out.g = clampTo(in.f(volume_field::anisotropy), -0.99f, 0.99f);
    out.g2 = clampTo(in.f(volume_field::secondaryAnisotropy), -0.99f, 0.99f);
    out.mix = clampTo(in.f(volume_field::secondaryMix), 0.0f, 1.0f);
    return out;
}

} // namespace nova
