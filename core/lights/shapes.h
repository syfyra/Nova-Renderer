#pragma once

// Sampling and ray intersection of the emitting shapes of area lights: the
// geometry behind UsdLux sphere, rect, disk and cylinder lights. Everything
// is in world space, and sample densities are solid-angle densities as seen
// from a reference point. This file is convention-free; which side emits and
// how much lives in lights.h.

#include "core/math/frame.h"
#include "core/math/sampling.h"

namespace nova {

struct ShapeSample {
    Vec3f p;   // point on the shape
    Vec3f n;   // unit surface normal at p (outward / front side)
    float pdf; // solid-angle density at the reference point; 0 if none
};

// Converts an area density at p (normal n) to a solid-angle density at ref.
NOVA_INLINE float areaToSolidAngle(float pdfArea, Vec3f ref, Vec3f p, Vec3f n) {
    const Vec3f d = p - ref;
    const float dist2 = dot(d, d);
    const float cosLight = std::fabs(dot(n, d)) / std::sqrt(dist2);
    return dist2 > 0.0f && cosLight > 0.0f ? pdfArea * dist2 / cosLight : 0.0f;
}

// ---------------------------------------------------------------------------
// Sphere: uniform sampling of the cone it subtends (pbrt-v4), area sampling
// from inside.
// ---------------------------------------------------------------------------

// 1 - cos(thetaMax) of the cone subtended by a sphere, with the small-angle
// series where the direct formula cancels. Returns -1 if ref is inside.
NOVA_INLINE float sphereConeOneMinusCos(float dist2, float radius) {
    const float sin2ThetaMax = radius * radius / dist2;
    if (!(sin2ThetaMax < 1.0f))
        return -1.0f;
    if (sin2ThetaMax < 0.00068523f) // sin^2(1.5 deg)
        return 0.5f * sin2ThetaMax;
    return 1.0f - safeSqrt(1.0f - sin2ThetaMax);
}

NOVA_INLINE ShapeSample sampleSphere(Vec3f center, float radius, Vec3f ref, Vec2f u) {
    ShapeSample s;
    const Vec3f toCenter = center - ref;
    const float dist2 = dot(toCenter, toCenter);
    const float omc = sphereConeOneMinusCos(dist2, radius);
    if (omc < 0.0f) {
        s.n = sampleUniformSphere(u);
        s.p = center + s.n * radius;
        s.pdf = areaToSolidAngle(1.0f / (4.0f * kPi * radius * radius), ref, s.p, s.n);
        return s;
    }
    const float dist = std::sqrt(dist2);
    const float sin2ThetaMax = radius * radius / dist2;
    const float sinThetaMax = std::sqrt(sin2ThetaMax);
    float cosTheta = 1.0f - u.x * omc;
    float sin2Theta = 1.0f - cosTheta * cosTheta;
    if (sin2ThetaMax < 0.00068523f) {
        sin2Theta = sin2ThetaMax * u.x;
        cosTheta = std::sqrt(1.0f - sin2Theta);
    }
    // Angle at the center between the axis and the sampled surface point.
    const float cosAlpha = sin2Theta / sinThetaMax + cosTheta * safeSqrt(1.0f - sin2Theta / sin2ThetaMax);
    const float sinAlpha = safeSqrt(1.0f - cosAlpha * cosAlpha);
    const float phi = kTwoPi * u.y;
    const Vec3f w{sinAlpha * std::cos(phi), sinAlpha * std::sin(phi), cosAlpha};
    s.n = -Frame::fromNormal(toCenter / dist).toWorld(w);
    s.p = center + s.n * radius;
    s.pdf = 1.0f / (kTwoPi * omc);
    return s;
}

// Density with which sampleSphere() produces the point p (normal n).
NOVA_INLINE float sphereSamplePdf(Vec3f center, float radius, Vec3f ref, Vec3f p, Vec3f n) {
    const Vec3f toCenter = center - ref;
    const float omc = sphereConeOneMinusCos(dot(toCenter, toCenter), radius);
    if (omc < 0.0f)
        return areaToSolidAngle(1.0f / (4.0f * kPi * radius * radius), ref, p, n);
    return 1.0f / (kTwoPi * omc);
}

// Nearest t in (0, tMax) where the ray o + t d (d unit) meets the sphere.
NOVA_INLINE bool intersectSphere(Vec3f center, float radius, Vec3f o, Vec3f d, float tMax, float &tHit) {
    const Vec3f oc = o - center;
    const float b = dot(oc, d);
    const Vec3f perp = oc - d * b; // closest approach, computed without cancellation
    const float r2 = radius * radius;
    const float disc = r2 - dot(perp, perp);
    if (disc < 0.0f)
        return false;
    const float c = dot(oc, oc) - r2;
    const float q = -b - std::copysign(std::sqrt(disc), b);
    float t0 = q, t1 = q != 0.0f ? c / q : 0.0f;
    if (t0 > t1) {
        const float tmp = t0;
        t0 = t1;
        t1 = tmp;
    }
    if (t0 > 0.0f && t0 < tMax) {
        tHit = t0;
        return true;
    }
    if (t1 > 0.0f && t1 < tMax) {
        tHit = t1;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Rectangle: corner s with edges ex, ey (orthogonal). Uniform sampling of the
// spherical rectangle it subtends (Urena et al. 2013, as in pbrt-v4), with a
// fallback to area sampling when the solid angle is tiny or ill-conditioned.
// ---------------------------------------------------------------------------

// The rectangle s + a ex + b ey (a, b in [0, 1]) with what sampling needs of
// it, independent of the reference point (precomputed per rect light).
struct RectFrame {
    Vec3f s;          // corner
    Vec3f t, b;       // unit edge directions
    Vec3f tb;         // cross(t, b)
    Vec3f n;          // unit normal, normalize(cross(ex, ey))
    float lu, lv;     // edge lengths
    float area;
    bool orthogonal;  // not sheared into a parallelogram (spherical sampling applies)

    NOVA_INLINE static RectFrame make(Vec3f s, Vec3f ex, Vec3f ey) {
        RectFrame r;
        r.s = s;
        r.lu = length(ex);
        r.lv = length(ey);
        r.t = ex / r.lu;
        r.b = ey / r.lv;
        r.tb = cross(r.t, r.b);
        const Vec3f nCross = cross(ex, ey);
        r.area = length(nCross);
        r.n = nCross / r.area;
        r.orthogonal = std::fabs(dot(r.t, r.b)) <= 1e-4f;
        return r;
    }
};

struct SphericalRectangle {
    Frame frame;     // x along ex, y along ey, z towards the reference point's side
    float x0, y0, z0, x1, y1;
    float b0, b1, solidAngle;
    float g2g3;      // g2 + g3

    // asin(x) for x in [0, 1], absolute error below 2e-8 (Abramowitz and
    // Stegun 4.4.46), inline instead of a CRT call (docs/design/M7.md D8).
    NOVA_FORCEINLINE static float asinUnit(float x) {
        const float p = ((((((-0.0012624911f * x + 0.0066700901f) * x - 0.0170881256f) * x + 0.0308918810f) * x -
                            0.0501743046f) * x + 0.0889789874f) * x - 0.2145988016f) * x + 1.5707963050f;
        return 1.5707963268f - std::sqrt(nova::fmax(0.0f, 1.0f - x)) * p;
    }
    NOVA_INLINE static float angleBetween(Vec3f a, Vec3f b) {
        // Numerically stable angle between unit vectors.
        if (dot(a, b) < 0.0f)
            return kPi - 2.0f * asinUnit(nova::fmin(1.0f, 0.5f * length(a + b)));
        return 2.0f * asinUnit(nova::fmin(1.0f, 0.5f * length(b - a)));
    }

    // Returns false if the rectangle is degenerate as seen from ref.
    NOVA_INLINE bool init(const RectFrame &r, Vec3f ref) {
        if (!r.orthogonal)
            return false; // sheared into a parallelogram: area sampling
        frame.t = r.t;
        frame.b = r.b;
        frame.n = r.tb;
        const float exl = r.lu, eyl = r.lv;
        const Vec3f d = r.s - ref;
        x0 = dot(d, frame.t);
        y0 = dot(d, frame.b);
        z0 = dot(d, frame.n);
        if (z0 > 0.0f) { // z must point away from the rectangle
            frame.n = -frame.n;
            z0 = -z0;
        }
        x1 = x0 + exl;
        y1 = y0 + eyl;
        if (!(z0 < 0.0f))
            return false;

        // Normals of the planes through ref and each edge, in closed form
        // (Urena et al. 2013): normalize(cross(v00, v10)) etc. for the
        // corners v_ij = (x_i, y_j, z0), whose cross products have one zero
        // component.
        const float zz = z0 * z0;
        const float r0 = 1.0f / std::sqrt(zz + y0 * y0), r1 = 1.0f / std::sqrt(zz + x1 * x1);
        const float r2 = 1.0f / std::sqrt(zz + y1 * y1), r3 = 1.0f / std::sqrt(zz + x0 * x0);
        const Vec3f n0{0.0f, z0 * r0, -y0 * r0};
        const Vec3f n1{-z0 * r1, 0.0f, x1 * r1};
        const Vec3f n2{0.0f, -z0 * r2, y1 * r2};
        const Vec3f n3{z0 * r3, 0.0f, -x0 * r3};
        const float g0 = angleBetween(-n0, n1), g1 = angleBetween(-n1, n2);
        const float g2 = angleBetween(-n2, n3), g3 = angleBetween(-n3, n0);
        b0 = n0.z;
        b1 = n2.z;
        g2g3 = g2 + g3;
        solidAngle = g0 + g1 + g2 + g3 - kTwoPi;
        return solidAngle > 0.0f && isFinite(solidAngle);
    }

    // Point on the rectangle (world space) for u in [0, 1)^2, relative to ref.
    NOVA_INLINE Vec3f sample(Vec3f ref, Vec2f u) const {
        const float au = u.x * solidAngle - g2g3; // = u (g0 + g1 - 2 pi) + (u - 1)(g2 + g3)
        const float fu = (std::cos(au) * b0 - b1) / std::sin(au);
        float cu = std::copysign(1.0f, fu) / std::sqrt(fu * fu + b0 * b0);
        cu = nova::fmin(nova::fmax(cu, -0x1.fffffep-1f), 0x1.fffffep-1f);
        float xu = -(cu * z0) / safeSqrt(1.0f - cu * cu);
        xu = nova::fmin(nova::fmax(xu, x0), x1);
        const float dd = std::sqrt(xu * xu + z0 * z0);
        const float h0 = y0 / std::sqrt(dd * dd + y0 * y0);
        const float h1 = y1 / std::sqrt(dd * dd + y1 * y1);
        const float hv = h0 + u.y * (h1 - h0), hv2 = hv * hv;
        const float yv = hv2 < 1.0f - 1e-6f ? hv * dd / std::sqrt(1.0f - hv2) : y1;
        return ref + frame.toWorld({xu, yv, z0});
    }
};

// Solid angles below this are sampled by area (the spherical construction
// loses precision); above it the spherical sampling pays off (pbrt-v4 uses
// the same bounds).
constexpr float kMinSphericalRectSolidAngle = 3e-4f;
constexpr float kMaxSphericalRectSolidAngle = 6.22f;

// Samples the rectangle (s, ex, ey; normal = normalize(cross(ex, ey))).
NOVA_INLINE ShapeSample sampleRectangle(const RectFrame &r, Vec3f ref, Vec2f u) {
    ShapeSample out;
    out.n = r.n;
    SphericalRectangle sr;
    if (sr.init(r, ref) && sr.solidAngle > kMinSphericalRectSolidAngle && sr.solidAngle < kMaxSphericalRectSolidAngle) {
        out.p = sr.sample(ref, u);
        out.pdf = 1.0f / sr.solidAngle;
        return out;
    }
    out.p = r.s + r.t * (r.lu * u.x) + r.b * (r.lv * u.y);
    out.pdf = areaToSolidAngle(1.0f / r.area, ref, out.p, out.n);
    return out;
}
NOVA_INLINE ShapeSample sampleRectangle(Vec3f s, Vec3f ex, Vec3f ey, Vec3f ref, Vec2f u) {
    return sampleRectangle(RectFrame::make(s, ex, ey), ref, u);
}

// Density with which sampleRectangle() produces a point p on the rectangle.
NOVA_INLINE float rectangleSamplePdf(const RectFrame &r, Vec3f ref, Vec3f p) {
    SphericalRectangle sr;
    if (sr.init(r, ref) && sr.solidAngle > kMinSphericalRectSolidAngle && sr.solidAngle < kMaxSphericalRectSolidAngle)
        return 1.0f / sr.solidAngle;
    return areaToSolidAngle(1.0f / r.area, ref, p, r.n);
}
NOVA_INLINE float rectangleSamplePdf(Vec3f s, Vec3f ex, Vec3f ey, Vec3f ref, Vec3f p) {
    return rectangleSamplePdf(RectFrame::make(s, ex, ey), ref, p);
}

// Intersection with the parallelogram s + a ex + b ey, a, b in [0, 1].
NOVA_INLINE bool intersectParallelogram(Vec3f s, Vec3f ex, Vec3f ey, Vec3f o, Vec3f d, float tMax, float &tHit) {
    const Vec3f n = cross(ex, ey);
    const float denom = dot(d, n);
    if (denom == 0.0f)
        return false;
    const float t = dot(s - o, n) / denom;
    if (!(t > 0.0f && t < tMax))
        return false;
    const Vec3f q = o + d * t - s;
    const float nn = dot(n, n);
    const float a = dot(cross(q, ey), n) / nn;
    const float b = dot(cross(ex, q), n) / nn;
    if (a < 0.0f || a > 1.0f || b < 0.0f || b > 1.0f)
        return false;
    tHit = t;
    return true;
}

// ---------------------------------------------------------------------------
// Disk: an ellipse center + a au + b av with a^2 + b^2 <= 1, area sampled.
// ---------------------------------------------------------------------------

// Shirley-Chiu concentric mapping of the unit square onto the unit disk.
NOVA_INLINE Vec2f sampleConcentricDisk(Vec2f u) {
    const float x = 2.0f * u.x - 1.0f, y = 2.0f * u.y - 1.0f;
    if (x == 0.0f && y == 0.0f)
        return {0.0f, 0.0f};
    float r, theta;
    if (std::fabs(x) > std::fabs(y)) {
        r = x;
        theta = 0.25f * kPi * (y / x);
    } else {
        r = y;
        theta = 0.5f * kPi - 0.25f * kPi * (x / y);
    }
    return {r * std::cos(theta), r * std::sin(theta)};
}

NOVA_INLINE ShapeSample sampleDisk(Vec3f center, Vec3f au, Vec3f av, Vec3f ref, Vec2f u) {
    ShapeSample s;
    const Vec2f d = sampleConcentricDisk(u);
    const Vec3f nCross = cross(au, av);
    const float area = kPi * length(nCross);
    s.n = normalize(nCross);
    s.p = center + au * d.x + av * d.y;
    s.pdf = areaToSolidAngle(1.0f / area, ref, s.p, s.n);
    return s;
}

NOVA_INLINE bool intersectDisk(Vec3f center, Vec3f au, Vec3f av, Vec3f o, Vec3f d, float tMax, float &tHit) {
    const Vec3f n = cross(au, av);
    const float denom = dot(d, n);
    if (denom == 0.0f)
        return false;
    const float t = dot(center - o, n) / denom;
    if (!(t > 0.0f && t < tMax))
        return false;
    const Vec3f q = o + d * t - center;
    const float nn = dot(n, n);
    const float a = dot(cross(q, av), n) / nn;
    const float b = dot(cross(au, q), n) / nn;
    if (a * a + b * b > 1.0f)
        return false;
    tHit = t;
    return true;
}

// ---------------------------------------------------------------------------
// Cylinder: open tube center + a axis + radius (cos phi ry + sin phi rz),
// a in [-1, 1], with orthogonal radius vectors ry, rz of equal length.
// ---------------------------------------------------------------------------

NOVA_INLINE ShapeSample sampleCylinder(Vec3f center, Vec3f axis, Vec3f ry, Vec3f rz, Vec3f ref, Vec2f u) {
    ShapeSample s;
    const float phi = kTwoPi * u.y;
    const Vec3f radial = ry * std::cos(phi) + rz * std::sin(phi);
    const float radius = length(ry);
    const float area = kTwoPi * radius * 2.0f * length(axis);
    s.p = center + axis * (2.0f * u.x - 1.0f) + radial;
    s.n = radial / radius;
    s.pdf = areaToSolidAngle(1.0f / area, ref, s.p, s.n);
    return s;
}

// Nearest t in (0, tMax) where the ray o + t d meets the tube. `normal` is the
// outward normal; `front` tells whether the ray meets the outside (from
// outside the infinite cylinder, at its first root). Deciding the side from
// the root keeps it exact at grazing angles, where the sign of dot(normal, d)
// is lost to rounding.
NOVA_INLINE bool intersectCylinder(Vec3f center, Vec3f axis, float radius, Vec3f o, Vec3f d, float tMax, float &tHit,
                                   Vec3f &normal, bool &front) {
    const float halfLength = length(axis);
    const Vec3f a = axis / halfLength;
    const Vec3f oc = o - center;
    // Components perpendicular to the axis.
    const Vec3f dp = d - a * dot(d, a);
    const Vec3f op = oc - a * dot(oc, a);
    const float qa = dot(dp, dp);
    if (qa == 0.0f)
        return false;
    const float qb = dot(op, dp);
    // qb^2 - qa qc = qa (r^2 - |perp|^2) with the closest approach perp of
    // the projected ray to the axis: no cancellation for thin, distant tubes
    // (as in intersectSphere).
    const Vec3f perp = op - dp * (qb / qa);
    const float r2 = radius * radius;
    const float disc = qa * (r2 - dot(perp, perp));
    if (disc < 0.0f)
        return false;
    const float qc = dot(op, op) - r2;
    const float q = -qb - std::copysign(std::sqrt(disc), qb);
    float t0 = q / qa, t1 = q != 0.0f ? qc / q : t0;
    if (t0 > t1) {
        const float tmp = t0;
        t0 = t1;
        t1 = tmp;
    }
    const float roots[2] = {t0, t1};
    for (int i = 0; i < 2; ++i) {
        const float t = roots[i];
        if (!(t > 0.0f && t < tMax))
            continue;
        const Vec3f p = oc + d * t;
        const float h = dot(p, a);
        if (std::fabs(h) > halfLength)
            continue;
        tHit = t;
        normal = normalize(p - a * h);
        front = i == 0 && qc > 0.0f;
        return true;
    }
    return false;
}

NOVA_INLINE float cylinderSamplePdf(Vec3f axis, float radius, Vec3f ref, Vec3f p, Vec3f n) {
    const float area = kTwoPi * radius * 2.0f * length(axis);
    return areaToSolidAngle(1.0f / area, ref, p, n);
}

// ---------------------------------------------------------------------------
// Triangle: corners v0, v0 + e1, v0 + e2 (emissive meshes, docs/design/M9.md
// D14). Uniform sampling of the spherical triangle it subtends (Arvo 1995,
// as in pbrt-v4, without its cosine warp) within the rectangle's solid-angle
// bounds, area sampling outside them. The point is returned with its
// barycentrics (b1, b2): p = v0 + b1 e1 + b2 e2.
// ---------------------------------------------------------------------------

// Solid angle of the triangle seen from ref (Van Oosterom and Strackee).
NOVA_INLINE float triangleSolidAngle(Vec3f v0, Vec3f e1, Vec3f e2, Vec3f ref) {
    const Vec3f a = v0 - ref, b = a + e1, c = a + e2;
    const float la = length(a), lb = length(b), lc = length(c);
    if (!(la > 0.0f && lb > 0.0f && lc > 0.0f))
        return 0.0f;
    const float num = std::fabs(dot(a, cross(b, c)));
    const float den = la * lb * lc + dot(a, b) * lc + dot(a, c) * lb + dot(b, c) * la;
    const float omega = 2.0f * std::atan2(num, den);
    return omega >= 0.0f ? omega : omega + kTwoPi;
}

NOVA_INLINE bool sphericalTriangleSampled(float solidAngle) {
    return solidAngle > kMinSphericalRectSolidAngle && solidAngle < kMaxSphericalRectSolidAngle;
}

NOVA_INLINE ShapeSample sampleTriangle(Vec3f v0, Vec3f e1, Vec3f e2, Vec3f ref, Vec2f u, Vec2f &bary) {
    ShapeSample s;
    const Vec3f nCross = cross(e1, e2);
    const float area2 = length(nCross);
    s.n = nCross / area2;
    s.pdf = 0.0f;
    if (!(area2 > 0.0f))
        return s;
    const float omega = triangleSolidAngle(v0, e1, e2, ref);
    if (sphericalTriangleSampled(omega)) {
        const Vec3f a = normalize(v0 - ref), b = normalize(v0 + e1 - ref), c = normalize(v0 + e2 - ref);
        Vec3f nab = cross(a, b), nbc = cross(b, c), nca = cross(c, a);
        if (dot(nab, nab) > 0.0f && dot(nbc, nbc) > 0.0f && dot(nca, nca) > 0.0f) {
            nab = normalize(nab);
            nbc = normalize(nbc);
            nca = normalize(nca);
            const auto angle = SphericalRectangle::angleBetween;
            const float alpha = angle(nab, -nca), beta = angle(nbc, -nab), gamma = angle(nca, -nbc);
            // The sub-triangle's area A' = u A, as alpha + beta' + gamma' - pi.
            const float apPi = kPi + u.x * (alpha + beta + gamma - kPi);
            const float cosA = std::cos(alpha), sinA = std::sin(alpha);
            const float sinPhi = std::sin(apPi) * cosA - std::cos(apPi) * sinA;
            const float cosPhi = std::cos(apPi) * cosA + std::sin(apPi) * sinA;
            const float k1 = cosPhi + cosA, k2 = sinPhi - sinA * dot(a, b);
            float cosBp = (k2 + (k2 * cosPhi - k1 * sinPhi) * cosA) / ((k2 * sinPhi + k1 * cosPhi) * sinA);
            cosBp = nova::fmin(nova::fmax(cosBp, -1.0f), 1.0f);
            const float sinBp = safeSqrt(1.0f - cosBp * cosBp);
            const Vec3f ca = c - a * dot(c, a);
            const Vec3f cp = a * cosBp + (dot(ca, ca) > 0.0f ? normalize(ca) : ca) * sinBp;
            const float cosT = 1.0f - u.y * (1.0f - dot(cp, b));
            const float sinT = safeSqrt(1.0f - cosT * cosT);
            const Vec3f cpb = cp - b * dot(cp, b);
            const Vec3f w = b * cosT + (dot(cpb, cpb) > 0.0f ? normalize(cpb) : cpb) * sinT;
            // Where the direction meets the triangle's plane: its barycentrics.
            const Vec3f s1 = cross(w, e2);
            const float div = dot(s1, e1);
            if (div != 0.0f) {
                const Vec3f q = ref - v0;
                float b1 = dot(q, s1) / div, b2 = dot(w, cross(q, e1)) / div;
                b1 = nova::fmin(nova::fmax(b1, 0.0f), 1.0f);
                b2 = nova::fmin(nova::fmax(b2, 0.0f), 1.0f);
                if (b1 + b2 > 1.0f) {
                    const float sum = b1 + b2;
                    b1 /= sum;
                    b2 /= sum;
                }
                bary = {b1, b2};
                s.p = v0 + e1 * b1 + e2 * b2;
                s.pdf = 1.0f / omega;
                return s;
            }
        }
    }
    // Uniform area sampling (folding the unit square onto the triangle).
    float b1 = u.x, b2 = u.y;
    if (b1 + b2 > 1.0f) {
        b1 = 1.0f - b1;
        b2 = 1.0f - b2;
    }
    bary = {b1, b2};
    s.p = v0 + e1 * b1 + e2 * b2;
    s.pdf = areaToSolidAngle(2.0f / area2, ref, s.p, s.n);
    return s;
}

// Density with which sampleTriangle() produces a point p on the triangle.
NOVA_INLINE float triangleSamplePdf(Vec3f v0, Vec3f e1, Vec3f e2, Vec3f n, float area, Vec3f ref, Vec3f p) {
    const float omega = triangleSolidAngle(v0, e1, e2, ref);
    if (sphericalTriangleSampled(omega))
        return 1.0f / omega;
    return area > 0.0f ? areaToSolidAngle(1.0f / area, ref, p, n) : 0.0f;
}

} // namespace nova
