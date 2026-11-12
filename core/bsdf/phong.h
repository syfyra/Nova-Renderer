#pragma once

// Phong's specular lobe as Houdini's VEX phong() has it. Karma renders
// geometry without a material with Houdini's plastic, diff Cd diffuse() +
// spec phong(exponent) (measured, docs/bug-audit-2026-11.md section 17), and
// so does Nova's default material (SurfaceModel::Plastic).
//
// The light from a direction wi above the surface is weighted with
// (n + 1) / (2 pi) cos^n of the angle between wi and the mirror direction of
// wo: no cosine of incidence and no Fresnel term, so the value is "f |cos|"
// as the closure takes it. The lobe's albedo is 1 while it lies above the
// surface and falls where the mirror direction nears the horizon.

#include "core/bsdf/common.h"
#include "core/math/frame.h"

namespace nova {

struct PhongLobe {
    float exponent;

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        pdf = 0.0f;
        if (wo.z <= 0.0f || wi.z <= 0.0f)
            return {0.0f, 0.0f, 0.0f};
        const float c = wi.z * wo.z - wi.x * wo.x - wi.y * wo.y; // wi . mirror(wo)
        if (!(c > 0.0f))
            return {0.0f, 0.0f, 0.0f};
        pdf = (exponent + 1.0f) * kInvTwoPi * std::pow(nova::fmin(c, 1.0f), exponent);
        return {pdf, pdf, pdf};
    }

    // Directions around the mirror direction with a density of the lobe's
    // own shape; those below the surface carry nothing.
    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, LobeSample &s) const {
        if (wo.z <= 0.0f)
            return false;
        const float cosA = std::pow(nova::fmax(1.0f - u.x, 0.0f), 1.0f / (exponent + 1.0f));
        const float sinA = std::sqrt(nova::fmax(0.0f, 1.0f - cosA * cosA));
        const float phi = kTwoPi * u.y;
        const Frame mirror = Frame::fromNormal(Vec3f{-wo.x, -wo.y, wo.z});
        s.wi = normalize(mirror.toWorld(Vec3f{sinA * std::cos(phi), sinA * std::sin(phi), cosA}));
        s.delta = false;
        s.transmit = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }

    // The GGX alpha of about the same width (Walter et al.: n = 2 / alpha^2 - 2), for ray cones and
    // the path's roughness.
    NOVA_INLINE float alpha() const { return nova::fmin(std::sqrt(2.0f / (exponent + 2.0f)), 1.0f); }
};

} // namespace nova
