#pragma once

// Pixel filters, applied by filter importance sampling: every camera ray is
// offset from its pixel center with the filter as density and carries weight
// 1, so a pixel is the plain average of its own samples (Karma's default
// "ubox" sample blending). In expectation this equals weighted
// reconstruction, while tiles stay independent (no splatting into
// neighbours) and crops stay bit-exact.
//
// The definitions match Karma CPU 22 (measured on slanted-edge renders): all
// filters are separable; box is 1 pixel wide, gauss has a standard deviation
// of 0.5 pixel (cut at 4 sigma), blackman a radius of 1.5 and blackmanharris
// a radius of 1.8 pixels, each times the filter scale. A separable warp keeps
// the stratification of the Sobol samples.

#include "core/math/vec.h"

#include <cstdint>

namespace nova {

enum class PixelFilterType : uint32_t { Point, Box, Gaussian, Blackman, BlackmanHarris, Triangle };

// Inverse error function for |x| < 1 (Giles 2011, single precision, absolute
// error about 5e-7).
NOVA_INLINE float erfInv(float x) {
#if defined(__CUDA_ARCH__)
    return erfinvf(x);
#else
    float w = -std::log((1.0f - x) * (1.0f + x)), p;
    if (w < 5.0f) {
        w = w - 2.5f;
        p = 2.81022636e-08f;
        p = 3.43273939e-07f + p * w;
        p = -3.5233877e-06f + p * w;
        p = -4.39150654e-06f + p * w;
        p = 0.00021858087f + p * w;
        p = -0.00125372503f + p * w;
        p = -0.00417768164f + p * w;
        p = 0.246640727f + p * w;
        p = 1.50140941f + p * w;
    } else {
        w = std::sqrt(w) - 3.0f;
        p = -0.000200214257f;
        p = 0.000100950558f + p * w;
        p = 0.00134934322f + p * w;
        p = -0.00367342844f + p * w;
        p = 0.00573950773f + p * w;
        p = -0.0076224613f + p * w;
        p = 0.00943887047f + p * w;
        p = 1.00167406f + p * w;
        p = 2.83297682f + p * w;
    }
    return p * x;
#endif
}

struct PixelFilter {
    static constexpr uint32_t kTableSize = 512;
    PixelFilterType type = PixelFilterType::Point;
    float radius = 0.0f;   // support half-width in pixels (gauss: 4 sigma)
    float sigma = 0.0f;    // gauss
    float gaussErf = 0.0f; // gauss: erf(radius / (sigma sqrt 2)), the mass inside the cut
    float invCdf[kTableSize + 1] = {}; // window filters: 1D inverse CDF at u = i / kTableSize

    // Offset from the pixel center in pixels, for u in [0, 1)^2.
    NOVA_FORCEINLINE Vec2f sample(Vec2f u) const { return {sample1D(u.x), sample1D(u.y)}; }

    NOVA_FORCEINLINE float sample1D(float u) const {
        switch (type) {
        case PixelFilterType::Point:
            return 0.0f;
        case PixelFilterType::Box:
            return (2.0f * u - 1.0f) * radius;
        case PixelFilterType::Gaussian:
            return 1.41421356f * sigma * erfInv((2.0f * u - 1.0f) * gaussErf);
        case PixelFilterType::Triangle:
            return u < 0.5f ? radius * (std::sqrt(2.0f * u) - 1.0f) : radius * (1.0f - std::sqrt(2.0f - 2.0f * u));
        default: {
            const float x = u * float(kTableSize);
            const uint32_t i = x < float(kTableSize - 1) ? uint32_t(x) : kTableSize - 1;
            const float t = x - float(i);
            // The windows vanish quadratically at the radius, so the CDF is
            // cubic there: cube-root interpolation in the two end cells.
            if (i == 0)
                return invCdf[0] + (invCdf[1] - invCdf[0]) * std::cbrt(t);
            if (i == kTableSize - 1)
                return invCdf[kTableSize] - (invCdf[kTableSize] - invCdf[kTableSize - 1]) * std::cbrt(1.0f - t);
            return invCdf[i] + t * (invCdf[i + 1] - invCdf[i]);
        }
        }
    }
};

// Unnormalized 1D profile (zero outside the support; 0 for the point filter).
float pixelFilterProfile(const PixelFilter &filter, float x);

// `scale` multiplies the base size; zero or less gives the point filter
// (every ray through the pixel center).
PixelFilter makePixelFilter(PixelFilterType type, float scale);

} // namespace nova
