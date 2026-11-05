#pragma once

// MaterialX noise and color functions (docs/design/M3.md M3.7), ported from
// MaterialX 1.39.5 stdlib/genglsl/lib/mx_noise.glsl and mx_hsv.glsl (Apache
// 2.0; the noise is OSL's, oslnoise.h). Integer hashing is bit-exact; Perlin
// and fBm match Houdini's mx_perlin within 1 ulp, cell noise and Worley
// exactly (docs/research/m3/mtlx-nodes.md 0.3, measured with vexexec), which
// is what Karma renders.

#include "core/math/vec.h"

#include <cmath>
#include <cstdint>

namespace nova::shading::noise {

NOVA_INLINE int floorInt(float x) { return int(std::floor(x)); }
NOVA_INLINE float floorFrac(float x, int &i) {
    i = floorInt(x);
    return x - float(i);
}
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
NOVA_INLINE uint32_t seedFor(uint32_t len) { return 0xdeadbeefu + (len << 2u) + 13u; }
NOVA_INLINE uint32_t hashInt(int x, int y) {
    uint32_t a = seedFor(2), b = a, c = a;
    a += uint32_t(x);
    b += uint32_t(y);
    return bjfinal(a, b, c);
}
NOVA_INLINE uint32_t hashInt(int x, int y, int z) {
    uint32_t a = seedFor(3), b = a, c = a;
    a += uint32_t(x);
    b += uint32_t(y);
    c += uint32_t(z);
    return bjfinal(a, b, c);
}

// Four-int hashes (MaterialX mx_hash_int(x, y, z, w)).
NOVA_INLINE uint32_t hashInt(int x, int y, int z, int w) {
    uint32_t a = seedFor(4), b = a, c = a;
    a += uint32_t(x);
    b += uint32_t(y);
    c += uint32_t(z);
    bjmix(a, b, c);
    a += uint32_t(w);
    return bjfinal(a, b, c);
}

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
NOVA_INLINE float perlin(Vec2f p) {
    int X, Y;
    const float fx = floorFrac(p.x, X), fy = floorFrac(p.y, Y);
    const float u = fade(fx), v = fade(fy);
    const float r = bilerp(grad(hashInt(X, Y), fx, fy), grad(hashInt(X + 1, Y), fx - 1.0f, fy),
                           grad(hashInt(X, Y + 1), fx, fy - 1.0f), grad(hashInt(X + 1, Y + 1), fx - 1.0f, fy - 1.0f), u, v);
    return 0.6616f * r;
}
NOVA_INLINE float perlin(Vec3f p) {
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
// Vector Perlin: the corner hash split into three 8-bit hashes (channel x
// uses bits 0-7, so x equals the float noise).
NOVA_INLINE Vec3f perlinV3(Vec2f p) {
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
NOVA_INLINE Vec3f perlinV3(Vec3f p) {
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
NOVA_INLINE float cell(Vec2f p) { return bitsTo01(hashInt(floorInt(p.x), floorInt(p.y))); }
NOVA_INLINE float cell(Vec3f p) { return bitsTo01(hashInt(floorInt(p.x), floorInt(p.y), floorInt(p.z))); }
NOVA_INLINE Vec3f cellV3(Vec2f p) {
    const int ix = floorInt(p.x), iy = floorInt(p.y);
    return {bitsTo01(hashInt(ix, iy, 0)), bitsTo01(hashInt(ix, iy, 1)), bitsTo01(hashInt(ix, iy, 2))};
}
NOVA_INLINE Vec3f cellV3(Vec3f p) {
    uint32_t a = seedFor(4), b = a, c = a;
    a += uint32_t(floorInt(p.x));
    b += uint32_t(floorInt(p.y));
    c += uint32_t(floorInt(p.z));
    bjmix(a, b, c);
    return {bitsTo01(bjfinal(a, b, c)), bitsTo01(bjfinal(a + 1u, b, c)), bitsTo01(bjfinal(a + 2u, b, c))};
}

// 4D cell noise (MaterialX mx_cell_noise_float(vec4), mx_cell_noise_vec3(vec4)).
NOVA_INLINE float cell(Vec3f p, float w) {
    return bitsTo01(hashInt(floorInt(p.x), floorInt(p.y), floorInt(p.z), floorInt(w)));
}
NOVA_INLINE Vec3f cellV3(Vec3f p, float w) {
    uint32_t a = seedFor(5), b = a, c = a;
    a += uint32_t(floorInt(p.x));
    b += uint32_t(floorInt(p.y));
    c += uint32_t(floorInt(p.z));
    bjmix(a, b, c);
    a += uint32_t(floorInt(w));
    return {bitsTo01(bjfinal(a, b, c)), bitsTo01(bjfinal(a, b + 1u, c)), bitsTo01(bjfinal(a, b + 2u, c))};
}

// MaterialX flakes (mx_flake.glsl, Arvo's random rotations): the flake at
// position p (in flake units, position / size) with the coverage density
// mapped to a probability. Returns its priority (0: none) and the cell's
// normal noise (xi0, xi1) and random value.
NOVA_INLINE void flake(Vec3f P, float coverage, float out[4]) {
    const float x = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage), xx = x * x;
    const float probability = (-26.19771808f * xx + 26.39663835f * x) /
                              (85.53857017f * xx * x + -102.35069432f * xx + -101.42634862f * x + 118.45082288f);
    const float d = 1.5f / std::sqrt(3.0f);
    const Vec3f base{std::floor(P.x), std::floor(P.y), std::floor(P.z)};
    float priorityMax = 0.0f;
    Vec3f flakeCell{0.0f, 0.0f, 0.0f};
    for (int i = -1; i < 2; ++i)
        for (int j = -1; j < 2; ++j)
            for (int k = -1; k < 2; ++k) {
                const Vec3f cellPos = base + Vec3f{float(i), float(j), float(k)};
                Vec3f q = P - cellPos - Vec3f{0.5f, 0.5f, 0.5f};
                if (dot(q, q) >= d * d * 3.0f)
                    continue;
                if (cell(cellPos) > probability)
                    continue;
                const float priority = cell(cellPos, 3.0f);
                if (priority < priorityMax)
                    continue;
                // Arvo's rotation (the GLSL matrix is column-major: m * q).
                const Vec3f r = cellV3(cellPos);
                const float theta = 3.14159265358979323846f * 2.0f * r.x, phi = 3.14159265358979323846f * 2.0f * r.y;
                const float z = r.z * 2.0f, rr = std::sqrt(z);
                const float vx = std::sin(phi) * rr, vy = std::cos(phi) * rr, vz = std::sqrt(2.0f - z);
                const float st = std::sin(theta), ct = std::cos(theta);
                const float sx = vx * ct - vy * st, sy = vx * st + vy * ct;
                const Vec3f c0{vx * sx - ct, vx * sy - st, vx * vz}, c1{vy * sx + st, vy * sy - ct, vy * vz},
                    c2{vz * sx, vz * sy, 1.0f - z};
                q = c0 * q.x + c1 * q.y + c2 * q.z;
                if (std::fabs(q.x) <= d && std::fabs(q.y) <= d && std::fabs(q.z) <= d) {
                    priorityMax = priority;
                    flakeCell = cellPos;
                }
            }
    if (priorityMax <= 0.0f) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    const Vec3f n = cellV3(flakeCell, 2.0f);
    out[0] = priorityMax;
    out[1] = n.x;
    out[2] = n.y;
    out[3] = n.z;
}

// fBm: p scaled incrementally by lacunarity, amplitude by diminish.
NOVA_INLINE float fractal(Vec2f p, int octaves, float lacunarity, float diminish) {
    float r = 0.0f, amp = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        r += amp * perlin(p);
        amp *= diminish;
        p = p * lacunarity;
    }
    return r;
}
NOVA_INLINE float fractal(Vec3f p, int octaves, float lacunarity, float diminish) {
    float r = 0.0f, amp = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        r += amp * perlin(p);
        amp *= diminish;
        p = p * lacunarity;
    }
    return r;
}
template <class V> NOVA_INLINE Vec3f fractalV3(V p, int octaves, float lacunarity, float diminish) {
    Vec3f r{0.0f, 0.0f, 0.0f};
    float amp = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        r += perlinV3(p) * amp;
        amp *= diminish;
        p = p * lacunarity;
    }
    return r;
}

// Worley (metric 0: euclidean, the nodes' choice): F1..F3 and the offset of
// the nearest feature point. style 1 ("solid") is the cell noise there.
NOVA_INLINE void worley(Vec2f p, float jitter, float d[3], Vec2f &minpos) {
    int X, Y;
    const Vec2f local{floorFrac(p.x, X), floorFrac(p.y, Y)};
    d[0] = d[1] = d[2] = 1e6f;
    minpos = {0.0f, 0.0f};
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            const Vec3f t = cellV3(Vec2f{float(x + X), float(y + Y)});
            const Vec2f c{float(x) + ((t.x - 0.5f) * jitter + 0.5f), float(y) + ((t.y - 0.5f) * jitter + 0.5f)};
            const Vec2f diff = c - local;
            const float dist = dot(diff, diff);
            if (dist < d[0]) {
                d[2] = d[1];
                d[1] = d[0];
                d[0] = dist;
                minpos = diff;
            } else if (dist < d[1]) {
                d[2] = d[1];
                d[1] = dist;
            } else if (dist < d[2]) {
                d[2] = dist;
            }
        }
}
NOVA_INLINE void worley(Vec3f p, float jitter, float d[3], Vec3f &minpos) {
    int X, Y, Z;
    const Vec3f local{floorFrac(p.x, X), floorFrac(p.y, Y), floorFrac(p.z, Z)};
    d[0] = d[1] = d[2] = 1e6f;
    minpos = {0.0f, 0.0f, 0.0f};
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z) {
                const Vec3f t = cellV3(Vec3f{float(x + X), float(y + Y), float(z + Z)});
                const Vec3f c{float(x) + ((t.x - 0.5f) * jitter + 0.5f), float(y) + ((t.y - 0.5f) * jitter + 0.5f),
                              float(z) + ((t.z - 0.5f) * jitter + 0.5f)};
                const Vec3f diff = c - local;
                const float dist = dot(diff, diff);
                if (dist < d[0]) {
                    d[2] = d[1];
                    d[1] = d[0];
                    d[0] = dist;
                    minpos = diff;
                } else if (dist < d[1]) {
                    d[2] = d[1];
                    d[1] = dist;
                } else if (dist < d[2]) {
                    d[2] = dist;
                }
            }
}

// Voronoi (Karma's kma_voronoinoise, VEX vnoise's features; the seed points
// are MaterialX's jittered cell points, so the pattern is Nova's own): the
// three nearest seed points in the metric (0 Euclidean, 1 squared Euclidean,
// 2 Manhattan, 3 Chebyshev), their distances F1 <= F2 <= F3 and positions.
// `dim` 2 searches the plane z = 0.
NOVA_INLINE void voronoi(Vec3f p, float jitter, int metric, int dim, float d[3], Vec3f pts[3]) {
    int X, Y, Z = 0;
    const Vec3f local{floorFrac(p.x, X), floorFrac(p.y, Y), dim == 3 ? floorFrac(p.z, Z) : 0.0f};
    d[0] = d[1] = d[2] = 1e30f;
    pts[0] = pts[1] = pts[2] = p;
    const int zr = dim == 3 ? 1 : 0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -zr; z <= zr; ++z) {
                const Vec3f cellPos{float(x + X), float(y + Y), float(z + Z)};
                const Vec3f t = dim == 3 ? cellV3(cellPos) : cellV3(Vec2f{cellPos.x, cellPos.y});
                const Vec3f c{float(x) + ((t.x - 0.5f) * jitter + 0.5f), float(y) + ((t.y - 0.5f) * jitter + 0.5f),
                              dim == 3 ? float(z) + ((t.z - 0.5f) * jitter + 0.5f) : 0.0f};
                const Vec3f diff = c - local;
                const Vec3f a{std::fabs(diff.x), std::fabs(diff.y), std::fabs(diff.z)};
                const float dist = metric == 2   ? a.x + a.y + a.z
                                   : metric == 3 ? (a.x > a.y ? (a.x > a.z ? a.x : a.z) : (a.y > a.z ? a.y : a.z))
                                   : metric == 1 ? dot(diff, diff)
                                                 : std::sqrt(dot(diff, diff));
                const Vec3f world{float(X) + c.x, float(Y) + c.y, dim == 3 ? float(Z) + c.z : 0.0f};
                if (dist < d[0]) {
                    d[2] = d[1];
                    pts[2] = pts[1];
                    d[1] = d[0];
                    pts[1] = pts[0];
                    d[0] = dist;
                    pts[0] = world;
                } else if (dist < d[1]) {
                    d[2] = d[1];
                    pts[2] = pts[1];
                    d[1] = dist;
                    pts[1] = world;
                } else if (dist < d[2]) {
                    d[2] = dist;
                    pts[2] = world;
                }
            }
}

// Foley & van Dam (MaterialX mx_hsv.glsl).
NOVA_INLINE Vec3f hsvToRgb(Vec3f hsv) {
    float h = hsv.x;
    const float s = hsv.y, v = hsv.z;
    if (s < 0.0001f)
        return {v, v, v};
    h = 6.0f * (h - std::floor(h));
    const int hi = int(std::trunc(h));
    const float f = h - float(hi);
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (hi) {
    case 0:
        return {v, t, p};
    case 1:
        return {q, v, p};
    case 2:
        return {p, v, t};
    case 3:
        return {p, q, v};
    case 4:
        return {t, p, v};
    default:
        return {v, p, q};
    }
}
NOVA_INLINE Vec3f rgbToHsv(Vec3f c) {
    const float mincomp = nova::fmin(c.x, nova::fmin(c.y, c.z)), maxcomp = nova::fmax(c.x, nova::fmax(c.y, c.z));
    const float delta = maxcomp - mincomp;
    const float v = maxcomp;
    const float s = maxcomp > 0.0f ? delta / maxcomp : 0.0f;
    float h = 0.0f;
    if (s > 0.0f) {
        if (c.x >= maxcomp)
            h = (c.y - c.z) / delta;
        else if (c.y >= maxcomp)
            h = 2.0f + (c.z - c.x) / delta;
        else
            h = 4.0f + (c.x - c.y) / delta;
        h *= 1.0f / 6.0f;
        if (h < 0.0f)
            h += 1.0f;
    }
    return {h, s, v};
}

} // namespace nova::shading::noise
