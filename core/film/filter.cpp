#include "core/film/filter.h"

#include <cmath>

namespace nova {

namespace {

// Cosine-sum windows: a0 + a1 cos(pi x / R) + a2 cos(2 pi x / R) + a3 cos(3 pi x / R).
struct CosineWindow {
    double a[4];
    double baseRadius;
};
constexpr CosineWindow kBlackman{{0.42, 0.5, 0.08, 0.0}, 1.5};
constexpr CosineWindow kBlackmanHarris{{0.35875, 0.48829, 0.14128, 0.01168}, 1.8};

const CosineWindow *windowOf(PixelFilterType type) {
    if (type == PixelFilterType::Blackman)
        return &kBlackman;
    if (type == PixelFilterType::BlackmanHarris)
        return &kBlackmanHarris;
    return nullptr;
}

// Exact CDF of a window on [-R, R].
double windowCdf(const CosineWindow &w, double R, double x) {
    if (x <= -R)
        return 0.0;
    if (x >= R)
        return 1.0;
    double s = w.a[0] * x;
    for (int k = 1; k < 4; ++k)
        s += w.a[k] * R / (k * M_PI) * std::sin(k * M_PI * x / R);
    return 0.5 + s / (2.0 * w.a[0] * R);
}

} // namespace

float pixelFilterProfile(const PixelFilter &f, float x) {
    const float ax = std::fabs(x);
    if (f.type == PixelFilterType::Point || ax > f.radius)
        return 0.0f;
    switch (f.type) {
    case PixelFilterType::Box:
        return 1.0f;
    case PixelFilterType::Gaussian:
        return std::exp(-0.5f * x * x / (f.sigma * f.sigma));
    case PixelFilterType::Triangle:
        return 1.0f - ax / f.radius;
    default: {
        const CosineWindow &w = *windowOf(f.type);
        double v = 0.0;
        for (int k = 0; k < 4; ++k)
            v += w.a[k] * std::cos(k * M_PI * double(x) / double(f.radius));
        return float(v);
    }
    }
}

PixelFilter makePixelFilter(PixelFilterType type, float scale) {
    PixelFilter f;
    if (type == PixelFilterType::Point || !(scale > 0.0f) || !std::isfinite(scale))
        return f;
    f.type = type;
    switch (type) {
    case PixelFilterType::Box:
        f.radius = 0.5f * scale;
        break;
    case PixelFilterType::Gaussian:
        f.sigma = 0.5f * scale;
        f.radius = 4.0f * f.sigma;                          // cuts 6.3e-5 of the mass per axis
        f.gaussErf = float(std::erf(4.0 / std::sqrt(2.0))); // 0.99993666
        break;
    case PixelFilterType::Triangle:
        f.radius = scale;
        break;
    default: {
        const CosineWindow &w = *windowOf(type);
        const double R = w.baseRadius * double(scale);
        f.radius = float(R);
        // Invert the exact CDF at the table nodes by bisection.
        for (uint32_t k = 0; k <= PixelFilter::kTableSize; ++k) {
            const double u = double(k) / PixelFilter::kTableSize;
            double lo = -R, hi = R;
            for (int it = 0; it < 64; ++it) {
                const double m = 0.5 * (lo + hi);
                (windowCdf(w, R, m) < u ? lo : hi) = m;
            }
            f.invCdf[k] = float(0.5 * (lo + hi));
        }
        f.invCdf[0] = -f.radius;
        f.invCdf[PixelFilter::kTableSize] = f.radius;
        break;
    }
    }
    return f;
}

} // namespace nova
