#pragma once
// Statistical BSDF checks for Nova's closure tests (tests/bsdf_stats.h).
// Self-contained: only <cmath>, <vector>, <functional>, <algorithm>.
//
//  * chi2Test: pbrt-v4 / Mitsuba design (bsdfs_test.cpp): sample histogram
//    over (theta, phi) of the whole sphere vs. the pdf integrated per cell
//    (open Gauss-Legendre rule), low-expectation cells pooled (minExpFrequency),
//    Sidak-corrected significance over all runs of a test case.
//  * pdfIntegral: integral of pdf over the sphere (must equal the
//    probability of a non-delta sample).
//  * albedoEstimates: E(wo) by importance sampling (f cos / pdf) and by
//    uniform sphere sampling (f cos * 4 pi): two independent estimators.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nova::test {

struct V3 { double x, y, z; };

// The lobe under test, in its local frame (+Z = shading normal).
struct LobeUnderTest {
    // u in [0,1)^3; returns false if no sample. delta = specular (no pdf).
    std::function<bool(V3 wo, V3 u, V3 &wi, double &pdf, double &weight, bool &delta)> sample;
    std::function<double(V3 wo, V3 wi)> pdf;      // solid-angle density of sample()
    std::function<double(V3 wo, V3 wi)> evalCos;  // f(wo, wi) |cos wi| (one channel)
};

// Small deterministic RNG (PCG32) so the tests do not depend on the renderer's.
struct Pcg32 {
    uint64_t state = 0x853c49e6748fea9bULL, inc = 0xda3e39cb94b95bdbULL;
    explicit Pcg32(uint64_t seed) { state += seed; next(); }
    uint32_t next() {
        const uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const uint32_t xs = uint32_t(((old >> 18u) ^ old) >> 27u), rot = uint32_t(old >> 59u);
        return (xs >> rot) | (xs << ((~rot + 1u) & 31u));
    }
    double uniform() { return double(next()) * (1.0 / 4294967296.0); }
};

inline double normalCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

// Upper tail P(X >= x) of chi^2 with k degrees of freedom. Wilson-Hilferty
// cube-root normal approximation: relative error of the tail below ~1e-3 for
// k >= 30 at the 1e-4..1e-2 levels used here (dof is in the hundreds/thousands).
inline double chi2UpperTail(double x, int k) {
    const double t = 2.0 / (9.0 * k);
    const double z = (std::cbrt(x / k) - (1.0 - t)) / std::sqrt(t);
    return 1.0 - normalCdf(z);
}

// Open Gauss-Legendre rule (never evaluates cell borders: a pdf that is
// nonzero exactly at cos(theta) = 0 must not leak into the cells below the
// horizon, which a closed rule such as Simpson's does). 8 x 8 nodes per
// cell, each cell split into sub x sub pieces.
inline double cellIntegral(const std::function<double(double, double)> &f, double x0, double y0, double x1,
                           double y1, int sub = 2) {
    static const double gx[8] = {-0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
                                 0.1834346424956498,  0.5255324099163290,  0.7966664774136267,  0.9602898564975363};
    static const double gw[8] = {0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
                                 0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763};
    double sum = 0.0;
    const double hx = (x1 - x0) / sub, hy = (y1 - y0) / sub;
    for (int a = 0; a < sub; ++a)
        for (int b = 0; b < sub; ++b)
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j)
                    sum += gw[i] * gw[j] *
                           f(x0 + hx * (a + 0.5 * (gx[i] + 1.0)), y0 + hy * (b + 0.5 * (gx[j] + 1.0)));
    return sum * 0.25 * hx * hy;
}

inline V3 sphDir(double theta, double phi) {
    return {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
}

struct Chi2Result {
    bool pass = false;
    double pValue = 0.0, alpha = 0.0, pdfMass = 0.0;
    int dof = 0;
    std::string message;
};

// One run: wo fixed. thetaRes x 2 thetaRes cells over the whole sphere.
inline Chi2Result chi2Test(const LobeUnderTest &lobe, V3 wo, Pcg32 &rng, int sampleCount = 1 << 20,
                           int thetaRes = 48, double minExpFrequency = 5.0, double significance = 0.01,
                           int runsInTestCase = 5) {
    const int phiRes = 2 * thetaRes, n = thetaRes * phiRes;
    std::vector<double> obs(size_t(n), 0.0), expct(size_t(n), 0.0);
    const double kPi = 3.14159265358979323846;
    for (int i = 0; i < sampleCount; ++i) {
        V3 wi{}, u{rng.uniform(), rng.uniform(), rng.uniform()};
        double pdf = 0.0, w = 0.0;
        bool delta = false;
        if (!lobe.sample(wo, u, wi, pdf, w, delta) || delta)
            continue;
        const double theta = std::acos(std::clamp(wi.z, -1.0, 1.0));
        double phi = std::atan2(wi.y, wi.x);
        if (phi < 0)
            phi += 2 * kPi;
        const int ti = std::min(thetaRes - 1, int(theta / kPi * thetaRes));
        const int pj = std::min(phiRes - 1, int(phi / (2 * kPi) * phiRes));
        obs[size_t(ti * phiRes + pj)] += 1.0;
    }
    double mass = 0.0;
    for (int ti = 0; ti < thetaRes; ++ti)
        for (int pj = 0; pj < phiRes; ++pj) {
            const double v = cellIntegral(
                [&](double th, double ph) { return lobe.pdf(wo, sphDir(th, ph)) * std::sin(th); },
                ti * kPi / thetaRes, pj * 2 * kPi / phiRes, (ti + 1) * kPi / thetaRes, (pj + 1) * 2 * kPi / phiRes);
            expct[size_t(ti * phiRes + pj)] = v * sampleCount;
            mass += v;
        }

    // Pool cells by ascending expectation (pbrt-v4 Chi2Test).
    std::vector<int> order(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        order[size_t(i)] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return expct[size_t(a)] < expct[size_t(b)]; });
    double pooledObs = 0, pooledExp = 0, chsq = 0;
    int dof = 0;
    Chi2Result r;
    r.pdfMass = mass;
    for (int c : order) {
        const double e = expct[size_t(c)], o = obs[size_t(c)];
        if (e == 0.0) {
            if (o > sampleCount * 1e-5) {
                r.message = "samples in a cell with zero pdf";
                return r;
            }
        } else if (e < minExpFrequency || (pooledExp > 0 && pooledExp < minExpFrequency)) {
            pooledObs += o;
            pooledExp += e;
        } else {
            chsq += (o - e) * (o - e) / e;
            ++dof;
        }
    }
    if (pooledExp > 0 || pooledObs > 0) {
        chsq += (pooledObs - pooledExp) * (pooledObs - pooledExp) / std::max(pooledExp, 1e-12);
        ++dof;
    }
    dof -= 1;
    r.dof = dof;
    if (dof <= 0) {
        r.message = "too few degrees of freedom";
        return r;
    }
    r.pValue = chi2UpperTail(chsq, dof);
    r.alpha = 1.0 - std::pow(1.0 - significance, 1.0 / runsInTestCase);
    r.pass = r.pValue >= r.alpha && std::isfinite(r.pValue);
    if (!r.pass)
        r.message = "rejected: p = " + std::to_string(r.pValue) + " < " + std::to_string(r.alpha);
    return r;
}

// E(wo) two ways; both must agree (and match a reference table).
struct AlbedoPair { double importance = 0, importanceErr = 0, uniform = 0, uniformErr = 0; };
inline AlbedoPair albedoEstimates(const LobeUnderTest &lobe, V3 wo, Pcg32 &rng, int n = 1 << 18) {
    const double kPi = 3.14159265358979323846;
    double s1 = 0, s1q = 0, s2 = 0, s2q = 0;
    for (int i = 0; i < n; ++i) {
        V3 wi{}, u{rng.uniform(), rng.uniform(), rng.uniform()};
        double pdf = 0, w = 0;
        bool delta = false;
        const double v = lobe.sample(wo, u, wi, pdf, w, delta) ? w : 0.0; // weight = f cos / pdf
        s1 += v;
        s1q += v * v;
        const double z = 1 - 2 * rng.uniform(), r = std::sqrt(std::max(0.0, 1 - z * z)), ph = 2 * kPi * rng.uniform();
        const double g = lobe.evalCos(wo, {r * std::cos(ph), r * std::sin(ph), z}) * 4 * kPi;
        s2 += g;
        s2q += g * g;
    }
    AlbedoPair a;
    a.importance = s1 / n;
    a.importanceErr = std::sqrt(std::max(0.0, s1q / n - a.importance * a.importance) / n);
    a.uniform = s2 / n;
    a.uniformErr = std::sqrt(std::max(0.0, s2q / n - a.uniform * a.uniform) / n);
    return a;
}

} // namespace nova::test
