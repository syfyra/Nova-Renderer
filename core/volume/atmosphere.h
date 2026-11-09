#pragma once

// Karma's planetary sky atmosphere (docs/design/M11-P7.md): a shell of air
// around a spherical planet, from sea level up to a top, with an exponential
// Rayleigh layer (pure scattering), an exponential Mie layer (scattering and
// absorption, Henyey-Greenstein phase) and an absorbing (ozone) layer whose
// density is a tent around an altitude; below it an opaque ground sphere
// lit by the distant lights. Shared by the CPU and the GPU.
//
// The density is analytic, so the transmittance along any segment is
// computed, not estimated: Gauss-Legendre quadrature (4 points) between the
// segment's crossings of a few altitude levels, inside which the density
// changes by at most e^2 (relative error of the optical depth below 1e-5).
// Rays that may scatter sample one in-scattering distance per segment from a
// piecewise-exponential proposal of sigma_s(t) T(t) over the same intervals
// (AtmosphereSegment): the weights carry the exact sigma_s T over the
// proposal's density, so the estimate is unbiased up to the quadrature.
//
// Geometry is evaluated in double where it subtracts planet-sized lengths
// (the per-ray base values and the level crossings); everything per point is
// float.

#include "core/math/common.h"
#include "core/math/spectrum.h"
#include "core/math/vec.h"

#include <cmath>
#include <cstdint>

namespace nova {

// What the trackers read (Scene::atmosphere(), uploaded unchanged to the GPU
// with `levels`). Lengths in world units, coefficients per world unit.
struct AtmosphereView {
    double center[3] = {0.0, 0.0, 0.0};
    double seaLevel = 0.0;     // the medium's inner radius
    double groundRadius = 0.0; // the ground sphere (sea level minus the ground offset)
    float topAltitude = 0.0f;  // the medium's outer radius minus the sea level
    Spectrum rayleigh{0.0f, 0.0f, 0.0f};      // scattering (= extinction) at sea level
    Spectrum mieScatter{0.0f, 0.0f, 0.0f};    // Mie scattering at sea level
    Spectrum mieExtinction{0.0f, 0.0f, 0.0f}; // Mie scattering + absorption at sea level
    Spectrum ozone{0.0f, 0.0f, 0.0f};         // absorption at the ozone peak
    float rayleighInvHeight = 0.0f, mieInvHeight = 0.0f;
    float ozoneAltitude = 0.0f, ozoneHalfWidth = 0.0f;
    float mieG = 0.0f;
    Spectrum groundAlbedo{0.0f, 0.0f, 0.0f};
    uint8_t groundVisibility = 0; // kVisible* mask of the ray classes that see the ground
    uint8_t groundShadows = 0;    // the scene's objects shadow the ground
    uint8_t backside = 0;         // rays that do not see the ground continue through the planet
    uint8_t ignoreClip = 1;       // camera rays that miss see the atmosphere past the far clipping plane
    // Altitudes (ascending, between 0 and the top) at which the quadrature
    // splits rays, and the VolumeId of the atmosphere's scene record.
    const float *levels = nullptr;
    uint32_t levelCount = 0;
    uint32_t volume = 0xFFFFFFFFu;
};

// The coefficients at altitude h above sea level (none outside the shell).
struct AtmosphereCoefficients {
    Spectrum rayleigh{0.0f, 0.0f, 0.0f}; // Rayleigh scattering
    Spectrum mie{0.0f, 0.0f, 0.0f};      // Mie scattering
    Spectrum extinction{0.0f, 0.0f, 0.0f};
};

NOVA_INLINE float atmosphereOzone(const AtmosphereView &a, float h) {
    if (!(a.ozoneHalfWidth > 0.0f))
        return 0.0f;
    const float o = 1.0f - std::fabs(h - a.ozoneAltitude) / a.ozoneHalfWidth;
    return o > 0.0f ? o : 0.0f;
}

NOVA_INLINE AtmosphereCoefficients atmosphereAt(const AtmosphereView &a, float h) {
    AtmosphereCoefficients c;
    if (!(h >= 0.0f) || h > a.topAltitude)
        return c;
    const float r = std::exp(-h * a.rayleighInvHeight), m = std::exp(-h * a.mieInvHeight);
    c.rayleigh = a.rayleigh * r;
    c.mie = a.mieScatter * m;
    c.extinction = c.rayleigh + a.mieExtinction * m + a.ozone * atmosphereOzone(a, h);
    return c;
}

NOVA_INLINE Spectrum atmosphereExtinction(const AtmosphereView &a, float h) {
    if (!(h >= 0.0f) || h > a.topAltitude)
        return Spectrum{0.0f, 0.0f, 0.0f};
    return a.rayleigh * std::exp(-h * a.rayleighInvHeight) + a.mieExtinction * std::exp(-h * a.mieInvHeight) +
           a.ozone * atmosphereOzone(a, h);
}

// A ray (unit direction) relative to the planet: the origin's distance from
// the centre r0, b = dot(o - centre, d) and the origin's altitude h0; the
// altitude at distance t is h0 + t (2b + t) / (r0 + r(t)).
struct AtmosphereRay {
    double r0 = 0.0, b = 0.0, h0 = 0.0;
    float B = 0.0f, R0 = 0.0f, H0 = 0.0f;

    NOVA_INLINE void init(const AtmosphereView &a, Vec3f o, Vec3f d) {
        const double x = double(o.x) - a.center[0], y = double(o.y) - a.center[1], z = double(o.z) - a.center[2];
        r0 = std::sqrt(x * x + y * y + z * z);
        b = x * double(d.x) + y * double(d.y) + z * double(d.z);
        h0 = r0 - a.seaLevel;
        B = float(b);
        R0 = float(r0);
        H0 = float(h0);
    }
    NOVA_INLINE float altitude(float t) const {
        const float n = t * (2.0f * B + t);
        const float r2 = R0 * R0 + n;
        return H0 + n / (R0 + std::sqrt(r2 > 0.0f ? r2 : 0.0f));
    }
    // The distances (ascending) at which the ray meets the sphere of radius
    // seaLevel + h (h relative to sea level, in double): the number of roots.
    NOVA_INLINE int crossings(double h, double out[2]) const {
        // t^2 + 2 b t + q = 0, q = (r0 - R)(r0 + R) with r0 - R = h0 - h.
        const double R = r0 - h0 + h;
        const double q = (h0 - h) * (r0 + R);
        const double disc = b * b - q;
        if (disc < 0.0)
            return 0;
        const double s = std::sqrt(disc);
        const double u = -(b + (b >= 0.0 ? s : -s));
        if (u == 0.0) {
            out[0] = out[1] = 0.0;
            return 2;
        }
        const double t0 = u, t1 = q / u;
        out[0] = t0 < t1 ? t0 : t1;
        out[1] = t0 < t1 ? t1 : t0;
        return 2;
    }
};

// Where a ray from outside the ground sphere first meets it, before tMax
// (kInf: not before). Rays from inside the planet do not meet it (one-sided).
NOVA_INLINE float atmosphereGroundHit(const AtmosphereView &a, const AtmosphereRay &r, float tMax) {
    const double offset = a.seaLevel - a.groundRadius;
    const double above = r.h0 + offset; // the origin's height above the ground
    if (!(above > 0.0) || r.b >= 0.0)
        return kInf;
    double t[2];
    if (r.crossings(-offset, t) < 2)
        return kInf;
    // Origins within rounding of the ground (points on it) leave it upwards.
    const double eps = 1e-6 * (r.r0 > 1.0 ? r.r0 : 1.0);
    const double hit = t[0] > eps ? t[0] : (t[1] > eps ? t[1] : -1.0);
    return hit > 0.0 && hit < double(tMax) ? float(hit) : kInf;
}

// The part [t0, t1] of [0, tMax] inside the top sphere: false if none.
NOVA_INLINE bool atmosphereShell(const AtmosphereView &a, const AtmosphereRay &r, float tMax, float &t0, float &t1) {
    double t[2];
    if (r.crossings(double(a.topAltitude), t) < 2)
        return false;
    const double lo = t[0] > 0.0 ? t[0] : 0.0;
    const double hi = t[1] < double(tMax) ? t[1] : double(tMax);
    if (!(hi > lo))
        return false;
    t0 = float(lo);
    t1 = float(hi);
    return t1 > t0;
}

// The distances in [t0, t1] at which the ray crosses the atmosphere's levels,
// ascending, beginning with t0 and ending with t1 (at most max entries; past
// that, crossings are dropped and their intervals merge). Returns the count.
NOVA_INLINE int atmosphereBreaks(const AtmosphereView &a, const AtmosphereRay &r, float t0, float t1, float *out,
                                 int max) {
    int n = 0;
    out[n++] = t0;
    // The altitude range of [t0, t1]: the lowest point is the closest
    // approach if it lies inside, else an end.
    const float ha = r.altitude(t0), hb = r.altitude(t1);
    const double tc = -r.b;
    float hmin = ha < hb ? ha : hb;
    if (tc > double(t0) && tc < double(t1)) {
        const float hc = r.altitude(float(tc));
        hmin = hc < hmin ? hc : hmin;
    }
    const float hmax = ha > hb ? ha : hb;
    for (uint32_t k = 0; k < a.levelCount; ++k) {
        const float L = a.levels[k];
        if (!(L > hmin && L < hmax))
            continue;
        double t[2];
        if (r.crossings(double(L), t) < 2)
            continue;
        for (int j = 0; j < 2; ++j) {
            const float tj = float(t[j]);
            if (tj > t0 && tj < t1 && n < max - 1) {
                int i = n++;
                while (i > 1 && out[i - 1] > tj) {
                    out[i] = out[i - 1];
                    --i;
                }
                out[i] = tj;
            }
        }
    }
    out[n++] = t1;
    return n;
}

// Optical depth of [u, v] by 4-point Gauss-Legendre quadrature.
NOVA_INLINE Spectrum atmosphereDepthGL(const AtmosphereView &a, const AtmosphereRay &r, float u, float v) {
    const float m = 0.5f * (u + v), h = 0.5f * (v - u);
    if (!(h > 0.0f))
        return Spectrum{0.0f, 0.0f, 0.0f};
    const float x0 = 0.339981043584856f, x1 = 0.861136311594053f;
    const float w0 = 0.652145154862546f, w1 = 0.347854845137454f;
    const Spectrum s = (atmosphereExtinction(a, r.altitude(m - h * x0)) + atmosphereExtinction(a, r.altitude(m + h * x0))) * w0 +
                       (atmosphereExtinction(a, r.altitude(m - h * x1)) + atmosphereExtinction(a, r.altitude(m + h * x1))) * w1;
    return s * h;
}

constexpr int kAtmosphereBreaks = 40;

// Optical depth of the ray between t0 and t1 (within the shell or not).
NOVA_INLINE Spectrum atmosphereOpticalDepth(const AtmosphereView &a, const AtmosphereRay &r, float t0, float t1) {
    float s0 = 0.0f, s1 = 0.0f;
    if (!(t1 > t0) || !atmosphereShell(a, r, t1, s0, s1))
        return Spectrum{0.0f, 0.0f, 0.0f};
    s0 = s0 > t0 ? s0 : t0;
    if (!(s1 > s0))
        return Spectrum{0.0f, 0.0f, 0.0f};
    float br[kAtmosphereBreaks];
    const int n = atmosphereBreaks(a, r, s0, s1, br, kAtmosphereBreaks);
    Spectrum tau{0.0f, 0.0f, 0.0f};
    for (int i = 0; i + 1 < n; ++i)
        tau += atmosphereDepthGL(a, r, br[i], br[i + 1]);
    return tau;
}

NOVA_INLINE Spectrum expNeg(Spectrum tau) { return Spectrum{std::exp(-tau.x), std::exp(-tau.y), std::exp(-tau.z)}; }

// (1 - e^(-x)) / x, accurate near 0.
NOVA_INLINE float oneMinusExpOver(float x) {
    return std::fabs(x) < 1e-4f ? 1.0f - 0.5f * x : -std::expm1(-x) / x;
}

// A ray's segment [t0, t1] through the atmosphere, split at the levels: the
// optical depth at each break, and a proposal for an in-scattering distance:
// per interval exponential in t (rate fitted to dot(w, sigma_s T) at its
// ends), the intervals in proportion to their mass. `w` weights the
// channels (the path throughput). The weight of a sample is
// sigma_s(t) T(t) / pdf(t), with T exact (tauAt).
struct AtmosphereSegment {
    static constexpr int kMax = kAtmosphereBreaks;
    int count = 0; // intervals
    float t[kMax];
    Spectrum tau[kMax];
    float cdf[kMax]; // the proposal's mass before each break
    float rate[kMax];

    NOVA_INLINE float total() const { return count > 0 ? cdf[count] : 0.0f; }
    NOVA_INLINE float begin() const { return t[0]; }
    NOVA_INLINE float end() const { return count > 0 ? t[count] : t[0]; }
    NOVA_INLINE Spectrum depth() const { return tau[count]; }

    // [t0, t1] must lie inside the shell (atmosphereShell) or be empty.
    NOVA_INLINE void build(const AtmosphereView &a, const AtmosphereRay &r, float t0, float t1, Spectrum w,
                           bool proposal = true) {
        t[0] = t0;
        tau[0] = Spectrum{0.0f, 0.0f, 0.0f};
        cdf[0] = 0.0f;
        count = 0;
        if (!(t1 > t0))
            return;
        const int n = atmosphereBreaks(a, r, t0, t1, t, kMax);
        count = n - 1;
        // The proposal's integrand at an end of interval i (its altitude kept
        // inside the shell when the interval's middle is: breaks at sea level
        // and at the top may round outside).
        const auto f = [&](int i, float tt, const Spectrum &tauAt, bool inside) {
            float h = r.altitude(tt);
            if (inside)
                h = h < 0.0f ? 0.0f : (h > a.topAltitude ? a.topAltitude : h);
            const AtmosphereCoefficients c = atmosphereAt(a, h);
            (void)i;
            return dot(w, (c.rayleigh + c.mie) * expNeg(tauAt));
        };
        for (int i = 0; i < count; ++i) {
            const float u = t[i], v = t[i + 1], dt = v - u;
            tau[i + 1] = tau[i] + atmosphereDepthGL(a, r, u, v);
            const float hm = r.altitude(0.5f * (u + v));
            const bool inside = hm >= 0.0f && hm <= a.topAltitude;
            float mass = 0.0f, k = 0.0f;
            if (proposal && inside && dt > 0.0f) {
                const float fa = f(i, u, tau[i], true), fb = f(i, v, tau[i + 1], true);
                if (fa > 0.0f && fb > 0.0f) {
                    k = std::log(fa / fb) / dt;
                    mass = fa * dt * oneMinusExpOver(k * dt);
                } else if (fa > 0.0f || fb > 0.0f) {
                    mass = 0.5f * (fa + fb) * dt;
                }
            }
            rate[i] = k;
            cdf[i + 1] = cdf[i] + (mass > 0.0f && mass < 1e30f ? mass : 0.0f);
        }
    }

    // The interval holding distance x (clamped to the segment).
    NOVA_INLINE int interval(float x) const {
        int lo = 0, hi = count - 1;
        while (lo < hi) {
            const int mid = (lo + hi + 1) >> 1;
            if (t[mid] <= x)
                lo = mid;
            else
                hi = mid - 1;
        }
        return lo;
    }

    // Optical depth from the segment's start to x.
    NOVA_INLINE Spectrum tauAt(const AtmosphereView &a, const AtmosphereRay &r, float x) const {
        if (count == 0 || !(x > t[0]))
            return Spectrum{0.0f, 0.0f, 0.0f};
        if (x >= t[count])
            return tau[count];
        const int i = interval(x);
        return tau[i] + atmosphereDepthGL(a, r, t[i], x);
    }

    // The proposal's density and its distribution function at x.
    NOVA_INLINE float pdf(float x) const {
        const float M = total();
        if (!(M > 0.0f) || x < t[0] || x > t[count])
            return 0.0f;
        const int i = interval(x);
        const float m = cdf[i + 1] - cdf[i], dt = t[i + 1] - t[i], k = rate[i];
        if (!(m > 0.0f) || !(dt > 0.0f))
            return 0.0f;
        return (m / M) * std::exp(-k * (x - t[i])) / (dt * oneMinusExpOver(k * dt));
    }
    NOVA_INLINE float cdfAt(float x) const {
        const float M = total();
        if (!(M > 0.0f) || !(x > t[0]))
            return 0.0f;
        if (x >= t[count])
            return 1.0f;
        const int i = interval(x);
        const float m = cdf[i + 1] - cdf[i], dt = t[i + 1] - t[i], k = rate[i], dx = x - t[i];
        const float part = dt > 0.0f ? (dx * oneMinusExpOver(k * dx)) / (dt * oneMinusExpOver(k * dt)) : 0.0f;
        return (cdf[i] + m * part) / M;
    }

    // A distance from u in [0, 1) (total() must be positive); its density.
    NOVA_INLINE float sample(float u, float &density) const {
        const float M = total(), target = u * M;
        int i = 0;
        while (i + 1 < count && cdf[i + 1] <= target)
            ++i;
        while (i > 0 && !(cdf[i + 1] > cdf[i])) // rounding into an empty interval
            --i;
        const float m = cdf[i + 1] - cdf[i], dt = t[i + 1] - t[i], k = rate[i];
        float v = m > 0.0f ? (target - cdf[i]) / m : 0.5f;
        v = v < 0.0f ? 0.0f : (v > 0.99999994f ? 0.99999994f : v);
        float dx;
        if (std::fabs(k * dt) < 1e-4f) {
            dx = v * dt;
        } else {
            const float D = -std::expm1(-k * dt); // 1 - e^(-k dt)
            dx = -std::log1p(-v * D) / k;
        }
        dx = dx < 0.0f ? 0.0f : (dx > dt ? dt : dx);
        const float x = t[i] + dx;
        density = (m / M) * std::exp(-k * dx) / (dt * oneMinusExpOver(k * dt));
        return x;
    }
};

// Whether the ground is visible to rays of class `rayClass` (the low bits of
// a MediumRay's rayType).
NOVA_INLINE bool atmosphereGroundVisible(const AtmosphereView &a, uint8_t rayType) {
    return ((a.groundVisibility >> (rayType & 0x0Fu)) & 1u) != 0;
}

// The ground's outward normal at point p.
NOVA_INLINE Vec3f atmosphereNormal(const AtmosphereView &a, Vec3f p) {
    const double x = double(p.x) - a.center[0], y = double(p.y) - a.center[1], z = double(p.z) - a.center[2];
    const double l = std::sqrt(x * x + y * y + z * z);
    return l > 0.0 ? Vec3f{float(x / l), float(y / l), float(z / l)} : Vec3f{0.0f, 1.0f, 0.0f};
}

// Transmittance from p to infinity along direction d (unit): zero where the
// ground blocks.
NOVA_INLINE Spectrum atmosphereTransmittanceToInfinity(const AtmosphereView &a, Vec3f p, Vec3f d) {
    AtmosphereRay r;
    r.init(a, p, d);
    if (atmosphereGroundHit(a, r, kInf) < kInf)
        return Spectrum{0.0f, 0.0f, 0.0f};
    return expNeg(atmosphereOpticalDepth(a, r, 0.0f, kInf));
}

} // namespace nova
