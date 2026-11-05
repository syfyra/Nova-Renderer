#pragma once

// Random-walk subsurface scattering (docs/design/M9.md, research
// docs/research/m9/sss.md): Karma's model. Light enters with a cosine
// distribution about the inward geometric normal, walks through an
// index-matched medium of extinction 1 / mfp per channel with
// Henyey-Greenstein scattering, and leaves through a Lambertian exit wherever
// a segment crosses the object's surface. The single-scattering albedo comes
// from the subsurface colour by the exact inverse of van de Hulst's
// half-space albedo with OpenPBR's correction for the anisotropy, so a thick
// object's furnace albedo is the colour (Karma and the lab within 0.1-0.4 %).

#include "core/math/common.h"
#include "core/math/frame.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"
#include "core/math/spectrum.h"
#include "core/volume/phase.h"

#include <cmath>

namespace nova {

// The walk is killed after this many segments (Karma's cap, research 2.6:
// it darkens A >= 0.95 and g = 0.9 by 4-11 %, as Karma does).
constexpr int kMaxWalkSegments = 256;
// Below this mean free path (scene units) the medium is clamped (Karma).
constexpr float kMinMeanFreePath = 1e-5f;
// Albedos are kept below 1 so that every walk ends by Russian roulette.
constexpr float kMaxSubsurfaceAlbedo = 1.0f - 1e-7f;

// The single-scattering albedo whose half space has the multiple-scattering
// albedo A with cosine entry (OpenPBR 2025 eq. 46-50, Karma's
// parametrization): s = 4.09712 + 4.20863 A - sqrt(9.59217 + 41.6808 A +
// 17.7126 A^2), alpha* = 1 - s^2, alpha = alpha* / (1 - g (1 - alpha*)).
NOVA_INLINE float subsurfaceAlbedo(float A, float g) {
    A = saturate(A);
    const float s = 4.09712f + 4.20863f * A - std::sqrt(9.59217f + A * (41.6808f + 17.7126f * A));
    const float a = 1.0f - s * s;
    const float alpha = a / (1.0f - g * (1.0f - a));
    return nova::fmin(nova::fmax(alpha, 0.0f), kMaxSubsurfaceAlbedo);
}

// The medium of a subsurface lobe.
struct SubsurfaceLobe {
    Spectrum sigmaT; // extinction per channel, 1 / mean free path
    Spectrum alpha;  // single-scattering albedo per channel
    float g;         // HG anisotropy, inside (-0.99, 0.99) as for volumes

    // color: the target albedo (clamped to [0, 1]); mfp: the mean free path
    // per channel in scene units (radius x scale).
    NOVA_INLINE static SubsurfaceLobe make(Spectrum color, Spectrum mfp, float anisotropy) {
        SubsurfaceLobe m;
        m.g = nova::fmin(nova::fmax(anisotropy, -0.99f), 0.99f);
        for (int c = 0; c < 3; ++c) {
            m.sigmaT[c] = 1.0f / nova::fmax(mfp[c], kMinMeanFreePath);
            m.alpha[c] = subsurfaceAlbedo(color[c], m.g);
        }
        return m;
    }
    NOVA_INLINE bool chromatic() const { return sigmaT.x != sigmaT.y || sigmaT.x != sigmaT.z; }
};

// How a walk ended. `weight` is the escape weight per channel (the walk's
// own throughput; 0 when absorbed or killed). The last segment ran from org
// along dir and crossed the surface at t (prim, uv: the crossing).
struct WalkResult {
    Spectrum weight{0.0f, 0.0f, 0.0f};
    Vec3f org{0.0f, 0.0f, 0.0f};
    Vec3f dir{0.0f, 0.0f, 0.0f};
    float t = 0.0f;
    uint32_t prim = 0;
    Vec2f uv{0.0f, 0.0f};
    uint32_t segments = 0; // closest-hit queries traced
    bool exited = false;
    bool capped = false; // killed at the segment cap
};

// One walk from `org` (just inside the surface) into the medium.
// - u: the entry direction (xy, cosine about `inward`) and the first free
//   flight (z, also the first channel choice when the mean free path is
//   chromatic); the rest comes from `rng`.
// - trace(org, dir, tMax, t, prim, uv): the closest crossing of the object's
//   own surface within tMax (false: none; an open mesh has no boundary in
//   that direction and the walk goes on, research 0.8).
// Chromatic mean free paths: one walk whose distances follow one channel per
// segment, chosen in proportion to the throughput, weighted by the balance
// heuristic over the channels (Cycles' scheme, research 5.5). Russian
// roulette below a max-channel throughput of 0.1.
template <typename Trace>
NOVA_INLINE WalkResult randomWalk(const SubsurfaceLobe &m, Vec3f org, Vec3f inward, Vec3f u, Rng &rng,
                                  int maxSegments, const Trace &trace) {
    WalkResult r;
    Spectrum beta = spectrum(1.0f);
    Vec3f dir = Frame::fromNormal(inward).toWorld(sampleCosineHemisphere({u.x, u.y}));
    const bool chromatic = m.chromatic();
    for (int seg = 0; seg < maxSegments; ++seg) {
        float uc = seg == 0 ? u.z : rng.next();
        int ch = 0;
        float pk[3] = {1.0f, 0.0f, 0.0f};
        if (chromatic) {
            const float sum = beta.x + beta.y + beta.z;
            for (int k = 0; k < 3; ++k)
                pk[k] = beta[k] / sum;
            float lo = 0.0f;
            for (; ch < 2 && uc >= lo + pk[ch]; ++ch)
                lo += pk[ch];
            while (pk[ch] == 0.0f) { // rounding past the last channel with a share
                --ch;
                lo -= pk[ch];
            }
            uc = nova::fmin(nova::fmax((uc - lo) / pk[ch], 0.0f), 0.99999994f);
        }
        const float sc = m.sigmaT[ch];
        const float t = -std::log1p(-uc) / sc;
        float tb = 0.0f;
        uint32_t prim = 0;
        Vec2f uv{0.0f, 0.0f};
        ++r.segments;
        if (trace(org, dir, t, tb, prim, uv) && tb < t) {
            // Crosses the surface: the probability of flying past tb.
            if (chromatic) {
                Spectrum e;
                float q = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    e[k] = std::exp((sc - m.sigmaT[k]) * tb);
                    q += pk[k] * e[k];
                }
                beta = beta * e * (1.0f / q);
            }
            r.weight = beta;
            r.org = org;
            r.dir = dir;
            r.t = tb;
            r.prim = prim;
            r.uv = uv;
            r.exited = true;
            return r;
        }
        // Scatters at t.
        if (chromatic) {
            Spectrum f;
            float q = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const float e = std::exp((sc - m.sigmaT[k]) * t);
                f[k] = m.sigmaT[k] * e;
                q += pk[k] * f[k];
            }
            beta = beta * m.alpha * f * (1.0f / q);
        } else {
            beta = beta * m.alpha;
        }
        org = org + dir * t;
        const float cosTheta = sampleHgCos(m.g, rng.next());
        const float sinTheta = std::sqrt(nova::fmax(0.0f, 1.0f - cosTheta * cosTheta));
        const float phi = kTwoPi * rng.next();
        dir = normalize(Frame::fromNormal(dir).toWorld({sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta}));
        const float mx = maxComponent(beta);
        if (!(mx > 0.0f))
            return r;
        if (mx < 0.1f) {
            const float survive = mx * 10.0f;
            if (rng.next() >= survive)
                return r;
            beta = beta * (1.0f / survive);
        }
    }
    r.capped = true;
    return r;
}

} // namespace nova
