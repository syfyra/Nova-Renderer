// Validates the statistical harness (tests/bsdf_stats.h) before any lobe is
// tested with it: correct samplers pass, samplers whose pdf disagrees with
// their sampling fail with p ~ 0, and the two albedo estimators agree. The
// GGX lobe here is an independent reference implementation in double
// (visible-normal sampling, Heitz 2018; height-correlated Smith G2; F = 1).

#include "bsdf_stats.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace nova::test;

namespace {

constexpr double kPi = 3.14159265358979323846;

double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) {
    const double l = std::sqrt(dot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

// Cosine-weighted Lambert (albedo 1). The wrong variant samples the
// hemisphere uniformly while its pdf still claims cosine weighting.
LobeUnderTest lambert(bool wrongSampling = false) {
    LobeUnderTest l;
    l.sample = [wrongSampling](V3 wo, V3 u, V3 &wi, double &pdf, double &w, bool &delta) {
        delta = false;
        if (wo.z <= 0)
            return false;
        if (wrongSampling) {
            const double z = u.x, r = std::sqrt(std::fmax(0.0, 1 - z * z));
            wi = {r * std::cos(2 * kPi * u.y), r * std::sin(2 * kPi * u.y), z};
        } else {
            const double r = std::sqrt(u.x);
            wi = {r * std::cos(2 * kPi * u.y), r * std::sin(2 * kPi * u.y), std::sqrt(std::fmax(0.0, 1 - u.x))};
        }
        pdf = wi.z / kPi;
        w = 1.0;
        return pdf > 0;
    };
    l.pdf = [](V3 wo, V3 wi) { return wo.z > 0 && wi.z > 0 ? wi.z / kPi : 0.0; };
    l.evalCos = [](V3 wo, V3 wi) { return wo.z > 0 && wi.z > 0 ? wi.z / kPi : 0.0; };
    return l;
}

struct Ggx {
    double ax, ay;
    double D(V3 h) const {
        if (h.z <= 0)
            return 0;
        const double e = h.x * h.x / (ax * ax) + h.y * h.y / (ay * ay) + h.z * h.z;
        return 1.0 / (kPi * ax * ay * e * e);
    }
    double lambda(V3 w) const {
        return 0.5 * (std::sqrt(1 + (ax * ax * w.x * w.x + ay * ay * w.y * w.y) / (w.z * w.z)) - 1);
    }
    double G1(V3 w) const { return 1 / (1 + lambda(w)); }
    double G2(V3 o, V3 i) const { return 1 / (1 + lambda(o) + lambda(i)); }
    double visibleD(V3 wo, V3 h) const { return G1(wo) * std::fmax(0.0, dot(wo, h)) * D(h) / wo.z; }
    V3 sampleVisible(V3 wo, double u1, double u2) const {
        const V3 vh = norm({ax * wo.x, ay * wo.y, wo.z});
        const double lensq = vh.x * vh.x + vh.y * vh.y;
        const V3 t1 = lensq > 0 ? V3{-vh.y / std::sqrt(lensq), vh.x / std::sqrt(lensq), 0} : V3{1, 0, 0};
        const V3 t2 = {vh.y * t1.z - vh.z * t1.y, vh.z * t1.x - vh.x * t1.z, vh.x * t1.y - vh.y * t1.x};
        const double r = std::sqrt(u1), phi = 2 * kPi * u2, s = 0.5 * (1 + vh.z);
        const double p1 = r * std::cos(phi);
        const double p2 = (1 - s) * std::sqrt(std::fmax(0.0, 1 - p1 * p1)) + s * r * std::sin(phi);
        const double p3 = std::sqrt(std::fmax(0.0, 1 - p1 * p1 - p2 * p2));
        const V3 nh = {p1 * t1.x + p2 * t2.x + p3 * vh.x, p1 * t1.y + p2 * t2.y + p3 * vh.y,
                       p1 * t1.z + p2 * t2.z + p3 * vh.z};
        return norm({ax * nh.x, ay * nh.y, std::fmax(1e-9, nh.z)});
    }
};

// GGX reflection with F = 1. The wrong variant's pdf uses the Jacobian
// 1 / (4 cos(wi)) instead of 1 / (4 wo.h).
LobeUnderTest ggx(double ax, double ay, bool wrongJacobian = false) {
    const Ggx g{ax, ay};
    LobeUnderTest l;
    l.sample = [g](V3 wo, V3 u, V3 &wi, double &pdf, double &w, bool &delta) {
        delta = false;
        if (wo.z <= 0)
            return false;
        const V3 h = g.sampleVisible(wo, u.x, u.y);
        const double oh = dot(wo, h);
        wi = {2 * oh * h.x - wo.x, 2 * oh * h.y - wo.y, 2 * oh * h.z - wo.z};
        if (wi.z <= 0)
            return false;
        pdf = g.visibleD(wo, h) / (4 * oh);
        w = g.G2(wo, wi) / g.G1(wo);
        return pdf > 0;
    };
    l.pdf = [g, wrongJacobian](V3 wo, V3 wi) {
        if (wo.z <= 0 || wi.z <= 0)
            return 0.0;
        const V3 h = norm({wo.x + wi.x, wo.y + wi.y, wo.z + wi.z});
        return g.visibleD(wo, h) / (wrongJacobian ? 4 * std::fmax(1e-9, wi.z) : 4 * dot(wo, h));
    };
    l.evalCos = [g](V3 wo, V3 wi) {
        if (wo.z <= 0 || wi.z <= 0)
            return 0.0;
        const V3 h = norm({wo.x + wi.x, wo.y + wi.y, wo.z + wi.z});
        return g.D(h) * g.G2(wo, wi) / (4 * wo.z);
    };
    return l;
}

V3 direction(double cosTheta, double phi) {
    const double s = std::sqrt(1 - cosTheta * cosTheta);
    return {s * std::cos(phi), s * std::sin(phi), cosTheta};
}

} // namespace

TEST_CASE("The chi-square harness accepts correct samplers and rejects wrong pdfs", "[bsdf][stats]") {
    struct Case {
        const char *name;
        LobeUnderTest lobe;
        bool correct;
    };
    const Case cases[] = {
        {"Lambert", lambert(), true},
        {"Lambert, uniform sampling", lambert(true), false},
        {"GGX 0.3", ggx(0.3, 0.3), true},
        {"GGX anisotropic 0.6 / 0.15", ggx(0.6, 0.15), true},
        {"GGX 0.3, wrong Jacobian", ggx(0.3, 0.3, true), false},
    };
    Pcg32 rng(42);
    for (const Case &c : cases) {
        for (const double cosTheta : {0.2, 0.7}) {
            INFO(c.name << ", cos(wo) " << cosTheta);
            const Chi2Result r = chi2Test(c.lobe, direction(cosTheta, 0.7), rng, 1 << 19, 48, 5.0, 0.01, 2);
            INFO(r.message << " (p " << r.pValue << ", dof " << r.dof << ")");
            CHECK(r.pass == c.correct);
            if (c.correct) {
                // Lambert loses no mass; GGX loses the samples reflected below
                // the horizon, which sample() rejects as well.
                CHECK(r.pdfMass <= 1.0 + 1e-3);
                CHECK(r.pdfMass > 0.8);
            } else {
                CHECK(r.pValue < 1e-6);
            }
        }
    }
}

TEST_CASE("The two albedo estimators of the harness agree", "[bsdf][stats]") {
    Pcg32 rng(7);
    for (const double cosTheta : {0.1, 0.5, 1.0}) {
        INFO("cos(wo) " << cosTheta);
        const AlbedoPair lam = albedoEstimates(lambert(), direction(cosTheta, 0.3), rng, 1 << 17);
        CHECK_THAT(lam.importance, Catch::Matchers::WithinAbs(1.0, 1e-12));
        CHECK(std::fabs(lam.uniform - 1.0) < 4 * lam.uniformErr + 1e-3);
        const AlbedoPair g = albedoEstimates(ggx(0.5, 0.5), direction(cosTheta, 0.3), rng, 1 << 17);
        CHECK(std::fabs(g.importance - g.uniform) < 4 * std::hypot(g.importanceErr, g.uniformErr));
        CHECK(g.importance < 1.0);
    }
}
