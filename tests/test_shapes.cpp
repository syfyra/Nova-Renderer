#include "core/lights/shapes.h"
#include "core/math/distribution.h"
#include "core/math/sobol.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <functional>
#include <vector>

using namespace nova;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr int kSamples = 1 << 16;

Vec2f sample2D(uint32_t i) { return Sampler::make(17, 0, i).get2D(stream::kLens); }

// Irradiance E / L at `ref` (normal +Z) from a uniform emitter, estimated with
// the shape's own samples; only front faces seen from ref emit.
double estimateIrradiance(const std::function<ShapeSample(Vec2f)> &sampleShape, Vec3f ref) {
    double sum = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        const ShapeSample s = sampleShape(sample2D(uint32_t(i)));
        if (!(s.pdf > 0.0f))
            continue;
        const Vec3f wi = normalize(s.p - ref);
        if (dot(s.n, wi) >= 0.0f) // the back side faces ref
            continue;
        sum += std::fmax(0.0f, wi.z) / s.pdf;
    }
    return sum / kSamples;
}

// Solid angle of the front-facing part of a parametric surface, by brute
// force quadrature: sum of cos / d^2 dA.
double solidAngleByQuadrature(const std::function<void(double, double, Vec3f &, Vec3f &, double &)> &surface, Vec3f ref,
                              int n = 1000) {
    double omega = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            Vec3f p, nrm;
            double dA;
            surface((i + 0.5) / n, (j + 0.5) / n, p, nrm, dA);
            const Vec3f d = p - ref;
            const double d2 = dot(d, d);
            const double c = -double(dot(nrm, d)) / std::sqrt(d2);
            if (c > 0.0)
                omega += c / d2 * dA;
        }
    }
    return omega;
}

// Parallel-rectangle form factor for a point below one corner (A = a/h, B = b/h).
double cornerFormFactor(double A, double B) {
    const double sa = std::sqrt(1.0 + A * A), sb = std::sqrt(1.0 + B * B);
    return (A / sa * std::atan(B / sa) + B / sb * std::atan(A / sb)) / (2.0 * kPi);
}

} // namespace

TEST_CASE("Sphere sampling matches the analytic irradiance", "[shapes]") {
    const Vec3f center{0.0f, 0.0f, 3.0f};
    const Vec3f ref{0.0f, 0.0f, 0.0f};
    for (const float radius : {1.0f, 0.01f}) { // the second uses the small-angle series
        const double e = estimateIrradiance([&](Vec2f u) { return sampleSphere(center, radius, ref, u); }, ref);
        CHECK_THAT(e, WithinRel(kPi * radius * radius / 9.0, 2e-3));

        bool consistent = true;
        for (int i = 0; i < 256; ++i) {
            const ShapeSample s = sampleSphere(center, radius, ref, sample2D(uint32_t(i)));
            consistent &= std::fabs(length(s.p - center) - radius) < 1e-4f * (1.0f + radius);
            consistent &= std::fabs(sphereSamplePdf(center, radius, ref, s.p, s.n) - s.pdf) <= 1e-4f * s.pdf;
            float t = 0.0f;
            const Vec3f d = normalize(s.p - ref);
            consistent &= intersectSphere(center, radius, ref, d, kInf, t);
            consistent &= std::fabs(t - length(s.p - ref)) < 1e-3f;
        }
        CHECK(consistent);
    }

    // From inside, the sphere covers every direction.
    double omega = 0.0;
    const Vec3f inside{0.2f, -0.3f, 3.1f};
    for (int i = 0; i < kSamples; ++i) {
        const ShapeSample s = sampleSphere(center, 1.0f, inside, sample2D(uint32_t(i)));
        omega += s.pdf > 0.0f ? 1.0 / s.pdf : 0.0;
    }
    CHECK_THAT(omega / kSamples, WithinRel(4.0 * kPi, 0.01));
}

TEST_CASE("Rectangle sampling matches the analytic irradiance", "[shapes]") {
    const Vec3f ref{0.0f, 0.0f, 0.0f};
    // A 2 x 2 square centered 2 above ref, facing it (normal -Z).
    const Vec3f s{-1.0f, -1.0f, 2.0f}, ex{0.0f, 2.0f, 0.0f}, ey{2.0f, 0.0f, 0.0f};
    const double expected = kPi * 4.0 * cornerFormFactor(0.5, 0.5);
    CHECK_THAT(estimateIrradiance([&](Vec2f u) { return sampleRectangle(s, ex, ey, ref, u); }, ref),
               WithinRel(expected, 2e-3));

    // Every regime (spherical sampling; area fallback when tiny or nearly a
    // hemisphere) keeps the density consistent with the solid angle. Near
    // the plane, area sampling is too noisy for a short estimate (BSDF
    // sampling covers that case under MIS), so only consistency is checked.
    for (const float height : {2.0f, 200.0f, 0.001f}) {
        const Vec3f corner{-1.0f, -1.0f, height};
        auto surface = [&](double a, double b, Vec3f &p, Vec3f &n, double &dA) {
            p = corner + ex * float(a) + ey * float(b);
            n = normalize(cross(ex, ey));
            dA = 4.0 / 1e6;
        };
        const double omega = solidAngleByQuadrature(surface, ref);
        double estimate = 0.0;
        bool consistent = true;
        for (int i = 0; i < kSamples; ++i) {
            const ShapeSample smp = sampleRectangle(corner, ex, ey, ref, sample2D(uint32_t(i)));
            estimate += smp.pdf > 0.0f ? 1.0 / smp.pdf : 0.0;
            if (i < 256) {
                consistent &= std::fabs(rectangleSamplePdf(corner, ex, ey, ref, smp.p) - smp.pdf) <= 1e-3f * smp.pdf;
                float t = 0.0f;
                consistent &= intersectParallelogram(corner, ex, ey, ref, normalize(smp.p - ref), kInf, t);
            }
        }
        CHECK(consistent);
        if (height > 0.01f)
            CHECK_THAT(estimate / kSamples, WithinRel(omega, 0.01));
    }
}

TEST_CASE("Disk sampling matches the analytic irradiance", "[shapes]") {
    const Vec3f ref{0.0f, 0.0f, 0.0f};
    const Vec3f center{0.0f, 0.0f, 2.0f};
    const Vec3f au{0.0f, 1.0f, 0.0f}, av{1.0f, 0.0f, 0.0f}; // normal -Z, towards ref
    const double e = estimateIrradiance([&](Vec2f u) { return sampleDisk(center, au, av, ref, u); }, ref);
    CHECK_THAT(e, WithinRel(kPi * 1.0 / (4.0 + 1.0), 2e-3));

    float t = 0.0f;
    CHECK(intersectDisk(center, au, av, ref, normalize(Vec3f{0.4f, 0.5f, 2.0f}), kInf, t));
    CHECK_FALSE(intersectDisk(center, au, av, ref, normalize(Vec3f{0.8f, 0.7f, 2.0f}), kInf, t));
}

TEST_CASE("Cylinder sampling covers its visible solid angle", "[shapes]") {
    const Vec3f center{0.5f, 0.0f, 3.0f};
    const Vec3f axis{1.5f, 0.0f, 0.0f}; // half length along X
    const Vec3f ry{0.0f, 0.5f, 0.0f}, rz{0.0f, 0.0f, 0.5f};
    const Vec3f ref{0.0f, 0.2f, 0.0f};
    auto surface = [&](double a, double b, Vec3f &p, Vec3f &n, double &dA) {
        const double phi = kTwoPi * b;
        const Vec3f radial = ry * float(std::cos(phi)) + rz * float(std::sin(phi));
        p = center + axis * float(2.0 * a - 1.0) + radial;
        n = normalize(radial);
        dA = (kTwoPi * 0.5 * 3.0) / 1e6;
    };
    const double omega = solidAngleByQuadrature(surface, ref);
    double estimate = 0.0;
    bool consistent = true;
    for (int i = 0; i < kSamples; ++i) {
        const ShapeSample s = sampleCylinder(center, axis, ry, rz, ref, sample2D(uint32_t(i)));
        const Vec3f wi = normalize(s.p - ref);
        if (dot(s.n, wi) >= 0.0f)
            continue; // back side
        estimate += 1.0 / s.pdf;
        if (i < 512) {
            float t = 0.0f;
            Vec3f n;
            bool front = false;
            consistent &= intersectCylinder(center, axis, 0.5f, ref, wi, kInf, t, n, front);
            consistent &= front;
            consistent &= std::fabs(t - length(s.p - ref)) < 1e-3f;
            consistent &= std::fabs(cylinderSamplePdf(axis, 0.5f, ref, s.p, s.n) - s.pdf) <= 1e-4f * s.pdf;
        }
    }
    CHECK(consistent);
    CHECK_THAT(estimate / kSamples, WithinRel(omega, 0.01));
}

// A 1 cm tube seen from 30 m (D/r = 3000): the hit/miss decision and the side
// must follow the exact geometry, which the textbook discriminant
// qb^2 - qa qc loses to cancellation.
TEST_CASE("Thin distant cylinders intersect exactly", "[shapes]") {
    const Vec3f center{0.0f, 0.0f, -30.0f};
    const Vec3f axis{0.7f, 0.5f, 0.1f}; // generic orientation, half length |axis|
    const float radius = 0.01f;
    const Vec3f a = normalize(axis);
    const Vec3f side = normalize(cross(a, Vec3f{0.0f, 0.0f, 1.0f})); // perpendicular to the axis and the view
    int wrong = 0, wrongSide = 0;
    const int n = 2001;
    for (int i = 0; i < n; ++i) {
        // Rays from the origin aimed across the tube, from -1.5 r to 1.5 r off the axis.
        const double offset = (double(i) / double(n - 1) * 3.0 - 1.5) * radius;
        const Vec3f target = center + side * float(offset);
        const Vec3f d = normalize(target);
        // Exact miss distance from the axis line, in double.
        const double ox = -double(center.x), oy = -double(center.y), oz = -double(center.z);
        const double ax = a.x, ay = a.y, az = a.z, dx = d.x, dy = d.y, dz = d.z;
        const double cx = ay * dz - az * dy, cy = az * dx - ax * dz, cz = ax * dy - ay * dx; // a x d
        const double cl = std::sqrt(cx * cx + cy * cy + cz * cz);
        const double miss = std::fabs(ox * cx + oy * cy + oz * cz) / cl;
        if (std::fabs(miss - radius) < 1e-3 * radius)
            continue; // too close to the silhouette to call
        float t = 0.0f;
        Vec3f nrm;
        bool front = false;
        const bool hit = intersectCylinder(center, axis, radius, {0.0f, 0.0f, 0.0f}, d, kInf, t, nrm, front);
        wrong += hit != (miss < radius);
        wrongSide += hit && !front;
    }
    CHECK(wrong == 0);
    CHECK(wrongSide == 0);
}

TEST_CASE("2D distributions sample in proportion to the function", "[shapes][distribution]") {
    constexpr uint32_t nu = 8, nv = 4;
    std::vector<float> f(nu * nv);
    for (uint32_t v = 0; v < nv; ++v)
        for (uint32_t u = 0; u < nu; ++u)
            f[v * nu + u] = (u == 3 && v == 1) ? 0.0f : float(1 + u + 2 * v);
    double total = 0.0;
    for (float x : f)
        total += x;

    Distribution2D dist;
    dist.build(f.data(), nu, nv);
    const Distribution2DView view = dist.view();
    CHECK_THAT(view.integral, WithinRel(total / (nu * nv), 1e-5));

    std::vector<int> hist(nu * nv, 0);
    bool pdfMatches = true;
    for (int i = 0; i < kSamples; ++i) {
        float pdf = 0.0f;
        const Vec2f p = view.sample(sample2D(uint32_t(i)), pdf);
        REQUIRE(p.x >= 0.0f);
        REQUIRE(p.x < 1.0f);
        REQUIRE(p.y >= 0.0f);
        REQUIRE(p.y < 1.0f);
        pdfMatches &= std::fabs(view.pdf(p) - pdf) <= 1e-5f * pdf;
        ++hist[size_t(uint32_t(p.y * nv) * nu + uint32_t(p.x * nu))];
    }
    CHECK(pdfMatches);
    for (size_t c = 0; c < hist.size(); ++c)
        CHECK_THAT(double(hist[c]) / kSamples, WithinAbs(f[c] / total, 2e-3));
    CHECK(hist[1 * nu + 3] == 0);
}
