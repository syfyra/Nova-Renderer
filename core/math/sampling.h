#pragma once

// Warping functions from [0,1)^2 to directions. Local frames have +Z as the
// pole. Sobol/Owen-scrambled sequences replace the PCG stream in M2.

#include "core/math/vec.h"

namespace nova {

NOVA_INLINE Vec3f sampleCosineHemisphere(Vec2f u) {
    const float r = std::sqrt(u.x);
    const float phi = kTwoPi * u.y;
    return {r * std::cos(phi), r * std::sin(phi), std::sqrt(nova::fmax(0.0f, 1.0f - u.x))};
}
NOVA_INLINE float cosineHemispherePdf(float cosTheta) { return cosTheta > 0.0f ? cosTheta * kInvPi : 0.0f; }

NOVA_INLINE Vec3f sampleUniformSphere(Vec2f u) {
    const float z = 1.0f - 2.0f * u.x;
    const float r = std::sqrt(nova::fmax(0.0f, 1.0f - z * z));
    const float phi = kTwoPi * u.y;
    return {r * std::cos(phi), r * std::sin(phi), z};
}
NOVA_INLINE float uniformSpherePdf() { return 1.0f / (4.0f * kPi); }

// Uniform cone around +Z. The cone is given by 1 - cos(thetaMax), which stays
// accurate for tiny cones such as the sun (0.53 degrees).
NOVA_INLINE Vec3f sampleUniformCone(Vec2f u, float oneMinusCosThetaMax) {
    const float oneMinusCos = u.x * oneMinusCosThetaMax;
    const float cosTheta = 1.0f - oneMinusCos;
    const float sinTheta = std::sqrt(nova::fmax(0.0f, oneMinusCos * (2.0f - oneMinusCos)));
    const float phi = kTwoPi * u.y;
    return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
}
NOVA_INLINE float uniformConeSolidAngle(float oneMinusCosThetaMax) { return kTwoPi * oneMinusCosThetaMax; }

// 1 - cos(theta), computed without cancellation.
NOVA_INLINE float oneMinusCos(float theta) {
    const float s = std::sin(0.5f * theta);
    return 2.0f * s * s;
}

NOVA_INLINE float powerHeuristic(float a, float b) {
    const float a2 = a * a, b2 = b * b;
    return a2 + b2 > 0.0f ? a2 / (a2 + b2) : 0.0f;
}

} // namespace nova
