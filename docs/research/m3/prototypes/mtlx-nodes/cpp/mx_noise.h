#pragma once
// Port of MaterialX 1.39 libraries/stdlib/genglsl/lib/mx_noise.glsl (itself a
// port of OSL's oslnoise.h). Bit-exact integer hashing; float math in float32.
#include <cstdint>
#include <cmath>
#ifndef NOVA_INLINE
#define NOVA_INLINE inline
#endif
namespace mxn {
struct V2 { float x, y; };
struct V3 { float x, y, z; };

NOVA_INLINE int mxFloor(float x) { return int(std::floor(x)); }
NOVA_INLINE float floorFrac(float x, int &i) { i = mxFloor(x); return x - float(i); }
NOVA_INLINE uint32_t rotl32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
NOVA_INLINE void bjmix(uint32_t &a, uint32_t &b, uint32_t &c) {
    a -= c; a ^= rotl32(c, 4);  c += b;
    b -= a; b ^= rotl32(a, 6);  a += c;
    c -= b; c ^= rotl32(b, 8);  b += a;
    a -= c; a ^= rotl32(c, 16); c += b;
    b -= a; b ^= rotl32(a, 19); a += c;
    c -= b; c ^= rotl32(b, 4);  b += a;
}
NOVA_INLINE uint32_t bjfinal(uint32_t a, uint32_t b, uint32_t c) {
    c ^= b; c -= rotl32(b, 14);
    a ^= c; a -= rotl32(c, 11);
    b ^= a; b -= rotl32(a, 25);
    c ^= b; c -= rotl32(b, 16);
    a ^= c; a -= rotl32(c, 4);
    b ^= a; b -= rotl32(a, 14);
    c ^= b; c -= rotl32(b, 24);
    return c;
}
NOVA_INLINE float bitsTo01(uint32_t bits) { return float(bits) / float(0xffffffffu); }
NOVA_INLINE float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
constexpr uint32_t seedFor(uint32_t len) { return 0xdeadbeefu + (len << 2u) + 13u; }
NOVA_INLINE uint32_t hashInt(int x) { const uint32_t s = seedFor(1); return bjfinal(s + uint32_t(x), s, s); }
NOVA_INLINE uint32_t hashInt(int x, int y) { uint32_t a = seedFor(2), b = a, c = a; a += uint32_t(x); b += uint32_t(y); return bjfinal(a, b, c); }
NOVA_INLINE uint32_t hashInt(int x, int y, int z) { uint32_t a = seedFor(3), b = a, c = a; a += uint32_t(x); b += uint32_t(y); c += uint32_t(z); return bjfinal(a, b, c); }
NOVA_INLINE uint32_t hashInt(int x, int y, int z, int w) { uint32_t a = seedFor(4), b = a, c = a; a += uint32_t(x); b += uint32_t(y); c += uint32_t(z); bjmix(a, b, c); a += uint32_t(w); return bjfinal(a, b, c); }

NOVA_INLINE float negateIf(float v, bool b) { return b ? -v : v; }
NOVA_INLINE float grad(uint32_t hash, float x, float y) {
    const uint32_t h = hash & 7u;
    const float u = h < 4u ? x : y;
    const float v = 2.0f * (h < 4u ? y : x);
    return negateIf(u, (h & 1u) != 0) + negateIf(v, (h & 2u) != 0);
}
NOVA_INLINE float grad(uint32_t hash, float x, float y, float z) {
    const uint32_t h = hash & 15u;
    const float u = h < 8u ? x : y;
    const float v = h < 4u ? y : ((h == 12u || h == 14u) ? x : z);
    return negateIf(u, (h & 1u) != 0) + negateIf(v, (h & 2u) != 0);
}
NOVA_INLINE float bilerp(float v0, float v1, float v2, float v3, float s, float t) {
    const float s1 = 1.0f - s;
    return (1.0f - t) * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s);
}
NOVA_INLINE float trilerp(float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7, float s,
                          float t, float r) {
    const float s1 = 1.0f - s, t1 = 1.0f - t, r1 = 1.0f - r;
    return r1 * (t1 * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s)) + r * (t1 * (v4 * s1 + v5 * s) + t * (v6 * s1 + v7 * s));
}
// Signed Perlin noise, roughly [-1, 1].
NOVA_INLINE float perlin(V2 p) {
    int X, Y;
    const float fx = floorFrac(p.x, X), fy = floorFrac(p.y, Y);
    const float u = fade(fx), v = fade(fy);
    const float r = bilerp(grad(hashInt(X, Y), fx, fy), grad(hashInt(X + 1, Y), fx - 1.0f, fy),
                           grad(hashInt(X, Y + 1), fx, fy - 1.0f), grad(hashInt(X + 1, Y + 1), fx - 1.0f, fy - 1.0f), u, v);
    return 0.6616f * r;
}
NOVA_INLINE float perlin(V3 p) {
    int X, Y, Z;
    const float fx = floorFrac(p.x, X), fy = floorFrac(p.y, Y), fz = floorFrac(p.z, Z);
    const float u = fade(fx), v = fade(fy), w = fade(fz);
    const float r = trilerp(
        grad(hashInt(X, Y, Z), fx, fy, fz), grad(hashInt(X + 1, Y, Z), fx - 1.0f, fy, fz),
        grad(hashInt(X, Y + 1, Z), fx, fy - 1.0f, fz), grad(hashInt(X + 1, Y + 1, Z), fx - 1.0f, fy - 1.0f, fz),
        grad(hashInt(X, Y, Z + 1), fx, fy, fz - 1.0f), grad(hashInt(X + 1, Y, Z + 1), fx - 1.0f, fy, fz - 1.0f),
        grad(hashInt(X, Y + 1, Z + 1), fx, fy - 1.0f, fz - 1.0f),
        grad(hashInt(X + 1, Y + 1, Z + 1), fx - 1.0f, fy - 1.0f, fz - 1.0f), u, v, w);
    return 0.9820f * r;
}
// Vector Perlin: the 32-bit corner hash is split into three 8-bit hashes
// (channel x uses bits 0-7, so x equals the float noise).
NOVA_INLINE V3 perlinV3(V2 p) {
    int X, Y;
    const float fx = floorFrac(p.x, X), fy = floorFrac(p.y, Y);
    const float u = fade(fx), v = fade(fy);
    const uint32_t h00 = hashInt(X, Y), h10 = hashInt(X + 1, Y), h01 = hashInt(X, Y + 1), h11 = hashInt(X + 1, Y + 1);
    float r[3];
    for (int c = 0; c < 3; ++c) {
        const int sh = 8 * c;
        r[c] = 0.6616f * bilerp(grad((h00 >> sh) & 0xFFu, fx, fy), grad((h10 >> sh) & 0xFFu, fx - 1.0f, fy),
                                grad((h01 >> sh) & 0xFFu, fx, fy - 1.0f),
                                grad((h11 >> sh) & 0xFFu, fx - 1.0f, fy - 1.0f), u, v);
    }
    return {r[0], r[1], r[2]};
}
NOVA_INLINE V3 perlinV3(V3 p) {
    int X, Y, Z;
    const float fx = floorFrac(p.x, X), fy = floorFrac(p.y, Y), fz = floorFrac(p.z, Z);
    const float u = fade(fx), v = fade(fy), w = fade(fz);
    uint32_t h[8];
    for (int i = 0; i < 8; ++i)
        h[i] = hashInt(X + (i & 1), Y + ((i >> 1) & 1), Z + (i >> 2));
    float r[3];
    for (int c = 0; c < 3; ++c) {
        const int sh = 8 * c;
        float g[8];
        for (int i = 0; i < 8; ++i)
            g[i] = grad((h[i] >> sh) & 0xFFu, fx - float(i & 1), fy - float((i >> 1) & 1), fz - float(i >> 2));
        r[c] = 0.9820f * trilerp(g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], u, v, w);
    }
    return {r[0], r[1], r[2]};
}
// Cell noise in [0, 1].
NOVA_INLINE float cell(float p) { return bitsTo01(hashInt(mxFloor(p))); }
NOVA_INLINE float cell(V2 p) { return bitsTo01(hashInt(mxFloor(p.x), mxFloor(p.y))); }
NOVA_INLINE float cell(V3 p) { return bitsTo01(hashInt(mxFloor(p.x), mxFloor(p.y), mxFloor(p.z))); }
NOVA_INLINE V3 cellV3(V2 p) {
    const int ix = mxFloor(p.x), iy = mxFloor(p.y);
    return {bitsTo01(hashInt(ix, iy, 0)), bitsTo01(hashInt(ix, iy, 1)), bitsTo01(hashInt(ix, iy, 2))};
}
NOVA_INLINE V3 cellV3(V3 p) {
    uint32_t a = seedFor(4), b = a, c = a;
    a += uint32_t(mxFloor(p.x));
    b += uint32_t(mxFloor(p.y));
    c += uint32_t(mxFloor(p.z));
    bjmix(a, b, c);
    return {bitsTo01(bjfinal(a, b, c)), bitsTo01(bjfinal(a + 1u, b, c)), bitsTo01(bjfinal(a + 2u, b, c))};
}
// fBm: p is scaled incrementally (p *= lacunarity), amplitude *= diminish.
NOVA_INLINE float fractal(V3 p, int octaves, float lacunarity, float diminish) {
    float r = 0.0f, amp = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        r += amp * perlin(p);
        amp *= diminish;
        p = {p.x * lacunarity, p.y * lacunarity, p.z * lacunarity};
    }
    return r;
}
NOVA_INLINE V3 fractalV3(V3 p, int octaves, float lacunarity, float diminish) {
    V3 r{0, 0, 0};
    float amp = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        const V3 n = perlinV3(p);
        r = {r.x + amp * n.x, r.y + amp * n.y, r.z + amp * n.z};
        amp *= diminish;
        p = {p.x * lacunarity, p.y * lacunarity, p.z * lacunarity};
    }
    return r;
}
// Worley. metric: 0 euclidean, 1 distance^2, 2 manhattan, 3 chebyshev.
NOVA_INLINE V2 worleyCellPos2(int x, int y, int xoff, int yoff, float jitter) {
    const V3 t = cellV3(V2{float(x + xoff), float(y + yoff)});
    return {float(x) + ((t.x - 0.5f) * jitter + 0.5f), float(y) + ((t.y - 0.5f) * jitter + 0.5f)};
}
NOVA_INLINE V3 worleyCellPos3(int x, int y, int z, int xoff, int yoff, int zoff, float jitter) {
    const V3 t = cellV3(V3{float(x + xoff), float(y + yoff), float(z + zoff)});
    return {float(x) + ((t.x - 0.5f) * jitter + 0.5f), float(y) + ((t.y - 0.5f) * jitter + 0.5f),
            float(z) + ((t.z - 0.5f) * jitter + 0.5f)};
}
NOVA_INLINE float worleyMetric(float dx, float dy, float dz, int metric) {
    if (metric == 2) return std::fabs(dx) + std::fabs(dy) + std::fabs(dz);
    if (metric == 3) return std::fmax(std::fmax(std::fabs(dx), std::fabs(dy)), std::fabs(dz));
    return dx * dx + dy * dy + dz * dz;
}
// F1..F3 distances and the offset (feature point - local position) of the nearest feature point.
NOVA_INLINE void worley2(V2 p, float jitter, int metric, float d[3], V2 &minpos) {
    int X, Y;
    const V2 lp{floorFrac(p.x, X), floorFrac(p.y, Y)};
    d[0] = d[1] = d[2] = 1e6f;
    minpos = {0, 0};
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            const V2 c = worleyCellPos2(x, y, X, Y, jitter);
            const V2 cp{c.x - lp.x, c.y - lp.y};
            const float dist = worleyMetric(cp.x, cp.y, 0.0f, metric);
            if (dist < d[0]) { d[2] = d[1]; d[1] = d[0]; d[0] = dist; minpos = cp; }
            else if (dist < d[1]) { d[2] = d[1]; d[1] = dist; }
            else if (dist < d[2]) { d[2] = dist; }
        }
    if (metric == 0)
        for (int i = 0; i < 3; ++i) d[i] = std::sqrt(d[i]);
}
NOVA_INLINE void worley3(V3 p, float jitter, int metric, float d[3], V3 &minpos) {
    int X, Y, Z;
    const V3 lp{floorFrac(p.x, X), floorFrac(p.y, Y), floorFrac(p.z, Z)};
    d[0] = d[1] = d[2] = 1e6f;
    minpos = {0, 0, 0};
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z) {
                const V3 c = worleyCellPos3(x, y, z, X, Y, Z, jitter);
                const V3 cp{c.x - lp.x, c.y - lp.y, c.z - lp.z};
                const float dist = worleyMetric(cp.x, cp.y, cp.z, metric);
                if (dist < d[0]) { d[2] = d[1]; d[1] = d[0]; d[0] = dist; minpos = cp; }
                else if (dist < d[1]) { d[2] = d[1]; d[1] = dist; }
                else if (dist < d[2]) { d[2] = dist; }
            }
    if (metric == 0)
        for (int i = 0; i < 3; ++i) d[i] = std::sqrt(d[i]);
}
} // namespace mxn
