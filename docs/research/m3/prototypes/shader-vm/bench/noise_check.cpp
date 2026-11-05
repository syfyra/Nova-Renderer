// Port of MaterialX 1.39 mx_perlin_noise_float(vec3) (stdlib/genglsl/lib/mx_noise.glsl)
// as a branch-free scalar function; checks it against Karma CPU probe values
// and measures it in a batch (SoA) loop vs. a per-lane loop.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

static inline uint32_t rotl32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
static inline uint32_t bjfinal(uint32_t a, uint32_t b, uint32_t c) {
    c ^= b; c -= rotl32(b, 14);
    a ^= c; a -= rotl32(c, 11);
    b ^= a; b -= rotl32(a, 25);
    c ^= b; c -= rotl32(b, 16);
    a ^= c; a -= rotl32(c, 4);
    b ^= a; b -= rotl32(a, 14);
    c ^= b; c -= rotl32(b, 24);
    return c;
}
static inline uint32_t hash3(int x, int y, int z) {
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + (3u << 2u) + 13u;
    a += uint32_t(x); b += uint32_t(y); c += uint32_t(z);
    return bjfinal(a, b, c);
}
static inline float sel(bool b, float t, float f) { return b ? t : f; }
static inline float negif(float v, bool b) { return b ? -v : v; }
static inline float grad3(uint32_t hash, float x, float y, float z) {
    uint32_t h = hash & 15u;
    float u = sel(h < 8u, x, y);
    float v = sel(h < 4u, y, sel((h == 12u) || (h == 14u), x, z));
    return negif(u, (h & 1u) != 0) + negif(v, (h & 2u) != 0);
}
static inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static inline float trilerp(float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7, float s,
                            float t, float r) {
    float s1 = 1.0f - s, t1 = 1.0f - t, r1 = 1.0f - r;
    return (r1 * (t1 * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s)) + r * (t1 * (v4 * s1 + v5 * s) + t * (v6 * s1 + v7 * s)));
}
static inline float perlin3(float px, float py, float pz) {
    const float flx = std::floor(px), fly = std::floor(py), flz = std::floor(pz);
    const int X = int(flx), Y = int(fly), Z = int(flz);
    const float fx = px - flx, fy = py - fly, fz = pz - flz;
    const float u = fade(fx), v = fade(fy), w = fade(fz);
    const float r = trilerp(grad3(hash3(X, Y, Z), fx, fy, fz), grad3(hash3(X + 1, Y, Z), fx - 1, fy, fz),
                            grad3(hash3(X, Y + 1, Z), fx, fy - 1, fz), grad3(hash3(X + 1, Y + 1, Z), fx - 1, fy - 1, fz),
                            grad3(hash3(X, Y, Z + 1), fx, fy, fz - 1), grad3(hash3(X + 1, Y, Z + 1), fx - 1, fy, fz - 1),
                            grad3(hash3(X, Y + 1, Z + 1), fx, fy - 1, fz - 1),
                            grad3(hash3(X + 1, Y + 1, Z + 1), fx - 1, fy - 1, fz - 1), u, v, w);
    return 0.9820f * r;
}

template <int N>
void noiseBatch(const float *__restrict x, const float *__restrict y, const float *__restrict z, float *__restrict out) {
    for (int l = 0; l < N; ++l) out[l] = perlin3(x[l], y[l], z[l]);
}

int main() {
    // Karma CPU probe (husk BRAY_HdKarma, ND_noise3d_float of Pobject, amplitude 1, pivot 0)
    struct P { float x, y, z, karma; } probes[] = {
        {-0.96875f, 0.96875f, 0.0f, 0.061339f}, {-0.46875f, 0.34375f, 0.0f, 0.027602f}, {0.96875f, -0.96875f, 0.0f, -0.030115f}};
    for (auto &p : probes)
        std::printf("Pobj(%.5f, %.5f, %.1f): port %.6f  karma %.6f  diff %.2e\n", p.x, p.y, p.z, perlin3(p.x, p.y, p.z),
                    p.karma, double(perlin3(p.x, p.y, p.z) - p.karma));

    const int n = 1 << 16;
    std::vector<float> X(n), Y(n), Z(n), O(n);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> U(-50.0f, 50.0f);
    for (int i = 0; i < n; ++i) { X[i] = U(rng); Y[i] = U(rng); Z[i] = U(rng); }
    using C = std::chrono::high_resolution_clock;
    double sink = 0;
    auto t0 = C::now();
    for (int it = 0; it < 50; ++it)
        for (int i = 0; i < n; i += 32) noiseBatch<32>(&X[i], &Y[i], &Z[i], &O[i]), sink += O[i];
    auto t1 = C::now();
    std::printf("noise3d batch (32-lane SoA loop): %.2f ns per lane (sink %g)\n",
                std::chrono::duration<double, std::nano>(t1 - t0).count() / (50.0 * n), sink);
    // per lane, opaque call to stop vectorization
    float (*volatile fn)(float, float, float) = perlin3;
    t0 = C::now();
    for (int it = 0; it < 50; ++it)
        for (int i = 0; i < n; ++i) O[i] = fn(X[i], Y[i], Z[i]);
    t1 = C::now();
    std::printf("noise3d scalar (opaque call):     %.2f ns per lane (sink %g)\n",
                std::chrono::duration<double, std::nano>(t1 - t0).count() / (50.0 * n), double(O[7]));
    return 0;
}
