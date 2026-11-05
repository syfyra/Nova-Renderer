// 8-wide AVX2 MaterialX perlin3 via OIIO simd.h (vfloat8/vint8), vs scalar.
#include <OpenImageIO/simd.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using OIIO::simd::vfloat8;
using OIIO::simd::vint8;
using OIIO::simd::vbool8;

static inline vint8 bjfinal(vint8 a, vint8 b, vint8 c) {
    using OIIO::simd::rotl;
    c ^= b; c -= rotl(b, 14);
    a ^= c; a -= rotl(c, 11);
    b ^= a; b -= rotl(a, 25);
    c ^= b; c -= rotl(b, 16);
    a ^= c; a -= rotl(c, 4);
    b ^= a; b -= rotl(a, 14);
    c ^= b; c -= rotl(b, 24);
    return c;
}
static inline vint8 hash3(vint8 x, vint8 y, vint8 z) {
    const vint8 s(int(0xdeadbeefu + (3u << 2u) + 13u));
    return bjfinal(s + x, s + y, s + z);
}
static inline vfloat8 negif(vfloat8 v, vbool8 b) { return OIIO::simd::select(b, -v, v); }
static inline vfloat8 grad3(vint8 hash, vfloat8 x, vfloat8 y, vfloat8 z) {
    const vint8 h = hash & vint8(15);
    const vfloat8 u = OIIO::simd::select(h < vint8(8), x, y);
    const vbool8 h12or14 = (h == vint8(12)) | (h == vint8(14));
    const vfloat8 v = OIIO::simd::select(h < vint8(4), y, OIIO::simd::select(h12or14, x, z));
    return negif(u, (h & vint8(1)) != vint8(0)) + negif(v, (h & vint8(2)) != vint8(0));
}
static inline vfloat8 fade(vfloat8 t) { return t * t * t * (t * (t * vfloat8(6.0f) - vfloat8(15.0f)) + vfloat8(10.0f)); }

static inline vfloat8 perlin3(vfloat8 px, vfloat8 py, vfloat8 pz) {
    const vfloat8 flx = OIIO::simd::floor(px), fly = OIIO::simd::floor(py), flz = OIIO::simd::floor(pz);
    const vint8 X = OIIO::simd::ifloor(px), Y = OIIO::simd::ifloor(py), Z = OIIO::simd::ifloor(pz);
    const vfloat8 fx = px - flx, fy = py - fly, fz = pz - flz;
    const vfloat8 u = fade(fx), v = fade(fy), w = fade(fz);
    const vint8 one(1);
    const vfloat8 f1(1.0f);
    const vfloat8 g0 = grad3(hash3(X, Y, Z), fx, fy, fz);
    const vfloat8 g1 = grad3(hash3(X + one, Y, Z), fx - f1, fy, fz);
    const vfloat8 g2 = grad3(hash3(X, Y + one, Z), fx, fy - f1, fz);
    const vfloat8 g3 = grad3(hash3(X + one, Y + one, Z), fx - f1, fy - f1, fz);
    const vfloat8 g4 = grad3(hash3(X, Y, Z + one), fx, fy, fz - f1);
    const vfloat8 g5 = grad3(hash3(X + one, Y, Z + one), fx - f1, fy, fz - f1);
    const vfloat8 g6 = grad3(hash3(X, Y + one, Z + one), fx, fy - f1, fz - f1);
    const vfloat8 g7 = grad3(hash3(X + one, Y + one, Z + one), fx - f1, fy - f1, fz - f1);
    const vfloat8 s1 = f1 - u, t1 = f1 - v, r1 = f1 - w;
    const vfloat8 r = r1 * (t1 * (g0 * s1 + g1 * u) + v * (g2 * s1 + g3 * u)) +
                      w * (t1 * (g4 * s1 + g5 * u) + v * (g6 * s1 + g7 * u));
    return vfloat8(0.9820f) * r;
}

int main() {
    alignas(32) float px[8] = {-0.96875f, -0.46875f, 0.96875f, 0, 0, 0, 0, 0};
    alignas(32) float py[8] = {0.96875f, 0.34375f, -0.96875f, 0, 0, 0, 0, 0};
    alignas(32) float pz[8] = {0};
    alignas(32) float o[8];
    perlin3(vfloat8(px), vfloat8(py), vfloat8(pz)).store(o);
    std::printf("simd: %.6f %.6f %.6f  (karma 0.061339 0.027602 -0.030115)\n", o[0], o[1], o[2]);

    const int n = 1 << 16;
    std::vector<float> X(n), Y(n), Z(n), O(n);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> U(-50.0f, 50.0f);
    for (int i = 0; i < n; ++i) { X[i] = U(rng); Y[i] = U(rng); Z[i] = U(rng); }
    using C = std::chrono::high_resolution_clock;
    double sink = 0;
    auto t0 = C::now();
    for (int it = 0; it < 50; ++it)
        for (int i = 0; i < n; i += 32) {
            for (int k = 0; k < 32; k += 8)
                perlin3(vfloat8(&X[i + k]), vfloat8(&Y[i + k]), vfloat8(&Z[i + k])).store(&O[i + k]);
            sink += O[i];
        }
    auto t1 = C::now();
    std::printf("noise3d AVX2 (32-lane chunk of 4x vfloat8): %.2f ns per lane (sink %g)\n",
                std::chrono::duration<double, std::nano>(t1 - t0).count() / (50.0 * n), sink);
    return 0;
}
