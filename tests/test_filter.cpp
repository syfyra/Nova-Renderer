#include "core/film/filter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace nova;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr PixelFilterType kFilters[] = {PixelFilterType::Box, PixelFilterType::Gaussian, PixelFilterType::Blackman,
                                        PixelFilterType::BlackmanHarris, PixelFilterType::Triangle};

// Normalized integral of the filter profile over [a, b].
double profileMass(const PixelFilter &f, double a, double b) {
    const double r = f.radius;
    constexpr int kSteps = 20000;
    double total = 0.0, part = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const double x = -r + (i + 0.5) * (2.0 * r / kSteps);
        const double v = pixelFilterProfile(f, float(x));
        total += v;
        if (x >= a && x < b)
            part += v;
    }
    return part / total;
}

} // namespace

TEST_CASE("Pixel filter samples follow the filter profile", "[filter]") {
    for (const PixelFilterType type : kFilters) {
        for (const float scale : {0.5f, 1.0f, 2.0f}) {
            const PixelFilter f = makePixelFilter(type, scale);
            const double r = f.radius;
            REQUIRE(r > 0.0);

            constexpr int kBins = 16, n = 1 << 16;
            std::vector<int> hist(kBins, 0);
            double mean = 0.0;
            bool inside = true, symmetric = true;
            for (int i = 0; i < n; ++i) {
                const float u = (float(i) + 0.5f) / float(n);
                const float x = f.sample1D(u);
                inside &= std::fabs(x) <= f.radius * 1.000001f;
                symmetric &= std::fabs(x + f.sample1D(1.0f - u)) < 1e-5f * (1.0f + f.radius);
                mean += x;
                const int bin = int((double(x) + r) / (2.0 * r) * kBins);
                ++hist[size_t(bin < 0 ? 0 : bin >= kBins ? kBins - 1 : bin)];
            }
            CHECK(inside);
            CHECK(symmetric);
            CHECK_THAT(mean / n, WithinAbs(0.0, 1e-4));
            for (int b = 0; b < kBins; ++b) {
                const double a = -r + 2.0 * r * b / kBins, e = a + 2.0 * r / kBins;
                CHECK_THAT(double(hist[size_t(b)]) / n, WithinAbs(profileMass(f, a, e), 1e-3));
            }
        }
    }
}

// Per-axis variances of Karma's filters (measured against Karma CPU 22).
TEST_CASE("Pixel filters have Karma's sizes", "[filter]") {
    struct Expected {
        PixelFilterType type;
        double variance; // per axis at scale 1
        double tolerance;
    };
    const Expected cases[] = {{PixelFilterType::Box, 1.0 / 12.0, 1e-4},
                              {PixelFilterType::Gaussian, 0.249732, 1e-4},
                              {PixelFilterType::Blackman, 0.228920, 2e-3},
                              {PixelFilterType::BlackmanHarris, 0.248628, 2e-3},
                              {PixelFilterType::Triangle, 1.0 / 6.0, 1e-3}};
    for (const Expected &c : cases) {
        for (const float scale : {0.5f, 1.0f, 2.0f}) {
            const PixelFilter f = makePixelFilter(c.type, scale);
            constexpr int n = 1 << 18;
            double sum2 = 0.0;
            for (int i = 0; i < n; ++i) {
                const double x = f.sample1D((float(i) + 0.5f) / float(n));
                sum2 += x * x;
            }
            CHECK_THAT(sum2 / n, WithinRel(c.variance * scale * scale, c.tolerance));
        }
    }
}

TEST_CASE("The Gaussian filter inverts its CDF", "[filter]") {
    const PixelFilter f = makePixelFilter(PixelFilterType::Gaussian, 1.0f);
    CHECK(f.sigma == 0.5f);
    double worst = 0.0;
    for (int i = 0; i <= 1000; ++i) {
        const double u = std::fmin(i / 1000.0, 1.0 - 1e-7);
        const double x = f.sample1D(float(u));
        const double cdf = 0.5 + 0.5 * std::erf(x / (0.5 * std::sqrt(2.0))) / f.gaussErf;
        worst = std::fmax(worst, std::fabs(cdf - u));
    }
    CHECK(worst < 2e-6);
    CHECK(std::isfinite(f.sample1D(0.0f))); // u = 0 occurs with Sobol samples
}

TEST_CASE("Filter profiles and the point filter", "[filter]") {
    const PixelFilter gauss = makePixelFilter(PixelFilterType::Gaussian, 1.0f);
    CHECK_THAT(pixelFilterProfile(gauss, 0.5f), WithinRel(std::exp(-0.5), 1e-5)); // one sigma
    CHECK(pixelFilterProfile(gauss, 2.01f) == 0.0f);                              // cut at 4 sigma
    for (const PixelFilterType window : {PixelFilterType::Blackman, PixelFilterType::BlackmanHarris}) {
        const PixelFilter f = makePixelFilter(window, 1.0f);
        CHECK_THAT(pixelFilterProfile(f, 0.0f), WithinAbs(1.0, 1e-6)); // the coefficients sum to 1
        CHECK_THAT(pixelFilterProfile(f, f.radius * 0.999f), WithinAbs(0.0, 1e-4));
    }
    CHECK(makePixelFilter(PixelFilterType::Blackman, 1.0f).radius == 1.5f);
    CHECK(makePixelFilter(PixelFilterType::BlackmanHarris, 1.0f).radius == 1.8f);
    CHECK(makePixelFilter(PixelFilterType::Box, 1.0f).radius == 0.5f);

    for (const PixelFilter &point :
         {makePixelFilter(PixelFilterType::Point, 1.0f), makePixelFilter(PixelFilterType::Gaussian, 0.0f)}) {
        for (const float u : {0.0f, 0.3f, 0.999f}) {
            const Vec2f o = point.sample({u, 1.0f - u});
            CHECK(o.x == 0.0f);
            CHECK(o.y == 0.0f);
        }
    }
}
