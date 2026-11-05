#pragma once

// Volume shading (docs/design/M8.md D4): what a volume material computes at
// a point. A material's volume program (Program::volume) writes these
// fields; in the material's IR graph they are outputs at volume_field::kBase
// + offset, apart from the surface's, and the compiler splits them off as it
// does the displacement. Semantics as Karma's (research H 4.2): scattering
// and absorption are coefficients per unit length (extinction = their sum),
// emission is radiance per unit length (not scaled by density), anisotropy
// is Henyey-Greenstein's g (> 0 forward); kma_volume's secondary lobe mixes
// a second g in, its extinction/contribution modifiers act from the start
// level on (M8.3).

#include "core/math/spectrum.h"

#include <cstdint>

namespace nova {

namespace volume_field {
constexpr uint16_t scattering = 0;            // 3
constexpr uint16_t absorption = 3;            // 3
constexpr uint16_t emission = 6;              // 3
constexpr uint16_t anisotropy = 9;            // 1
constexpr uint16_t secondaryAnisotropy = 10;  // 1
constexpr uint16_t secondaryMix = 11;         // 1
constexpr uint16_t extinctionModifier = 12;   // 1
constexpr uint16_t contributionModifier = 13; // 1
constexpr uint16_t modifierStartLevel = 14;   // 1
constexpr uint16_t kCount = 15;
// The outputs' field numbers in a material's IR graph.
constexpr uint16_t kBase = 0xFE00;
NOVA_INLINE constexpr bool isVolumeOutput(uint16_t field) { return field >= kBase && field < kBase + kCount; }
} // namespace volume_field

struct VolumeInputs {
    float v[volume_field::kCount];

    NOVA_INLINE float f(uint16_t field) const { return v[field]; }
    NOVA_INLINE Spectrum c(uint16_t field) const { return {v[field], v[field + 1], v[field + 2]}; }
    NOVA_INLINE void set(uint16_t field, float x) { v[field] = x; }
    NOVA_INLINE void set(uint16_t field, Spectrum x) {
        v[field] = x.x;
        v[field + 1] = x.y;
        v[field + 2] = x.z;
    }
};

// kma_volume's defaults (Karma's node registry): no scattering, absorption
// or emission; isotropic; no secondary lobe; modifiers off from level 1.
NOVA_INLINE VolumeInputs volumeDefaults() {
    VolumeInputs in{};
    in.set(volume_field::contributionModifier, 1.0f);
    in.set(volume_field::modifierStartLevel, 1.0f);
    return in;
}

} // namespace nova
