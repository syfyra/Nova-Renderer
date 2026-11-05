#pragma once

// Spectral abstraction. Nova currently renders in linear RGB (the rendering
// color space); all radiometric quantities use Spectrum so that a sampled
// spectral representation can replace it later without touching call sites.

#include "core/math/vec.h"

namespace nova {

using Spectrum = Vec3f;

NOVA_INLINE Spectrum spectrum(float v) { return {v, v, v}; }

// Rec.709 / linear sRGB luminance.
NOVA_INLINE float luminance(Spectrum s) { return 0.2126f * s.x + 0.7152f * s.y + 0.0722f * s.z; }

} // namespace nova
