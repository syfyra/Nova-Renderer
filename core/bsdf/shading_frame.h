#pragma once

// Shading-normal robustness (docs/design/M3.md section 5):
//   ensureValidSpecularReflection  bends a shading normal towards the
//       geometric one just enough that the mirror reflection of wo stays a
//       little above the surface (Cycles' ensure_valid_specular_reflection;
//       Karma's _mx_normal_clamp does the same with bias 0.01). Interpolated
//       and bump/normal-mapped normals otherwise send specular rays into the
//       surface, which renders black patches.
//   bumpShadowingTerm  softens diffuse lobes whose normal was bent by a bump
//       or normal map at the light's terminator and cuts light leaking to
//       the back (Cycles' bump_shadowing_term).
// All vectors in world space, unit length; wo points away from the surface
// and lies above the geometric normal ng.

#include "core/math/vec.h"

namespace nova {

NOVA_INLINE Vec3f ensureValidSpecularReflection(Vec3f ng, Vec3f wo, Vec3f n) {
    const Vec3f r = n * (2.0f * dot(n, wo)) - wo;
    const float woz = dot(wo, ng);
    // Reflections may always be at least as shallow as wo itself.
    const float threshold = nova::fmin(0.9f * woz, 0.01f);
    if (dot(ng, r) >= threshold)
        return n;
    // In the plane of ng (z) and n: the normal n' whose reflection of wo
    // lies exactly `threshold` above the surface (the one valid root of
    // 4 a nz^4 - 2 b nz^2 + c = 0).
    const Vec3f t = n - ng * dot(n, ng);
    const float tl = dot(t, t);
    const Vec3f X = tl > 0.0f ? t * (1.0f / std::sqrt(tl)) : n;
    const float wox = dot(wo, X);
    const float a = wox * wox + woz * woz, b = 2.0f * (a + woz * threshold), c = sqr(threshold + woz);
    const float root = safeSqrt(b * b - 4.0f * a * c);
    const float nz2 = (wox < 0.0f ? 0.25f * (b + root) : 0.25f * (b - root)) / a;
    return X * safeSqrt(1.0f - nz2) + ng * safeSqrt(nz2);
}

// ns: the smooth (interpolated, unbumped) normal, n: the lobe's normal after
// bump or normal mapping, wi: the light direction. 1 when n == ns.
NOVA_INLINE float bumpShadowingTerm(Vec3f ns, Vec3f n, Vec3f wi) {
    const float cosNsI = dot(ns, wi), cosNsN = dot(ns, n), cosNI = dot(n, wi);
    if (cosNsI * cosNsN * cosNI < 0.0f)
        return 0.0f; // light from behind the smooth surface
    const float cosI = std::fabs(cosNsI), cosD = std::fabs(cosNsN);
    if (cosD >= 1.0f || cosI >= 1.0f)
        return 1.0f;
    if (cosI < 1e-6f)
        return 0.0f;
    // GGX masking with alpha^2 = 0.125 tan^2(deviation), smooth at 90 degrees.
    const float alpha2 = saturate(0.125f * (1.0f / (cosD * cosD) - 1.0f));
    const float tan2I = (1.0f - cosI * cosI) / (cosI * cosI);
    return 2.0f / (1.0f + std::sqrt(1.0f + alpha2 * tan2I));
}

} // namespace nova
