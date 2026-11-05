// LUT resolution check for E(mu, r) of GGX (F = 1), parameterized x = r = sqrt(alpha), y = sqrt(mu)
#include "bsdf_sketch.h"
#include <cstdio>
#include <random>
#include <vector>
using namespace sk;

static double Emc(float mu, float r, int N, uint64_t seed) {
    std::mt19937_64 g(seed);
    std::uniform_real_distribution<float> U(0.0f, 0.99999f);
    const float a = std::fmax(r * r, 1e-4f);
    Ggx gg{a, a};
    V3 wo{std::sqrt(std::fmax(0.f, 1 - mu * mu)), 0, mu};
    double s = 0;
    // stratified 2D
    int n = int(std::sqrt(double(N)));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            V2 u{(i + U(g)) / n, (j + U(g)) / n};
            V3 m = gg.sampleVisible(wo, u);
            V3 wi = reflect(wo, m);
            if (wi.z <= 0) continue;
            s += gg.G2(wo, wi) / gg.G1(wo);
        }
    return s / (double(n) * n);
}

int main() {
    for (int res : {16, 32}) {
        std::vector<float> T(res * res);
        for (int y = 0; y < res; ++y)
            for (int x = 0; x < res; ++x) {
                float r = float(x) / (res - 1), yy = float(y) / (res - 1);
                float mu = std::fmax(yy * yy, 1e-4f);
                T[y * res + x] = float(Emc(mu, std::fmax(r, 1e-2f), 1 << 16, x * 131 + y));
            }
        auto lookup = [&](float mu, float r) {
            float fx = clamp01(r) * (res - 1), fy = clamp01(std::sqrt(mu)) * (res - 1);
            int x0 = std::min(int(fx), res - 2), y0 = std::min(int(fy), res - 2);
            float tx = fx - x0, ty = fy - y0;
            float a = lerp(T[y0 * res + x0], T[y0 * res + x0 + 1], tx);
            float b = lerp(T[(y0 + 1) * res + x0], T[(y0 + 1) * res + x0 + 1], tx);
            return lerp(a, b, ty);
        };
        std::mt19937_64 g(99);
        std::uniform_real_distribution<float> U(0.0f, 1.0f);
        double maxErr = 0, sumErr = 0;
        int K = 300;
        for (int k = 0; k < K; ++k) {
            float mu = 0.01f + 0.99f * U(g), r = 0.05f + 0.95f * U(g);
            double ref = Emc(mu, r, 1 << 18, 1000 + k);
            double e = std::fabs(lookup(mu, r) - ref);
            maxErr = std::max(maxErr, e);
            sumErr += e;
        }
        std::printf("res %dx%d: max |lut - E| = %.5f, mean = %.5f  (E(1,1)=%.4f E(0.01,1)=%.4f)\n", res, res, maxErr,
                    sumErr / K, T[(res - 1) * res + res - 1], T[res - 1]);
    }
}
