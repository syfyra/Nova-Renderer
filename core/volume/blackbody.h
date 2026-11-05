#pragma once

// Karma's blackbody (docs/design/M8.md D14): MaterialX's ND_blackbody
// chromaticity with a brightness that grows roughly like T^4 (research H
// 4.4), measured from Karma CPU (tests/karma/probe_blackbody.py,
// core/volume/blackbody_table.h). Between the table's temperatures (log-
// uniform) the luminance is interpolated log-log and the normalized colour
// linearly in log T; outside the table the end colour, its luminance scaled
// by (T / T_end)^4. Channels may be negative below about 1900 K (out of the
// Rec.709 gamut), as Karma's are; T <= 0 is black.

#include "core/math/spectrum.h"
#include "core/volume/blackbody_table.h"

#include <cmath>

namespace nova {

namespace blackbody_detail {
#if defined(__CUDACC__)
__device__ __constant__ float kTableDevice[NOVA_BLACKBODY_TABLE_COUNT][4] = NOVA_BLACKBODY_TABLE_DATA;
#endif
inline constexpr float kTableHost[NOVA_BLACKBODY_TABLE_COUNT][4] = NOVA_BLACKBODY_TABLE_DATA;

NOVA_INLINE const float *entry(int i) {
#if defined(__CUDA_ARCH__)
    return kTableDevice[i];
#else
    return kTableHost[i];
#endif
}
} // namespace blackbody_detail

constexpr int kBlackbodyEntries = NOVA_BLACKBODY_TABLE_COUNT;
constexpr float kBlackbodyMinKelvin = NOVA_BLACKBODY_TABLE_MIN_KELVIN;
constexpr float kBlackbodyMaxKelvin = NOVA_BLACKBODY_TABLE_MAX_KELVIN;

// The table's temperature of entry i.
NOVA_INLINE float blackbodyKelvin(int i) {
    return kBlackbodyMinKelvin *
           std::pow(kBlackbodyMaxKelvin / kBlackbodyMinKelvin, float(i) / float(kBlackbodyEntries - 1));
}

// Karma's ND_blackbody at `kelvin`, linear Rec.709.
NOVA_INLINE Spectrum blackbody(float kelvin) {
    using namespace blackbody_detail;
    if (!(kelvin > 0.0f))
        return {0.0f, 0.0f, 0.0f};
    const float span = std::log(kBlackbodyMaxKelvin / kBlackbodyMinKelvin);
    float x = std::log(kelvin / kBlackbodyMinKelvin) / span * float(kBlackbodyEntries - 1);
    float scale = 1.0f;
    if (!(x >= 0.0f)) {
        const float r = kelvin / kBlackbodyMinKelvin;
        scale = (r * r) * (r * r);
        x = 0.0f;
    } else if (x > float(kBlackbodyEntries - 1)) {
        const float r = kelvin / kBlackbodyMaxKelvin; // infinite kelvin: infinite light
        scale = (r * r) * (r * r);
        x = float(kBlackbodyEntries - 1);
    }
    int i = int(x);
    if (i > kBlackbodyEntries - 2)
        i = kBlackbodyEntries - 2;
    const float f = x - float(i);
    const float *a = entry(i), *b = entry(i + 1);
    const float y = a[0] * std::pow(b[0] / a[0], f) * scale;
    return Spectrum{a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f, a[3] + (b[3] - a[3]) * f} * y;
}

} // namespace nova
