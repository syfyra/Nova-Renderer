// M8 research prototype (docs/research/m8/volume-algorithms.md): volume
// estimators on a NanoVDB grid built with the NanoVDB 32.6 headers that ship
// with Houdini 22.0.466 ($HFS/toolkit/include/nanovdb, header-only).
//
// Experiments (each prints one block; see the report for the numbers):
//   E1  transmittance: delta / ratio / residual ratio tracking, global majorant
//       vs. a DDA majorant grid with cell sizes 2..32 voxels, against exact
//       optical depth (voxel DDA + Simpson, exact for trilinear along a line);
//   E2  NanoVDB node statistics as majorants: violation rate for trilinear
//       sampling (they are not conservative), and of the dilated majorant grid;
//   E3  free-flight sampling: delta tracking CDF vs 1 - T(t) (KS statistic);
//   E4  "thermal equilibrium furnace": a heterogeneous, chromatic, emissive
//       medium (emission = sigma_a * Lenv) in a uniform environment Lenv = 1
//       must render 1 in every channel; two integrators: (A) hero channel +
//       spectral MIS (Miller et al. 2019 / pbrt-v4 VolPath) with per-channel
//       majorants, (B) one common majorant + spectral tracking (Kutz et al.
//       2017) + direction-only light/phase MIS, (B2) as B with average-based
//       event probabilities, (C) the proposal for Nova: hero free flights with
//       spectral MIS, residual-ratio-tracked NEE, direction-only MIS, (D) as
//       C but the hero and the spectral MIS per free flight (no r_u in the
//       path state; D2 picks the hero by throughput). All with NEE, HG phase
//       sampling, Russian roulette;
//   E5  single scattering in a homogeneous sphere under a distant light vs
//       1D quadrature (a reference value for a Nova unit test);
//   E6  equiangular vs distance sampling vs MIS for a point light in a
//       homogeneous medium (single scattering, quadrature reference);
//   E7  emission estimators on an emissive, absorbing grid (fire-like): real
//       collision, tentative-collision (pbrt-v4), ratio-tracking collision, and
//       jittered marching with ratio-tracked transmittance (all unbiased);
//   E8  emissive volume as a light: irradiance at a point below a fire blob,
//       unidirectional (cosine + tracking) vs NEE from an emission-power grid
//       vs MIS;
//   E9  lookup cost: trilinear vs stochastic trilinear vs nearest, and ratio
//       tracking with stochastic lookups against the exact optical depth.
//
// Build: va_build.bat (cl /O2 /std:c++20, only the NanoVDB headers).
// Run:   va_volume_lab.exe [N=160] [threads=0: hardware] [all|e1..e9] [seed=0] [sample scale=1]
// The seed varies the random streams of E3, E4, E5, E8 and E9; the scale
// multiplies the sample counts of E4, E5 and E8.

#define NOMINMAX
#define _USE_MATH_DEFINES
#include <nanovdb/NanoVDB.h>
#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/GridBuilder.h>
#include <nanovdb/util/SampleFromVoxels.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace va {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInf = std::numeric_limits<float>::infinity();

// ---------------------------------------------------------------- math ----
struct V3 {
    float x = 0, y = 0, z = 0;
    V3() = default;
    constexpr V3(float a, float b, float c) : x(a), y(b), z(c) {}
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    float &operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator*(float s, V3 a) { return a * s; }
inline V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline V3 operator/(V3 a, V3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
inline V3 &operator+=(V3 &a, V3 b) { a = a + b; return a; }
inline V3 &operator*=(V3 &a, V3 b) { a = a * b; return a; }
inline V3 &operator*=(V3 &a, float s) { a = a * s; return a; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(V3 a) { return a * (1.0f / length(a)); }
inline float maxc(V3 a) { return std::max(a.x, std::max(a.y, a.z)); }
inline float avg(V3 a) { return (a.x + a.y + a.z) * (1.0f / 3.0f); }
inline V3 vexp(V3 a) { return {std::exp(a.x), std::exp(a.y), std::exp(a.z)}; }
inline V3 vmax0(V3 a) { return {std::max(a.x, 0.0f), std::max(a.y, 0.0f), std::max(a.z, 0.0f)}; }
inline bool isZero(V3 a) { return a.x == 0 && a.y == 0 && a.z == 0; }
inline V3 splat(float v) { return {v, v, v}; }

inline void basis(V3 n, V3 &t, V3 &b) { // Duff et al. 2017
    const float s = std::copysign(1.0f, n.z);
    const float a = -1.0f / (s + n.z), c = n.x * n.y * a;
    t = {1.0f + s * n.x * n.x * a, s * c, -s * n.x};
    b = {c, s + n.y * n.y * a, -n.y};
}

// ------------------------------------------------------------------ rng ----
struct Rng { // PCG32
    uint64_t state, inc;
    explicit Rng(uint64_t seed, uint64_t seq = 1) : state(0), inc((seq << 1u) | 1u) {
        next();
        state += seed;
        next();
    }
    uint32_t next() {
        const uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const uint32_t xs = uint32_t(((old >> 18u) ^ old) >> 27u), rot = uint32_t(old >> 59u);
        return (xs >> rot) | (xs << ((~rot + 1u) & 31));
    }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }
    float expo(float rate) { return -std::log(1.0f - uni()) / rate; }
};

// ---------------------------------------------------- procedural fields ----
inline uint32_t hash3(int x, int y, int z) {
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
inline float lattice(int x, int y, int z) { return float(hash3(x, y, z) & 0xffffffu) * (2.0f / 16777215.0f) - 1.0f; }
inline float smooth(float t) { return t * t * (3.0f - 2.0f * t); }
float vnoise(float x, float y, float z) {
    const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
    const float fx = smooth(x - ix), fy = smooth(y - iy), fz = smooth(z - iz);
    auto L = [&](int a, int b, int c) { return lattice(ix + a, iy + b, iz + c); };
    const float x00 = L(0, 0, 0) + (L(1, 0, 0) - L(0, 0, 0)) * fx, x10 = L(0, 1, 0) + (L(1, 1, 0) - L(0, 1, 0)) * fx;
    const float x01 = L(0, 0, 1) + (L(1, 0, 1) - L(0, 0, 1)) * fx, x11 = L(0, 1, 1) + (L(1, 1, 1) - L(0, 1, 1)) * fx;
    const float y0 = x00 + (x10 - x00) * fy, y1 = x01 + (x11 - x01) * fy;
    return y0 + (y1 - y0) * fz;
}
float fbm(float x, float y, float z, int octaves) {
    float s = 0, a = 0.5f, f = 1.0f;
    for (int i = 0; i < octaves; ++i, a *= 0.5f, f *= 2.03f)
        s += a * vnoise(x * f + 17.1f * i, y * f, z * f - 5.3f * i);
    return s;
}
inline float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// A rising, turbulent plume in [0,1]^3: sparse (zero outside), wispy edges,
// dense cores. Smoke-like statistics: max/mean of active voxels ~ 4-6.
float smokeDensity(float x, float y, float z) {
    const float ax = 0.5f + 0.07f * std::sin(7.0f * y), az = 0.5f + 0.07f * std::cos(5.0f * y);
    const float r = std::sqrt((x - ax) * (x - ax) + (z - az) * (z - az));
    const float R = 0.08f + 0.28f * y;
    const float n1 = fbm(x * 5, y * 5, z * 5, 4);
    const float edge = 1.0f - r / (R * (1.0f + 0.7f * n1));
    if (edge <= 0.0f)
        return 0.0f;
    const float core = smoothstep(0.0f, 0.35f, edge);
    const float n2 = fbm(x * 13 + 3.1f, y * 13, z * 13 - 1.7f, 3);
    const float wisps = std::clamp(0.25f + 1.6f * (n2 + 0.15f), 0.0f, 2.5f);
    const float h = smoothstep(0.03f, 0.12f, y) * (1.0f - smoothstep(0.82f, 0.97f, y));
    const float d = core * wisps * h;
    return d < 2e-3f ? 0.0f : d;
}
// Flame intensity: the lower, inner part of the plume.
float flameField(float x, float y, float z) {
    const float d = smokeDensity(x, y, z);
    if (d <= 0.0f)
        return 0.0f;
    const float ax = 0.5f + 0.07f * std::sin(7.0f * y), az = 0.5f + 0.07f * std::cos(5.0f * y);
    const float r = std::sqrt((x - ax) * (x - ax) + (z - az) * (z - az));
    const float f = (1.0f - smoothstep(0.08f, 0.40f, y)) * (1.0f - smoothstep(0.02f, 0.10f, r)) *
                    std::clamp(0.6f + 1.2f * fbm(x * 9, y * 9 - 2.0f, z * 9, 3), 0.0f, 2.0f);
    return f < 1e-3f ? 0.0f : 6.0f * f;
}

// ----------------------------------------------------------- NanoVDB ----
using FloatGrid = nanovdb::NanoGrid<float>;
using Acc = nanovdb::DefaultReadAccessor<float>;

struct Volume {
    nanovdb::GridHandle<nanovdb::HostBuffer> handle;
    const FloatGrid *grid = nullptr;
    float h = 1.0f; // voxel size in world units
    int N = 0;
};

template <typename F>
Volume buildVolume(int N, float worldSize, F f, const char *name) {
    nanovdb::build::Grid<float> g(0.0f, name, nanovdb::GridClass::FogVolume);
    g.setTransform(double(worldSize) / N);
    g([&](const nanovdb::Coord &ijk) -> float { return f((ijk[0] + 0.5f) / N, (ijk[1] + 0.5f) / N, (ijk[2] + 0.5f) / N); },
      nanovdb::CoordBBox(nanovdb::Coord(0), nanovdb::Coord(N - 1)));
    Volume v;
    v.handle = nanovdb::createNanoGrid(g); // StatsMode::Default = All (bbox, min/max, avg, stddev per node)
    v.grid = v.handle.grid<float>();
    v.h = worldSize / N;
    v.N = N;
    return v;
}

// Trilinear lookups in index space through a per-thread accessor (NanoVDB's
// ReadAccessor caches the path to the last node: not shareable across threads).
struct Lookup {
    Acc acc;
    nanovdb::SampleFromVoxels<Acc, 1, true> smp;
    uint64_t count = 0;
    explicit Lookup(const FloatGrid *g) : acc(g->getAccessor()), smp(acc) {}
    Lookup(const Lookup &) = delete;
    float operator()(V3 p) {
        ++count;
        return std::max(0.0f, float(smp(nanovdb::Vec3f(p.x, p.y, p.z))));
    }
};

// ------------------------------------------------------- majorant grid ----
// Cell c covers index-space points [lo + c k, lo + (c+1) k). Trilinear
// sampling at x reads voxels floor(x) and floor(x)+1, so the cell's bound is
// the max over voxels [lo + c k, lo + (c+1) k] (k+1 per axis). Built by
// scattering every stored value (leaf values, active or not, and tiles) into
// the cells it can influence: O(stored values), never a dense loop.
struct MajorantGrid {
    int k = 8;
    int lo[3] = {0, 0, 0};
    int res[3] = {1, 1, 1};
    std::vector<float> maxv, minv; // minv: for residual ratio tracking (dense build, prototype only)
    size_t index(int x, int y, int z) const { return (size_t(z) * res[1] + y) * res[0] + x; }
};

struct ValueRegion { // voxels [a, b] inclusive with value v
    nanovdb::Coord a, b;
    float v;
};

template <typename F>
void forEachStoredValue(const FloatGrid *grid, F &&f) {
    const auto &tree = grid->tree();
    const auto *leaf = tree.getFirstLeaf();
    for (uint32_t i = 0; i < tree.nodeCount(0); ++i, ++leaf)
        for (uint32_t n = 0; n < 512; ++n) {
            const float v = leaf->getValue(n);
            if (v > 0.0f) {
                const nanovdb::Coord c = leaf->offsetToGlobalCoord(n);
                f(ValueRegion{c, c, v});
            }
        }
    const auto *lower = tree.getFirstLower();
    for (uint32_t i = 0; i < tree.nodeCount(1); ++i, ++lower)
        for (uint32_t n = 0; n < 4096; ++n)
            if (!lower->isChild(n)) {
                const float v = lower->data()->getValue(n);
                if (v > 0.0f) {
                    const nanovdb::Coord c = lower->offsetToGlobalCoord(n);
                    f(ValueRegion{c, c + nanovdb::Coord(7), v});
                }
            }
    const auto *upper = tree.getFirstUpper();
    for (uint32_t i = 0; i < tree.nodeCount(2); ++i, ++upper)
        for (uint32_t n = 0; n < 32768; ++n)
            if (!upper->isChild(n)) {
                const float v = upper->data()->getValue(n);
                if (v > 0.0f) {
                    const nanovdb::Coord c = upper->offsetToGlobalCoord(n);
                    f(ValueRegion{c, c + nanovdb::Coord(127), v});
                }
            }
    // (Root tiles with a non-zero value would cover 4096^3 voxels: a real
    // implementation clamps them to the grid's bbox; none here.)
}

MajorantGrid buildMajorantGrid(const Volume &vol, int k, bool withMin, Lookup *dense = nullptr) {
    int mn[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, mx[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
    forEachStoredValue(vol.grid, [&](const ValueRegion &r) {
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], r.a[a]);
            mx[a] = std::max(mx[a], r.b[a]);
        }
    });
    MajorantGrid g;
    g.k = k;
    for (int a = 0; a < 3; ++a) {
        g.lo[a] = mn[a] - 1;              // density can be > 0 on (mn - 1, mx + 1)
        const int extent = mx[a] + 1 - g.lo[a];
        g.res[a] = std::max(1, (extent + k - 1) / k);
    }
    g.maxv.assign(size_t(g.res[0]) * g.res[1] * g.res[2], 0.0f);
    forEachStoredValue(vol.grid, [&](const ValueRegion &r) {
        int c0[3], c1[3];
        for (int a = 0; a < 3; ++a) {
            // voxel v influences cells c with lo + c k <= v <= lo + (c+1) k
            const int va = r.a[a] - g.lo[a], vb = r.b[a] - g.lo[a];
            c0[a] = std::clamp(va % k == 0 ? va / k - 1 : va / k, 0, g.res[a] - 1);
            c1[a] = std::clamp(vb / k, 0, g.res[a] - 1);
        }
        for (int z = c0[2]; z <= c1[2]; ++z)
            for (int y = c0[1]; y <= c1[1]; ++y)
                for (int x = c0[0]; x <= c1[0]; ++x) {
                    float &m = g.maxv[g.index(x, y, z)];
                    m = std::max(m, r.v);
                }
    });
    if (withMin && dense) {
        g.minv.assign(g.maxv.size(), 0.0f);
        for (int z = 0; z < g.res[2]; ++z)
            for (int y = 0; y < g.res[1]; ++y)
                for (int x = 0; x < g.res[0]; ++x) {
                    float m = kInf;
                    const int x0 = g.lo[0] + x * k, y0 = g.lo[1] + y * k, z0 = g.lo[2] + z * k;
                    for (int kz = z0; kz <= z0 + k && m > 0; ++kz)
                        for (int ky = y0; ky <= y0 + k && m > 0; ++ky)
                            for (int kx = x0; kx <= x0 + k && m > 0; ++kx)
                                m = std::min(m, std::max(0.0f, dense->acc.getValue(nanovdb::Coord(kx, ky, kz))));
                    g.minv[g.index(x, y, z)] = m;
                }
    }
    return g;
}

struct Seg {
    float t0, t1, maj, min;
};

// 3D DDA over the majorant cells (Amanatides & Woo; pbrt-v4's
// DDAMajorantIterator has the same structure). Rays are in index space with
// a non-normalized direction, so t stays the world-space distance and any
// affine object-to-index map works.
struct MajIter {
    const MajorantGrid *g = nullptr;
    float tCur = 0, tEnd = 0;
    int cell[3], step[3], stop[3];
    float tNext[3], dt[3];

    bool init(const MajorantGrid &grid, V3 o, V3 d, float t0, float t1) {
        g = &grid;
        for (int a = 0; a < 3; ++a) {
            const float lo = float(grid.lo[a]), hi = float(grid.lo[a] + grid.res[a] * grid.k);
            if (d[a] == 0.0f) {
                if (o[a] < lo || o[a] >= hi)
                    return false;
                continue;
            }
            float ta = (lo - o[a]) / d[a], tb = (hi - o[a]) / d[a];
            if (ta > tb)
                std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
        }
        if (!(t0 < t1))
            return false;
        tCur = t0;
        tEnd = t1;
        for (int a = 0; a < 3; ++a) {
            const float gp = (o[a] + d[a] * t0 - float(grid.lo[a])) / float(grid.k);
            const int c = std::clamp(int(std::floor(gp)), 0, grid.res[a] - 1);
            cell[a] = c;
            if (d[a] == 0.0f) {
                tNext[a] = kInf; dt[a] = kInf; step[a] = 0; stop[a] = -2;
            } else if (d[a] > 0.0f) {
                tNext[a] = t0 + (float(c + 1) - gp) * float(grid.k) / d[a];
                dt[a] = float(grid.k) / d[a]; step[a] = 1; stop[a] = grid.res[a];
            } else {
                tNext[a] = t0 + (float(c) - gp) * float(grid.k) / d[a];
                dt[a] = -float(grid.k) / d[a]; step[a] = -1; stop[a] = -1;
            }
        }
        return true;
    }
    bool next(Seg &s) {
        if (!(tCur < tEnd))
            return false;
        const int a = tNext[0] < tNext[1] ? (tNext[0] < tNext[2] ? 0 : 2) : (tNext[1] < tNext[2] ? 1 : 2);
        const float te = std::min(tEnd, tNext[a]);
        const size_t i = g->index(cell[0], cell[1], cell[2]);
        s = {tCur, te, g->maxv[i], g->minv.empty() ? 0.0f : g->minv[i]};
        tCur = te;
        if (tNext[a] >= tEnd) {
            tCur = tEnd;
        } else {
            cell[a] += step[a];
            if (cell[a] == stop[a])
                tCur = tEnd;
            tNext[a] += dt[a];
        }
        return true;
    }
};

// Ray in world space mapped to index space (uniform scale here; the code
// only needs an affine map).
struct IRay {
    V3 o, d; // index-space origin and direction (t = world distance)
};
inline IRay toIndex(const Volume &v, V3 o, V3 d) { return {o * (1.0f / v.h), d * (1.0f / v.h)}; }
inline V3 at(const IRay &r, float t) { return r.o + r.d * t; }

// ------------------------------------------------ exact optical depth ----
// Trilinear interpolation is a cubic along a line inside one voxel cell, so
// Simpson's rule per unit-cell piece is exact.
double exactTau(const Volume &vol, const MajorantGrid &unit, Lookup &L, const IRay &r, float t0, float t1,
                double sigma) {
    MajIter it;
    if (!it.init(unit, r.o, r.d, t0, t1))
        return 0.0;
    double tau = 0.0;
    Seg s;
    while (it.next(s)) {
        if (s.maj <= 0.0f)
            continue;
        const double a = L(at(r, s.t0)), m = L(at(r, 0.5f * (s.t0 + s.t1))), b = L(at(r, s.t1));
        tau += (double(s.t1) - s.t0) / 6.0 * (a + 4.0 * m + b);
    }
    return tau * sigma;
}

// ----------------------------------------------- grey estimators (E1) ----
struct Counters {
    uint64_t lookups = 0, segs = 0, viol = 0;
};

float ratioTracking(Lookup &L, const MajorantGrid &g, const IRay &r, float t1, float sigma, Rng &rng, Counters &c) {
    MajIter it;
    if (!it.init(g, r.o, r.d, 0.0f, t1))
        return 1.0f;
    float T = 1.0f;
    Seg s;
    while (it.next(s)) {
        ++c.segs;
        const float m = s.maj * sigma;
        if (m <= 0.0f)
            continue;
        for (float t = s.t0;;) {
            t += rng.expo(m);
            if (t >= s.t1)
                break;
            const float sig = L(at(r, t)) * sigma;
            if (sig > m * 1.00001f)
                ++c.viol;
            T *= 1.0f - sig / m;
        }
    }
    return T;
}

float deltaTrackingT(Lookup &L, const MajorantGrid &g, const IRay &r, float t1, float sigma, Rng &rng, Counters &c) {
    MajIter it;
    if (!it.init(g, r.o, r.d, 0.0f, t1))
        return 1.0f;
    Seg s;
    while (it.next(s)) {
        ++c.segs;
        const float m = s.maj * sigma;
        if (m <= 0.0f)
            continue;
        for (float t = s.t0;;) {
            t += rng.expo(m);
            if (t >= s.t1)
                break;
            if (rng.uni() * m < L(at(r, t)) * sigma)
                return 0.0f;
        }
    }
    return 1.0f;
}

// Residual ratio tracking (Novak et al. 2014) with the cell minimum as the
// control: T = exp(-sum sigma_c dt) * prod (1 - (sigma - sigma_c) / (sigma_max - sigma_c)).
float residualRatioTracking(Lookup &L, const MajorantGrid &g, const IRay &r, float t1, float sigma, Rng &rng,
                            Counters &c) {
    MajIter it;
    if (!it.init(g, r.o, r.d, 0.0f, t1))
        return 1.0f;
    float tauC = 0.0f, T = 1.0f;
    Seg s;
    while (it.next(s)) {
        ++c.segs;
        const float sc = s.min * sigma, m = (s.maj - s.min) * sigma;
        tauC += sc * (s.t1 - s.t0);
        if (m <= 0.0f)
            continue;
        for (float t = s.t0;;) {
            t += rng.expo(m);
            if (t >= s.t1)
                break;
            T *= 1.0f - (L(at(r, t)) * sigma - sc) / m;
        }
    }
    return std::exp(-tauC) * T;
}

// Free-flight distance by delta tracking (kInf: none before t1).
float freeFlight(Lookup &L, const MajorantGrid &g, const IRay &r, float t1, float sigma, Rng &rng, Counters &c) {
    MajIter it;
    if (!it.init(g, r.o, r.d, 0.0f, t1))
        return kInf;
    Seg s;
    while (it.next(s)) {
        ++c.segs;
        const float m = s.maj * sigma;
        if (m <= 0.0f)
            continue;
        for (float t = s.t0;;) {
            t += rng.expo(m);
            if (t >= s.t1)
                break;
            if (rng.uni() * m < L(at(r, t)) * sigma)
                return t;
        }
    }
    return kInf;
}

// ------------------------------------------------------------- helpers ----
using Clock = std::chrono::steady_clock;
inline double seconds(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

int gThreads = 1;
uint64_t gSeed = 0; // varies the random streams of E4, E5, E8 between runs
template <typename F>
void parallelFor(int n, F &&f) { // f(begin, end, threadIndex)
    std::vector<std::thread> th;
    const int T = std::max(1, std::min(gThreads, n));
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] { f(n * t / T, n * (t + 1) / T, t); });
    for (auto &x : th)
        x.join();
}

// Rays through the smoke: origin on a sphere around the volume, aimed at a
// random point with density > 0.
struct TestRay {
    V3 o, d;
};
std::vector<TestRay> makeRays(const Volume &vol, int count, uint64_t seed, float yMax = 1.0f) {
    Rng rng(seed);
    Lookup L(vol.grid);
    std::vector<TestRay> rays;
    const float W = vol.h * vol.N;
    const V3 c{0.5f * W, 0.5f * W, 0.5f * W};
    while (int(rays.size()) < count) {
        const V3 p{rng.uni() * W, rng.uni() * W * yMax, rng.uni() * W};
        if (L(p * (1.0f / vol.h)) <= 0.05f)
            continue;
        const float z = 1.0f - 2.0f * rng.uni(), phi = 2.0f * kPi * rng.uni(), s = std::sqrt(1.0f - z * z);
        const V3 o = c + V3{s * std::cos(phi), z, s * std::sin(phi)} * (1.5f * W);
        rays.push_back({o, normalize(p - o)});
    }
    return rays;
}

// ===================================================================== E1 ==
void experimentTransmittance(const Volume &vol, const std::vector<MajorantGrid> &grids, const MajorantGrid &unit,
                             const std::vector<std::string> &names, float sigma, int rayCount, int perRay) {
    std::printf("\n== E1 transmittance estimators (sigma scale %.1f / world unit, %d rays x %d estimates) ==\n", sigma,
                rayCount, perRay);
    const std::vector<TestRay> rays = makeRays(vol, rayCount, 7);
    std::vector<double> Tref(rays.size());
    std::vector<double> taus;
    {
        Lookup L(vol.grid);
        for (size_t i = 0; i < rays.size(); ++i) {
            const IRay r = toIndex(vol, rays[i].o, rays[i].d);
            const double tau = exactTau(vol, unit, L, r, 0.0f, kInf, sigma);
            Tref[i] = std::exp(-tau);
            taus.push_back(tau);
        }
    }
    std::sort(taus.begin(), taus.end());
    std::printf("optical depth of the rays: median %.2f, p10 %.2f, p90 %.2f, max %.2f\n", taus[taus.size() / 2],
                taus[taus.size() / 10], taus[taus.size() * 9 / 10], taus.back());
    std::printf("%-26s %-8s %10s %10s %9s %9s %9s %10s %8s\n", "estimator", "majorant", "bias z", "relVar", "lookups",
                "cells", "ns/est", "1/(V*t)", "viol");
    const char *estNames[3] = {"delta tracking (0/1)", "ratio tracking", "residual ratio tracking"};
    for (int e = 0; e < 3; ++e)
        for (size_t gi = 0; gi < grids.size(); ++gi) {
            const MajorantGrid &g = grids[gi];
            if (e == 2 && g.minv.empty())
                continue;
            Counters c;
            Lookup L(vol.grid);
            double sumZ = 0.0, sumVarRef = 0.0, relVar = 0.0;
            int relN = 0;
            const auto t0 = Clock::now();
            for (size_t i = 0; i < rays.size(); ++i) {
                Rng rng(1000003ull * i + 17ull * gi + e, 3);
                const IRay r = toIndex(vol, rays[i].o, rays[i].d);
                double s = 0, s2 = 0;
                for (int j = 0; j < perRay; ++j) {
                    const float T = e == 0 ? deltaTrackingT(L, g, r, kInf, sigma, rng, c)
                                  : e == 1 ? ratioTracking(L, g, r, kInf, sigma, rng, c)
                                           : residualRatioTracking(L, g, r, kInf, sigma, rng, c);
                    s += T;
                    s2 += double(T) * T;
                }
                const double mean = s / perRay, var = std::max(0.0, s2 / perRay - mean * mean);
                sumZ += mean - Tref[i];
                sumVarRef += var / perRay;
                if (Tref[i] > 1e-2) {
                    relVar += (s2 / perRay - 2 * mean * Tref[i] + Tref[i] * Tref[i] + 0.0) / (Tref[i] * Tref[i]);
                    ++relN;
                }
            }
            const double secs = seconds(t0, Clock::now());
            const double n = double(rays.size()) * perRay;
            const double z = sumZ / std::sqrt(std::max(sumVarRef, 1e-30));
            const double rv = relVar / std::max(1, relN), ns = secs * 1e9 / n;
            std::printf("%-26s %-8s %10.2f %10.4f %9.1f %9.1f %9.0f %10.3g %8llu\n", estNames[e], names[gi].c_str(), z, rv,
                        double(L.count) / n, double(c.segs) / n, ns, 1e9 / (rv * ns), (unsigned long long)c.viol);
        }
}

// ===================================================================== E2 ==
void experimentNodeStats(const Volume &vol, const MajorantGrid &g8, int samples) {
    std::printf("\n== E2 NanoVDB node statistics as majorants (trilinear sampling) ==\n");
    Lookup L(vol.grid);
    Rng rng(99);
    const float W = float(vol.N);
    uint64_t inLeaf = 0, leafViol = 0, outside = 0, outsideViol = 0, gridViol = 0;
    double worst = 0.0;
    for (int i = 0; i < samples; ++i) {
        const V3 p{rng.uni() * W, rng.uni() * W, rng.uni() * W};
        const float v = L(p);
        const nanovdb::Coord ijk(int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z)));
        const auto info = L.acc.getNodeInfo(ijk); // level 0: leaf (its own min/max over ACTIVE values)
        if (info.level == 0) {
            ++inLeaf;
            if (v > info.maximum * 1.000001f) {
                ++leafViol;
                worst = std::max(worst, double(v) / std::max(info.maximum, 1e-6f));
            }
        } else {
            // A tile or background region. Note: getNodeInfo() here returns the
            // stats of the enclosing internal node (not the tile value), so the
            // test is against the tile value itself, which is what HDDA-style
            // empty-space skipping assumes (skip value-0 tiles).
            ++outside;
            const float tileValue = L.acc.getValue(ijk);
            if (v > tileValue * 1.000001f && v > 1e-6f)
                ++outsideViol;
        }
        // the dilated majorant grid (k = 8)
        int c[3];
        bool ok = true;
        for (int a = 0; a < 3; ++a) {
            c[a] = int(std::floor((p[a] - g8.lo[a]) / g8.k));
            ok = ok && c[a] >= 0 && c[a] < g8.res[a];
        }
        const float m = ok ? g8.maxv[g8.index(c[0], c[1], c[2])] : 0.0f;
        if (v > m * 1.000001f && v > 0.0f)
            ++gridViol;
    }
    std::printf("points in leaves: %llu, density above the leaf's max: %llu (%.2f %%), worst ratio %.2f\n",
                (unsigned long long)inLeaf, (unsigned long long)leafViol, 100.0 * leafViol / std::max<uint64_t>(1, inLeaf),
                worst);
    std::printf("points outside leaves: %llu, density above the tile value (trilinear reaches into a neighbouring leaf): %llu (%.2f %%)\n",
                (unsigned long long)outside, (unsigned long long)outsideViol,
                100.0 * outsideViol / std::max<uint64_t>(1, outside));
    std::printf("dilated majorant grid (k=8) violations: %llu of %d\n", (unsigned long long)gridViol, samples);
    const auto &tree = vol.grid->tree();
    std::printf("tree: %u leaves, %u lower, %u upper nodes, %llu active voxels, grid buffer %.1f MB\n",
                tree.nodeCount(0), tree.nodeCount(1), tree.nodeCount(2),
                (unsigned long long)vol.grid->activeVoxelCount(), vol.handle.size() / 1048576.0);
}

// ===================================================================== E3 ==
void experimentFreeFlight(const Volume &vol, const MajorantGrid &g, const MajorantGrid &unit, float sigma) {
    std::printf("\n== E3 free-flight sampling: delta tracking CDF vs 1 - T(t) ==\n");
    const std::vector<TestRay> rays = makeRays(vol, 3, 4242);
    for (size_t i = 0; i < rays.size(); ++i) {
        const IRay r = toIndex(vol, rays[i].o, rays[i].d);
        Lookup L(vol.grid);
        Counters c;
        Rng rng(31 + i + gSeed * 1000003ull);
        const int n = 200000;
        std::vector<float> ts;
        ts.reserve(n);
        int escaped = 0;
        for (int j = 0; j < n; ++j) {
            const float t = freeFlight(L, g, r, kInf, sigma, rng, c);
            if (t == kInf)
                ++escaped;
            else
                ts.push_back(t);
        }
        std::sort(ts.begin(), ts.end());
        double ks = 0.0;
        for (int q = 1; q <= 200; ++q) {
            const size_t idx = std::min(ts.size() - 1, size_t(double(q) / 201.0 * ts.size()));
            const float t = ts[idx];
            const double F = 1.0 - std::exp(-exactTau(vol, unit, L, r, 0.0f, t, sigma));
            const double Fhat = double(idx + 1) / n;
            ks = std::max(ks, std::fabs(F - Fhat));
        }
        const double Tall = std::exp(-exactTau(vol, unit, L, r, 0.0f, kInf, sigma));
        std::printf("ray %zu: T(inf) %.4f vs escaped share %.4f; KS max |F - Fhat| %.5f (1.36/sqrt(n) = %.5f)\n", i, Tall,
                    double(escaped) / n, ks, 1.36 / std::sqrt(double(n)));
    }
}

// ===================================================================== E4 ==
// Medium: sigma_t,c = d * sigma * kext_c, albedo_c, HG(g), emission sigma_a,c * Lenv.
struct Medium {
    const Volume *vol;
    const MajorantGrid *maj;
    float sigma;
    V3 kext, albedo;
    float g;
    float Lenv;
    float emit = 1.0f; // emission = emit * sigma_a * Lenv (1: thermal equilibrium, the furnace)
};

inline float hgEval(float cosT, float g) {
    const float d = 1.0f + g * g - 2.0f * g * cosT;
    return (1.0f - g * g) / (4.0f * kPi * d * std::sqrt(d));
}
// cos between the propagation directions before and after scattering.
inline V3 hgSample(V3 dir, float g, float u1, float u2, float &pdf) {
    float cosT;
    if (std::fabs(g) < 1e-3f)
        cosT = 1.0f - 2.0f * u1;
    else {
        const float s = (1.0f - g * g) / (1.0f - g + 2.0f * g * u1);
        cosT = std::clamp((1.0f + g * g - s * s) / (2.0f * g), -1.0f, 1.0f);
    }
    const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT)), phi = 2.0f * kPi * u2;
    V3 t, b;
    basis(dir, t, b);
    pdf = hgEval(cosT, g);
    return normalize(t * (sinT * std::cos(phi)) + b * (sinT * std::sin(phi)) + dir * cosT);
}
inline V3 uniformSphere(float u1, float u2) {
    const float z = 1.0f - 2.0f * u1, r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = 2.0f * kPi * u2;
    return {r * std::cos(phi), r * std::sin(phi), z};
}

struct PtStats {
    uint64_t lookups = 0, events = 0;
};

// (B) common majorant (max over channels) + spectral tracking. Shadow rays:
// ratio tracking with the common majorant (per channel exact). MIS between
// environment NEE and phase sampling on directional densities only.
// Event probabilities from the throughput-weighted coefficients: their max
// over channels (Kutz et al. 2017) or, with useAvg, their average.
// absorbAsWeight: absorption is never sampled as an event (probability 0);
// the null weight then carries it (survival biasing), so the path continues
// through absorbing material with a decaying throughput instead of ending.
V3 radianceCommon(const Medium &M, Lookup &L, V3 o, V3 d, Rng &rng, PtStats &st, int maxDepth, bool useAvg,
                  bool absorbAsWeight = false) {
    const Volume &vol = *M.vol;
    const float kmax = maxc(M.kext);
    auto prob = [useAvg](V3 v) { return useAvg ? avg(v) : maxc(v); };
    V3 Lr{0, 0, 0}, beta{1, 1, 1};
    float phasePdf = -1.0f; // < 0: camera ray (no MIS)
    const float lightPdf = 1.0f / (4.0f * kPi);
    for (int depth = 0;; ++depth) {
        const IRay r = toIndex(vol, o, d);
        MajIter it;
        bool scattered = false;
        float tS = 0;
        if (it.init(*M.maj, r.o, r.d, 0.0f, kInf)) {
            Seg s;
            while (!scattered && it.next(s)) {
                const float m = s.maj * M.sigma * kmax;
                if (m <= 0.0f)
                    continue;
                for (float t = s.t0;;) {
                    t += rng.expo(m);
                    if (t >= s.t1)
                        break;
                    ++st.events;
                    const float dens = L(at(r, t)) * M.sigma;
                    const V3 st_ = M.kext * dens, ss = st_ * M.albedo, sa = st_ - ss, sn = splat(m) - st_;
                    Lr += beta * sa * (M.emit * M.Lenv / m); // emission, collision estimator
                    const float Pa = absorbAsWeight ? 0.0f : prob(sa * beta), Ps = prob(ss * beta),
                                Pn = prob(vmax0(sn) * beta);
                    const float C = Pa + Ps + Pn;
                    if (!(C > 0.0f))
                        return Lr;
                    const float u = rng.uni() * C;
                    if (u < Pa)
                        return Lr; // absorbed
                    if (u < Pa + Ps) {
                        beta *= ss * (C / (m * Ps));
                        scattered = true;
                        tS = t;
                        break;
                    }
                    beta *= vmax0(sn) * (C / (m * Pn));
                    if (absorbAsWeight && maxc(beta) < 0.1f) { // in-flight roulette on the decaying weight
                        if (rng.uni() < 0.5f)
                            return Lr;
                        beta *= 2.0f;
                    }
                }
            }
        }
        if (!scattered) { // escaped into the environment
            const float w = phasePdf < 0.0f ? 1.0f : phasePdf * phasePdf / (phasePdf * phasePdf + lightPdf * lightPdf);
            return Lr + beta * (M.Lenv * w);
        }
        if (depth >= maxDepth)
            return Lr;
        // Russian roulette on the throughput
        if (depth >= 3) {
            const float q = std::min(0.95f, maxc(beta));
            if (rng.uni() >= q)
                return Lr;
            beta *= 1.0f / q;
        }
        const V3 p = o + d * tS;
        // NEE towards the uniform environment, ratio-tracked transmittance
        {
            const V3 wi = uniformSphere(rng.uni(), rng.uni());
            const float f = hgEval(dot(d, wi), M.g);
            const IRay sr = toIndex(vol, p, wi);
            V3 Tr{1, 1, 1};
            MajIter si;
            if (si.init(*M.maj, sr.o, sr.d, 0.0f, kInf)) {
                Seg s;
                while (si.next(s) && !isZero(Tr)) {
                    const float m = s.maj * M.sigma * kmax;
                    if (m <= 0.0f)
                        continue;
                    for (float t = s.t0;;) {
                        t += rng.expo(m);
                        if (t >= s.t1)
                            break;
                        const float dens = L(at(sr, t)) * M.sigma;
                        Tr *= vmax0(splat(1.0f) - M.kext * (dens / m));
                        // Russian roulette on small transmittance (unbiased)
                        if (maxc(Tr) < 0.1f) {
                            if (rng.uni() < 0.5f) { Tr = {0, 0, 0}; break; }
                            Tr *= 2.0f;
                        }
                    }
                }
            }
            const float w = lightPdf * lightPdf / (lightPdf * lightPdf + f * f);
            Lr += beta * Tr * (f * M.Lenv * w / lightPdf);
        }
        float pdf;
        d = hgSample(d, M.g, rng.uni(), rng.uni(), pdf);
        phasePdf = pdf; // beta *= f / pdf == 1
        o = p;
    }
}

// (A) hero channel + spectral MIS, the structure of pbrt-v4's VolPathIntegrator
// (r_u, r_l rescaled path probabilities), per-channel majorants.
V3 radianceHero(const Medium &M, Lookup &L, V3 o, V3 d, Rng &rng, PtStats &st, int maxDepth) {
    const Volume &vol = *M.vol;
    const int h = std::min(2, int(rng.uni() * 3.0f));
    V3 Lr{0, 0, 0}, beta{1, 1, 1}, r_u{1, 1, 1}, r_l{1, 1, 1};
    bool camera = true;
    const float lightPdf = 1.0f / (4.0f * kPi);
    for (int depth = 0;; ++depth) {
        const IRay r = toIndex(vol, o, d);
        MajIter it;
        bool scattered = false;
        float tS = 0;
        if (it.init(*M.maj, r.o, r.d, 0.0f, kInf)) {
            Seg s;
            while (!scattered && it.next(s)) {
                const V3 smaj = M.kext * (s.maj * M.sigma);
                if (smaj[h] <= 0.0f)
                    continue;
                for (float t = s.t0;;) {
                    const float dt = rng.expo(smaj[h]);
                    if (t + dt >= s.t1) {
                        const V3 Tm = vexp(smaj * -(s.t1 - t));
                        const float th = Tm[h];
                        beta *= Tm * (1.0f / th); r_u *= Tm * (1.0f / th); r_l *= Tm * (1.0f / th);
                        break;
                    }
                    t += dt;
                    ++st.events;
                    const V3 Tm = vexp(smaj * -dt);
                    const float dens = L(at(r, t)) * M.sigma;
                    const V3 st_ = M.kext * dens, ss = st_ * M.albedo, sa = st_ - ss;
                    { // emission at every tentative collision
                        const float pdf = smaj[h] * Tm[h];
                        const V3 betap = beta * Tm * (1.0f / pdf), r_e = r_u * smaj * Tm * (1.0f / pdf);
                        if (avg(r_e) > 0.0f)
                            Lr += betap * sa * (M.emit * M.Lenv / avg(r_e));
                    }
                    const float pA = sa[h] / smaj[h], pS = ss[h] / smaj[h];
                    const float u = rng.uni();
                    if (u < pA)
                        return Lr;
                    if (u < pA + pS) {
                        const float pdf = Tm[h] * ss[h];
                        beta *= Tm * ss * (1.0f / pdf);
                        r_u *= Tm * ss * (1.0f / pdf);
                        scattered = true;
                        tS = t;
                        break;
                    }
                    const V3 sn = vmax0(smaj - st_);
                    const float pdf = Tm[h] * sn[h];
                    if (!(pdf > 0.0f))
                        return Lr;
                    beta *= Tm * sn * (1.0f / pdf);
                    r_u *= Tm * sn * (1.0f / pdf);
                    r_l *= Tm * smaj * (1.0f / pdf);
                    if (isZero(beta) || isZero(r_u))
                        return Lr;
                }
            }
        }
        if (!scattered) {
            if (camera)
                return Lr + beta * (M.Lenv / avg(r_u));
            const V3 rl = r_l * lightPdf;
            return Lr + beta * (M.Lenv / avg(r_u + rl));
        }
        if (depth >= maxDepth)
            return Lr;
        { // Russian roulette, pbrt-v4 style
            const V3 rr = beta * (1.0f / avg(r_u));
            if (maxc(rr) < 1.0f && depth > 1) {
                const float q = std::max(0.0f, 1.0f - maxc(rr));
                if (rng.uni() < q)
                    return Lr;
                beta *= 1.0f / (1.0f - q);
            }
        }
        const V3 p = o + d * tS;
        { // NEE with ratio tracking and spectral MIS
            const V3 wi = uniformSphere(rng.uni(), rng.uni());
            const float f = hgEval(dot(d, wi), M.g);
            const IRay sr = toIndex(vol, p, wi);
            V3 Tray{1, 1, 1}, rl{1, 1, 1}, ru{1, 1, 1};
            MajIter si;
            if (si.init(*M.maj, sr.o, sr.d, 0.0f, kInf)) {
                Seg s;
                bool dead = false;
                while (!dead && si.next(s)) {
                    const V3 smaj = M.kext * (s.maj * M.sigma);
                    if (smaj[h] <= 0.0f)
                        continue;
                    for (float t = s.t0;;) {
                        const float dt = rng.expo(smaj[h]);
                        if (t + dt >= s.t1) {
                            const V3 Tm = vexp(smaj * -(s.t1 - t));
                            const float th = Tm[h];
                            Tray *= Tm * (1.0f / th); rl *= Tm * (1.0f / th); ru *= Tm * (1.0f / th);
                            break;
                        }
                        t += dt;
                        const V3 Tm = vexp(smaj * -dt);
                        const float dens = L(at(sr, t)) * M.sigma;
                        const V3 sn = vmax0(smaj - M.kext * dens);
                        const float pdf = Tm[h] * smaj[h];
                        Tray *= Tm * sn * (1.0f / pdf);
                        rl *= Tm * smaj * (1.0f / pdf);
                        ru *= Tm * sn * (1.0f / pdf);
                        const V3 Tr = Tray * (1.0f / avg(rl + ru));
                        if (maxc(Tr) < 0.05f) {
                            if (rng.uni() < 0.75f) { Tray = {0, 0, 0}; dead = true; break; }
                            Tray *= 1.0f / 0.25f;
                        }
                        if (isZero(Tray)) { dead = true; break; }
                    }
                }
            }
            if (!isZero(Tray)) {
                rl *= r_u * lightPdf;
                ru *= r_u * f; // phase pdf == phase value
                Lr += beta * Tray * (f * M.Lenv / avg(rl + ru));
            }
        }
        float pdf;
        d = hgSample(d, M.g, rng.uni(), rng.uni(), pdf);
        r_l = r_u * (1.0f / pdf);
        camera = false;
        o = p;
    }
}

// (C) the design proposed for Nova: hero-channel free flights with spectral
// MIS over the channels (r_u only), direction-only MIS between NEE and phase
// sampling, and per-channel residual ratio tracking for shadow rays (one
// common majorant over the channels, the cell minimum as control).
V3 radianceHybrid(const Medium &M, Lookup &L, V3 o, V3 d, Rng &rng, PtStats &st, int maxDepth) {
    const Volume &vol = *M.vol;
    const int h = std::min(2, int(rng.uni() * 3.0f));
    const float kmax = maxc(M.kext);
    V3 Lr{0, 0, 0}, beta{1, 1, 1}, r_u{1, 1, 1};
    float phasePdf = -1.0f;
    const float lightPdf = 1.0f / (4.0f * kPi);
    for (int depth = 0;; ++depth) {
        const IRay r = toIndex(vol, o, d);
        MajIter it;
        bool scattered = false;
        float tS = 0;
        if (it.init(*M.maj, r.o, r.d, 0.0f, kInf)) {
            Seg s;
            while (!scattered && it.next(s)) {
                const V3 smaj = M.kext * (s.maj * M.sigma);
                if (smaj[h] <= 0.0f)
                    continue;
                for (float t = s.t0;;) {
                    const float dt = rng.expo(smaj[h]);
                    if (t + dt >= s.t1) {
                        const V3 Tm = vexp(smaj * -(s.t1 - t));
                        beta *= Tm * (1.0f / Tm[h]);
                        r_u *= Tm * (1.0f / Tm[h]);
                        break;
                    }
                    t += dt;
                    ++st.events;
                    const V3 Tm = vexp(smaj * -dt);
                    const float dens = L(at(r, t)) * M.sigma;
                    const V3 st_ = M.kext * dens, ss = st_ * M.albedo, sa = st_ - ss;
                    {
                        const float pdf = smaj[h] * Tm[h];
                        const V3 betap = beta * Tm * (1.0f / pdf), r_e = r_u * smaj * Tm * (1.0f / pdf);
                        if (avg(r_e) > 0.0f)
                            Lr += betap * sa * (M.emit * M.Lenv / avg(r_e));
                    }
                    const float pA = sa[h] / smaj[h], pS = ss[h] / smaj[h];
                    const float u = rng.uni();
                    if (u < pA)
                        return Lr;
                    if (u < pA + pS) {
                        const float pdf = Tm[h] * ss[h];
                        beta *= Tm * ss * (1.0f / pdf);
                        r_u *= Tm * ss * (1.0f / pdf);
                        scattered = true;
                        tS = t;
                        break;
                    }
                    const V3 sn = vmax0(smaj - st_);
                    const float pdf = Tm[h] * sn[h];
                    if (!(pdf > 0.0f))
                        return Lr;
                    beta *= Tm * sn * (1.0f / pdf);
                    r_u *= Tm * sn * (1.0f / pdf);
                    if (isZero(beta) || isZero(r_u))
                        return Lr;
                }
            }
        }
        if (!scattered) {
            const float w = phasePdf < 0.0f ? 1.0f : phasePdf * phasePdf / (phasePdf * phasePdf + lightPdf * lightPdf);
            return Lr + beta * (M.Lenv * w / avg(r_u));
        }
        if (depth >= maxDepth)
            return Lr;
        {
            const V3 rr = beta * (1.0f / avg(r_u));
            if (maxc(rr) < 1.0f && depth > 1) {
                const float q = std::max(0.0f, 1.0f - maxc(rr));
                if (rng.uni() < q)
                    return Lr;
                beta *= 1.0f / (1.0f - q);
            }
        }
        const V3 p = o + d * tS;
        { // NEE: residual ratio tracking, common majorant, per-channel unbiased
            const V3 wi = uniformSphere(rng.uni(), rng.uni());
            const float f = hgEval(dot(d, wi), M.g);
            const IRay sr = toIndex(vol, p, wi);
            V3 Tr{1, 1, 1}, tauC{0, 0, 0};
            MajIter si;
            if (si.init(*M.maj, sr.o, sr.d, 0.0f, kInf)) {
                Seg s;
                while (si.next(s) && !isZero(Tr)) {
                    const V3 sc = M.kext * (s.min * M.sigma);
                    tauC += sc * (s.t1 - s.t0);
                    const float m = (s.maj - s.min) * M.sigma * kmax;
                    if (m <= 0.0f)
                        continue;
                    for (float t = s.t0;;) {
                        t += rng.expo(m);
                        if (t >= s.t1)
                            break;
                        const float dens = L(at(sr, t)) * M.sigma;
                        Tr *= vmax0(splat(1.0f) - (M.kext * dens - sc) * (1.0f / m));
                        if (maxc(Tr * vexp(tauC * -1.0f)) < 0.05f) {
                            if (rng.uni() < 0.75f) { Tr = {0, 0, 0}; break; }
                            Tr *= 4.0f;
                        }
                    }
                }
            }
            Tr *= vexp(tauC * -1.0f);
            const float w = lightPdf * lightPdf / (lightPdf * lightPdf + f * f);
            Lr += beta * Tr * (f * M.Lenv * w / (lightPdf * avg(r_u)));
        }
        float pdf;
        d = hgSample(d, M.g, rng.uni(), rng.uni(), pdf);
        phasePdf = pdf;
        o = p;
    }
}

// (D) the simplest structure for Nova's wavefront integrator: spectral MIS
// applied per free flight instead of per path. Each free flight picks its own
// hero channel (uniformly, or with byThroughput in proportion to the path
// throughput, as Cycles picks its distance-sampling channel), and the flight's
// weight is normalized by the mixture of the channels' pdfs right away, so the
// path keeps a plain RGB throughput: no r_u in the path state, every light
// contribution (escape, NEE, emission) is weighted as for surfaces
// (direction-only MIS), and shadow rays use residual ratio tracking with one
// common majorant (per channel exact, no hero). Unbiased: one-sample MIS over
// the hero choice of each flight, independent flights.
V3 radianceSegment(const Medium &M, Lookup &L, V3 o, V3 d, Rng &rng, PtStats &st, int maxDepth, bool byThroughput) {
    const Volume &vol = *M.vol;
    const float kmax = maxc(M.kext);
    V3 Lr{0, 0, 0}, beta{1, 1, 1};
    float phasePdf = -1.0f;
    const float lightPdf = 1.0f / (4.0f * kPi);
    for (int depth = 0;; ++depth) {
        // hero selection probabilities of this flight
        V3 P{1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};
        if (byThroughput) {
            const float s = beta.x + beta.y + beta.z;
            if (s > 0.0f)
                P = beta * (1.0f / s);
        }
        const float u0 = rng.uni();
        const int h = u0 < P.x ? 0 : (u0 < P.x + P.y ? 1 : 2);
        // mixture density relative to the hero's: sum_c P_c p_c / p_h
        auto mix = [&](V3 r) { return P.x * r.x + P.y * r.y + P.z * r.z; };
        const IRay r = toIndex(vol, o, d);
        MajIter it;
        bool scattered = false;
        float tS = 0;
        V3 bl{1, 1, 1}, ru{1, 1, 1}; // flight weight f/p_h and pdf ratios p_c/p_h
        if (it.init(*M.maj, r.o, r.d, 0.0f, kInf)) {
            Seg s;
            while (!scattered && it.next(s)) {
                const V3 smaj = M.kext * (s.maj * M.sigma);
                if (smaj[h] <= 0.0f)
                    continue;
                for (float t = s.t0;;) {
                    const float dt = rng.expo(smaj[h]);
                    if (t + dt >= s.t1) {
                        const V3 Tm = vexp(smaj * -(s.t1 - t));
                        bl *= Tm * (1.0f / Tm[h]);
                        ru *= Tm * (1.0f / Tm[h]);
                        break;
                    }
                    t += dt;
                    ++st.events;
                    const V3 Tm = vexp(smaj * -dt);
                    const float dens = L(at(r, t)) * M.sigma;
                    const V3 st_ = M.kext * dens, ss = st_ * M.albedo, sa = st_ - ss;
                    {
                        const float pdf = smaj[h] * Tm[h];
                        const V3 r_e = ru * smaj * Tm * (1.0f / pdf);
                        if (mix(r_e) > 0.0f)
                            Lr += beta * bl * Tm * sa * (M.emit * M.Lenv / (pdf * mix(r_e)));
                    }
                    const float pA = sa[h] / smaj[h], pS = ss[h] / smaj[h];
                    const float u = rng.uni();
                    if (u < pA)
                        return Lr;
                    if (u < pA + pS) {
                        const float pdf = Tm[h] * ss[h];
                        bl *= Tm * ss * (1.0f / pdf);
                        ru *= Tm * ss * (1.0f / pdf);
                        scattered = true;
                        tS = t;
                        break;
                    }
                    const V3 sn = vmax0(smaj - st_);
                    const float pdf = Tm[h] * sn[h];
                    if (!(pdf > 0.0f))
                        return Lr;
                    bl *= Tm * sn * (1.0f / pdf);
                    ru *= Tm * sn * (1.0f / pdf);
                }
            }
        }
        beta *= bl * (1.0f / mix(ru)); // the flight's MIS-weighted throughput
        if (!scattered) {
            const float w = phasePdf < 0.0f ? 1.0f : phasePdf * phasePdf / (phasePdf * phasePdf + lightPdf * lightPdf);
            return Lr + beta * (M.Lenv * w);
        }
        if (depth >= maxDepth)
            return Lr;
        if (depth >= 3) { // Russian roulette on the throughput (Nova's rule)
            const float q = std::clamp(maxc(beta), 0.05f, 0.95f);
            if (rng.uni() >= q)
                return Lr;
            beta *= 1.0f / q;
        }
        const V3 p = o + d * tS;
        { // NEE: residual ratio tracking, common majorant, per-channel unbiased
            const V3 wi = uniformSphere(rng.uni(), rng.uni());
            const float f = hgEval(dot(d, wi), M.g);
            const IRay sr = toIndex(vol, p, wi);
            V3 Tr{1, 1, 1}, tauC{0, 0, 0};
            MajIter si;
            if (si.init(*M.maj, sr.o, sr.d, 0.0f, kInf)) {
                Seg s;
                while (si.next(s) && !isZero(Tr)) {
                    const V3 sc = M.kext * (s.min * M.sigma);
                    tauC += sc * (s.t1 - s.t0);
                    const float m = (s.maj - s.min) * M.sigma * kmax;
                    if (m <= 0.0f)
                        continue;
                    for (float t = s.t0;;) {
                        t += rng.expo(m);
                        if (t >= s.t1)
                            break;
                        const float dens = L(at(sr, t)) * M.sigma;
                        Tr *= vmax0(splat(1.0f) - (M.kext * dens - sc) * (1.0f / m));
                        if (maxc(Tr * vexp(tauC * -1.0f)) < 0.05f) {
                            if (rng.uni() < 0.75f) { Tr = {0, 0, 0}; break; }
                            Tr *= 4.0f;
                        }
                    }
                }
            }
            Tr *= vexp(tauC * -1.0f);
            const float w = lightPdf * lightPdf / (lightPdf * lightPdf + f * f);
            Lr += beta * Tr * (f * M.Lenv * w / lightPdf);
        }
        float pdf;
        d = hgSample(d, M.g, rng.uni(), rng.uni(), pdf);
        phasePdf = pdf;
        o = p;
    }
}

void experimentFurnace(const Volume &vol, const MajorantGrid &maj, float sigma, V3 kext, V3 albedo, float g,
                       int rayCount, int spp, const char *label, float emit = 1.0f) {
    std::printf("\n== E4 %s: %s (sigma %.1f, kext %.2f %.2f %.2f, albedo %.2f %.2f %.2f, g %.2f) ==\n",
                emit == 1.0f ? "thermal-equilibrium furnace (expect 1)" : "no emission (no reference: estimators must agree)",
                label, sigma, kext.x, kext.y, kext.z, albedo.x, albedo.y, albedo.z, g);
    const Medium M{&vol, &maj, sigma, kext, albedo, g, 1.0f, emit};
    const std::vector<TestRay> rays = makeRays(vol, rayCount, 1234);
    const char *labels[7] = {"(A) hero + spectral MIS (pbrt-v4)", "(B) common majorant, max-prob trk", "(B2) common majorant, avg-prob trk",
                             "(C) hero + residual NEE, dir. MIS", "(D) per-flight hero, uniform", "(D2) per-flight hero, by beta",
                             "(B3) as B2, absorption as weight"};
    for (int integ = 0; integ < 7; ++integ) {
        std::vector<V3> sum(gThreads, V3{0, 0, 0}), sum2(gThreads, V3{0, 0, 0}), big(gThreads, V3{0, 0, 0});
        std::vector<PtStats> stats(gThreads);
        std::vector<uint64_t> lookups(gThreads, 0);
        const auto t0 = Clock::now();
        parallelFor(rayCount, [&](int b, int e, int ti) {
            Lookup L(vol.grid);
            for (int i = b; i < e; ++i)
                for (int j = 0; j < spp; ++j) {
                    Rng rng(uint64_t(i) * 7919ull + j + gSeed * 0x9E3779B97F4A7C15ull, 11 + integ);
                    const V3 v = integ == 0   ? radianceHero(M, L, rays[i].o, rays[i].d, rng, stats[ti], 100000)
                                 : integ == 3 ? radianceHybrid(M, L, rays[i].o, rays[i].d, rng, stats[ti], 100000)
                                 : integ == 4 || integ == 5
                                     ? radianceSegment(M, L, rays[i].o, rays[i].d, rng, stats[ti], 100000, integ == 5)
                                     : radianceCommon(M, L, rays[i].o, rays[i].d, rng, stats[ti], 100000, integ != 1,
                                                      integ == 6);
                    sum[ti] += v;
                    sum2[ti] += v * v;
                    for (int c = 0; c < 3; ++c)
                        big[ti][c] = std::max(big[ti][c], v[c]);
                }
            lookups[ti] = L.count;
        });
        const double secs = seconds(t0, Clock::now());
        V3 S{0, 0, 0}, S2{0, 0, 0}, B{0, 0, 0};
        uint64_t lk = 0;
        for (int t = 0; t < gThreads; ++t) {
            S += sum[t];
            S2 += sum2[t];
            lk += lookups[t];
            for (int c = 0; c < 3; ++c)
                B[c] = std::max(B[c], big[t][c]);
        }
        const double n = double(rayCount) * spp;
        double mean[3], se[3], var[3];
        for (int c = 0; c < 3; ++c) {
            mean[c] = S[c] / n;
            var[c] = std::max(0.0, S2[c] / n - mean[c] * mean[c]);
            se[c] = std::sqrt(var[c] / n);
        }
        std::printf("%-35s mean %.4f %.4f %.4f (+-%.4f %.4f %.4f) var %.3f %.3f %.3f max %.0f %.0f %.0f  lookups/path %.0f  %.2f us/path\n",
                    labels[integ], mean[0], mean[1], mean[2], se[0], se[1], se[2], var[0], var[1], var[2], B[0], B[1],
                    B[2], double(lk) / n, secs * 1e6 / n * gThreads);
    }
}

// ===================================================================== E5 ==
// Homogeneous sphere (radius 1 at the origin), distant light of irradiance E
// from direction l, black environment, single scattering only.
void experimentSingleScatter(int scale) {
    std::printf("\n== E5 single scattering, homogeneous sphere under a distant light ==\n");
    const float sigT = 1.5f, alb = 0.7f, g = 0.3f, E = 1.0f;
    const V3 l = normalize(V3{1.0f, 1.0f, 0.3f});
    auto exitDist = [](V3 p, V3 w) { // distance from p (inside) to the unit sphere along w
        const float b = dot(p, w), c = dot(p, p) - 1.0f;
        return -b + std::sqrt(std::max(0.0f, b * b - c));
    };
    for (float bx : {0.0f, 0.5f, 0.9f}) {
        const V3 o{bx, 0.0f, -5.0f}, d{0, 0, 1};
        const float b = dot(o, d), c = dot(o, o) - 1.0f, disc = b * b - c;
        const float t0 = -b - std::sqrt(disc), t1 = -b + std::sqrt(disc);
        // reference: midpoint quadrature with 2e5 steps (smooth integrand)
        double ref = 0.0;
        const int n = 200000;
        const double dtq = double(t1 - t0) / n;
        for (int i = 0; i < n; ++i) {
            const float t = float(t0 + (i + 0.5) * dtq);
            const V3 p = o + d * t;
            ref += dtq * std::exp(-sigT * (t - t0)) * sigT * alb * hgEval(dot(d, l), g) * E *
                   std::exp(-sigT * exitDist(p, l));
        }
        // path tracer: exact free flight, NEE to the delta light
        Rng rng(5 + int(bx * 10) + gSeed * 1000);
        double s = 0, s2 = 0;
        const int spp = (1 << 20) * scale;
        for (int i = 0; i < spp; ++i) {
            const float t = t0 + rng.expo(sigT);
            float v = 0.0f;
            if (t < t1) {
                const V3 p = o + d * t;
                v = alb * hgEval(dot(d, l), g) * E * std::exp(-sigT * exitDist(p, l));
            }
            s += v;
            s2 += double(v) * v;
        }
        const double m = s / spp, se = std::sqrt(std::max(0.0, s2 / spp - m * m) / spp);
        std::printf("impact %.1f: quadrature %.6f, path tracer %.6f +- %.6f (z %.2f)\n", bx, ref, m, se, (m - ref) / se);
    }
}

// ===================================================================== E6 ==
void experimentEquiangular() {
    std::printf("\n== E6 point light in a homogeneous medium: distance vs equiangular vs MIS (single scattering) ==\n");
    std::printf("%-8s %-6s %12s %12s %12s %12s\n", "sigma_t", "h", "ref", "relVar dist", "relVar equi", "relVar MIS");
    const float tMax = 10.0f, tL = 4.0f, I = 1.0f;
    for (float sigT : {0.01f, 0.1f, 1.0f})
        for (float hgt : {0.05f, 0.5f}) {
            auto f = [&](float t) {
                const float r2 = hgt * hgt + (t - tL) * (t - tL);
                return std::exp(-sigT * t) * sigT * (1.0f / (4.0f * kPi)) * I * std::exp(-sigT * std::sqrt(r2)) / r2;
            };
            // reference: substitution t = tL + h tan(theta) makes the integrand smooth
            const double ta = std::atan2(0.0 - tL, double(hgt)), tb = std::atan2(double(tMax) - tL, double(hgt));
            double ref = 0;
            const int n = 400000;
            for (int i = 0; i < n; ++i) {
                const double th = ta + (i + 0.5) * (tb - ta) / n;
                const double t = tL + hgt * std::tan(th);
                const double jac = hgt / (std::cos(th) * std::cos(th));
                ref += f(float(t)) * jac * (tb - ta) / n;
            }
            const float Z = 1.0f - std::exp(-sigT * tMax);
            auto pdfDist = [&](float t) { return sigT * std::exp(-sigT * t) / Z; };
            auto sampleDist = [&](float u) { return -std::log(1.0f - u * Z) / sigT; };
            auto pdfEqui = [&](float t) { return float(hgt / ((tb - ta) * (hgt * hgt + (t - tL) * (t - tL)))); };
            auto sampleEqui = [&](float u) { return float(tL + hgt * std::tan(ta + u * (tb - ta))); };
            Rng rng(77);
            const int N = 1 << 18;
            double v[3] = {0, 0, 0};
            for (int i = 0; i < N; ++i) {
                // two samples per estimate for every strategy (equal sample count)
                double e0 = 0, e1 = 0, e2 = 0;
                for (int k = 0; k < 2; ++k) {
                    const float a = sampleDist(rng.uni());
                    e0 += 0.5 * f(a) / pdfDist(a);
                    const float b = sampleEqui(rng.uni());
                    e1 += 0.5 * f(b) / pdfEqui(b);
                }
                const float a = sampleDist(rng.uni()), b = sampleEqui(rng.uni());
                const double pa0 = pdfDist(a), pa1 = pdfEqui(a), pb0 = pdfDist(b), pb1 = pdfEqui(b);
                e2 = f(a) / pa0 * (pa0 * pa0 / (pa0 * pa0 + pa1 * pa1)) + f(b) / pb1 * (pb1 * pb1 / (pb0 * pb0 + pb1 * pb1));
                v[0] += (e0 - ref) * (e0 - ref);
                v[1] += (e1 - ref) * (e1 - ref);
                v[2] += (e2 - ref) * (e2 - ref);
            }
            std::printf("%-8.2f %-6.2f %12.5g %12.4g %12.4g %12.4g\n", sigT, hgt, ref, v[0] / N / (ref * ref),
                        v[1] / N / (ref * ref), v[2] / N / (ref * ref));
        }
}

// ===================================================================== E7 ==
// Absorbing, emissive medium (no scattering): L = int T(t) sigma_a(t) Le(t) dt.
struct Emissive {
    const Volume *dens, *flame;
    const MajorantGrid *maj;
    float sigma;
};

double exactEmission(const Emissive &E, const MajorantGrid &unit, Lookup &Ld, Lookup &Lf, const IRay &r) {
    // 4-point Gauss-Legendre on 4 sub-pieces per unit cell; T at the nodes from
    // exact Simpson pieces (the extinction is cubic inside a cell).
    static const double gx[4] = {-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526};
    static const double gw[4] = {0.3478548451374538, 0.6521451548625461, 0.6521451548625461, 0.3478548451374538};
    MajIter it;
    if (!it.init(unit, r.o, r.d, 0.0f, kInf))
        return 0.0;
    double tau = 0.0, L = 0.0;
    Seg s;
    auto simpson = [&](float a, float b) {
        return (double(b) - a) / 6.0 * (Ld(at(r, a)) + 4.0 * Ld(at(r, 0.5f * (a + b))) + Ld(at(r, b))) * E.sigma;
    };
    while (it.next(s)) {
        if (s.maj <= 0.0f)
            continue;
        for (int q = 0; q < 4; ++q) {
            const float a = s.t0 + (s.t1 - s.t0) * q / 4.0f, b = s.t0 + (s.t1 - s.t0) * (q + 1) / 4.0f;
            for (int j = 0; j < 4; ++j) {
                const float t = float(0.5 * (a + b) + 0.5 * (b - a) * gx[j]);
                const double e = Ld(at(r, t)) * E.sigma * Lf(at(r, t));
                if (e > 0.0)
                    L += 0.5 * (double(b) - a) * gw[j] * e * std::exp(-(tau + simpson(a, t)));
            }
            tau += simpson(a, b);
        }
    }
    return L;
}

// mode 0: real collision only; 1: every tentative collision until absorbed
// (pbrt-v4); 2: every tentative collision weighted by the running ratio-
// tracking transmittance (never absorbed); 3: jittered marching at spacing
// `step` (world units) with a ratio-tracked transmittance.
float emissionEstimate(const Emissive &E, Lookup &Ld, Lookup &Lf, const IRay &r, int mode, float step, Rng &rng) {
    MajIter it;
    if (!it.init(*E.maj, r.o, r.d, 0.0f, kInf))
        return 0.0f;
    float L = 0.0f, T = 1.0f;
    Seg s;
    if (mode == 3) {
        // T(t) by ratio tracking, advanced lazily to each march point.
        float tNextCollision = -1.0f, segEnd = -1.0f, m = 0.0f;
        bool haveSeg = false;
        auto advanceTo = [&](float tq) { // multiply in all tentative collisions before tq
            for (;;) {
                if (!haveSeg) {
                    if (!it.next(s))
                        return;
                    haveSeg = true;
                    m = s.maj * E.sigma;
                    segEnd = s.t1;
                    tNextCollision = m > 0.0f ? s.t0 + rng.expo(m) : kInf;
                }
                if (tNextCollision < std::min(segEnd, tq)) {
                    T *= 1.0f - Ld(at(r, tNextCollision)) * E.sigma / m;
                    tNextCollision += rng.expo(m);
                    continue;
                }
                if (tq < segEnd)
                    return;
                haveSeg = false;
            }
        };
        const float tStart = it.tCur, tEnd = it.tEnd;
        for (float t = tStart + rng.uni() * step; t < tEnd; t += step) {
            advanceTo(t);
            const float fl = Lf(at(r, t));
            if (fl > 0.0f)
                L += step * T * Ld(at(r, t)) * E.sigma * fl;
            if (T < 1e-3f) { // unbiased roulette on the remaining tail
                if (rng.uni() < 0.5f)
                    break;
                T *= 2.0f;
            }
        }
        return L;
    }
    while (it.next(s)) {
        const float m = s.maj * E.sigma;
        if (m <= 0.0f)
            continue;
        for (float t = s.t0;;) {
            t += rng.expo(m);
            if (t >= s.t1)
                break;
            const float sa = Ld(at(r, t)) * E.sigma;
            if (mode == 0) {
                if (rng.uni() * m < sa)
                    return Lf(at(r, t)); // sigma_a Le / sigma_t = Le
            } else if (mode == 1) {
                if (sa > 0.0f)
                    L += sa * Lf(at(r, t)) / m;
                if (rng.uni() * m < sa)
                    return L;
            } else {
                if (sa > 0.0f)
                    L += T * sa * Lf(at(r, t)) / m;
                T *= 1.0f - sa / m;
                if (T < 1e-3f) {
                    if (rng.uni() < 0.5f)
                        return L;
                    T *= 2.0f;
                }
            }
        }
    }
    return L;
}

void experimentEmission(const Volume &dens, const Volume &flame, const MajorantGrid &maj, const MajorantGrid &unit,
                        float sigma, int rayCount, int perRay) {
    std::printf("\n== E7 emission estimators (absorbing, emissive; sigma %.1f) ==\n", sigma);
    const Emissive E{&dens, &flame, &maj, sigma};
    const std::vector<TestRay> rays = makeRays(dens, rayCount, 555, 0.4f); // through the fire region
    std::vector<double> ref(rays.size());
    {
        Lookup Ld(dens.grid), Lf(flame.grid);
        for (size_t i = 0; i < rays.size(); ++i)
            ref[i] = exactEmission(E, unit, Ld, Lf, toIndex(dens, rays[i].o, rays[i].d));
    }
    const char *names[5] = {"real collision (1 sample)", "tentative collisions", "ratio-tracked collisions",
                            "jittered march, 1 voxel", "jittered march, 4 voxels"};
    std::printf("%-28s %10s %10s %10s %10s %12s\n", "estimator", "bias z", "relVar", "lookups", "ns/est", "1/(V*t)");
    for (int mode = 0; mode < 5; ++mode) {
        Lookup Ld(dens.grid), Lf(flame.grid);
        double sumD = 0, sumV = 0, rv = 0;
        int rn = 0;
        const auto t0 = Clock::now();
        for (size_t i = 0; i < rays.size(); ++i) {
            Rng rng(91 * i + mode, 5);
            const IRay r = toIndex(dens, rays[i].o, rays[i].d);
            double s = 0, s2 = 0;
            for (int j = 0; j < perRay; ++j) {
                const float v = emissionEstimate(E, Ld, Lf, r, mode < 3 ? mode : 3, mode == 3 ? dens.h : 4.0f * dens.h, rng);
                s += v;
                s2 += double(v) * v;
            }
            const double m = s / perRay;
            sumD += m - ref[i];
            sumV += std::max(0.0, s2 / perRay - m * m) / perRay;
            if (ref[i] > 1e-3) {
                rv += (s2 / perRay - 2 * m * ref[i] + ref[i] * ref[i]) / (ref[i] * ref[i]);
                ++rn;
            }
        }
        const double secs = seconds(t0, Clock::now()), n = double(rays.size()) * perRay;
        const double relv = rv / std::max(1, rn), ns = secs * 1e9 / n;
        std::printf("%-28s %10.2f %10.4f %10.1f %10.0f %12.3g\n", names[mode], sumD / std::sqrt(std::max(sumV, 1e-30)), relv,
                    double(Ld.count + Lf.count) / n, ns, 1e9 / (relv * ns));
    }
}

// ===================================================================== E8 ==
// Irradiance at a point below the fire from the emissive volume (absorbing,
// no scattering): (U) cosine hemisphere + ratio-tracked collision emission
// along the ray; (N) NEE: a cell of the emission-power grid by power, a
// uniform point in it, ratio-tracked transmittance; (M) both, balance
// heuristic in the volume measure.
struct PowerGrid {
    MajorantGrid cells;          // geometry (lo, res, k) of the majorant grid
    std::vector<float> power;    // per cell: upper bound of int sigma_a Le dV (index units)
    std::vector<float> cdf;      // over cells
    float total = 0.0f;
};

PowerGrid buildPowerGrid(const Volume &dens, const Volume &flame, const MajorantGrid &maj, float sigma) {
    // bound per cell: maj(sigma_a) * maj(Le) * cell volume; maj(Le) from a
    // majorant grid of the flame field with the same geometry.
    PowerGrid P;
    P.cells = maj;
    MajorantGrid fm = buildMajorantGrid(flame, maj.k, false);
    // resample the flame majorant into the density grid's cells (same k; lo may differ)
    P.power.assign(maj.maxv.size(), 0.0f);
    for (int z = 0; z < maj.res[2]; ++z)
        for (int y = 0; y < maj.res[1]; ++y)
            for (int x = 0; x < maj.res[0]; ++x) {
                float fmax = 0.0f;
                const int c[3] = {x, y, z};
                int f0[3], f1[3];
                bool any = true;
                for (int a = 0; a < 3; ++a) {
                    const int lo = maj.lo[a] + c[a] * maj.k, hi = lo + maj.k;
                    f0[a] = std::max(0, int(std::floor(float(lo - fm.lo[a]) / fm.k)));
                    f1[a] = std::min(fm.res[a] - 1, int(std::floor(float(hi - 1 - fm.lo[a]) / fm.k)));
                    any = any && f0[a] <= f1[a];
                }
                if (any)
                    for (int fz = f0[2]; fz <= f1[2]; ++fz)
                        for (int fy = f0[1]; fy <= f1[1]; ++fy)
                            for (int fx = f0[0]; fx <= f1[0]; ++fx)
                                fmax = std::max(fmax, fm.maxv[fm.index(fx, fy, fz)]);
                const size_t i = maj.index(x, y, z);
                P.power[i] = maj.maxv[i] * sigma * fmax * float(maj.k * maj.k * maj.k);
            }
    P.cdf.resize(P.power.size());
    double acc = 0.0;
    for (size_t i = 0; i < P.power.size(); ++i) {
        acc += P.power[i];
        P.cdf[i] = float(acc);
    }
    P.total = float(acc);
    return P;
}

void experimentEmissionLight(const Volume &dens, const Volume &flame, const MajorantGrid &maj, float sigma, int spp) {
    std::printf("\n== E8 emissive volume as a light: irradiance at points under the fire (sigma %.1f) ==\n", sigma);
    const PowerGrid P = buildPowerGrid(dens, flame, maj, sigma);
    const float W = dens.h * dens.N;
    const Emissive E{&dens, &flame, &maj, sigma};
    std::printf("power grid: %d x %d x %d cells, %zu non-zero\n", maj.res[0], maj.res[1], maj.res[2],
                size_t(std::count_if(P.power.begin(), P.power.end(), [](float v) { return v > 0; })));
    const float h3 = dens.h * dens.h * dens.h; // index volume -> world volume
    for (float px : {0.5f, 0.2f}) {
        const V3 x{px * W, -0.1f * W, 0.5f * W}, n{0, 1, 0};
        double res[3][2] = {};
        double times[3] = {};
        for (int strat = 0; strat < 3; ++strat) {
            Lookup Ld(dens.grid), Lf(flame.grid);
            Rng rng(17 + strat + gSeed * 100);
            double s = 0, s2 = 0;
            const auto t0 = Clock::now();
            for (int i = 0; i < spp; ++i) {
                double v = 0.0;
                // probability densities in world volume measure for MIS:
                // NEE: P(cell) / cell volume; unidirectional ray + collision
                // estimator: cos/pi / r^2 * sigma_maj (tentative collision rate).
                if (strat != 1) { // unidirectional
                    const float u1 = rng.uni(), u2 = rng.uni();
                    const float r = std::sqrt(u1), phi = 2 * kPi * u2;
                    const V3 wi{r * std::cos(phi), std::sqrt(std::max(0.0f, 1 - u1)), r * std::sin(phi)};
                    const IRay ir = toIndex(dens, x, wi);
                    MajIter it;
                    float T = 1.0f;
                    if (it.init(maj, ir.o, ir.d, 0.0f, kInf)) {
                        Seg sg;
                        while (it.next(sg) && T > 0.0f) {
                            const float m = sg.maj * sigma;
                            if (m <= 0.0f)
                                continue;
                            for (float t = sg.t0;;) {
                                t += rng.expo(m);
                                if (t >= sg.t1)
                                    break;
                                const V3 pi = at(ir, t);
                                const float sa = Ld(pi) * sigma;
                                const float fl = sa > 0.0f ? Lf(pi) : 0.0f;
                                if (fl > 0.0f) {
                                    float w = 1.0f;
                                    if (strat == 2) {
                                        const float cosT = wi.y, pU = cosT / kPi / (t * t) * m;
                                        int c[3];
                                        for (int a = 0; a < 3; ++a)
                                            c[a] = std::clamp(int(std::floor((pi[a] - maj.lo[a]) / maj.k)), 0, maj.res[a] - 1);
                                        const float pN = P.power[maj.index(c[0], c[1], c[2])] / P.total /
                                                         (float(maj.k * maj.k * maj.k) * h3);
                                        w = pU / (pU + pN);
                                    }
                                    // irradiance: L cos / pdf_dir, pdf_dir = cos/pi -> pi * L
                                    v += kPi * T * sa * fl / m * w;
                                }
                                T *= 1.0f - sa / m;
                            }
                        }
                    }
                }
                if (strat != 0) { // NEE from the power grid
                    const float u = rng.uni() * P.total;
                    const size_t ci = std::lower_bound(P.cdf.begin(), P.cdf.end(), u) - P.cdf.begin();
                    if (ci < P.power.size() && P.power[ci] > 0.0f) {
                        const int cx = int(ci % maj.res[0]), cy = int((ci / maj.res[0]) % maj.res[1]),
                                  cz = int(ci / (size_t(maj.res[0]) * maj.res[1]));
                        const V3 pi{float(maj.lo[0] + (cx + rng.uni()) * maj.k), float(maj.lo[1] + (cy + rng.uni()) * maj.k),
                                    float(maj.lo[2] + (cz + rng.uni()) * maj.k)};
                        const float sa = Ld(pi) * sigma;
                        const float fl = sa > 0.0f ? Lf(pi) : 0.0f;
                        const V3 pw = pi * dens.h;
                        const V3 dv = pw - x;
                        const float dist = length(dv);
                        const V3 wi = dv * (1.0f / dist);
                        const float cosT = dot(wi, n);
                        if (fl > 0.0f && cosT > 0.0f) {
                            const float pN = P.power[ci] / P.total / (float(maj.k * maj.k * maj.k) * h3);
                            // transmittance x -> p by ratio tracking
                            const IRay ir = toIndex(dens, x, wi);
                            MajIter it;
                            float T = 1.0f;
                            if (it.init(maj, ir.o, ir.d, 0.0f, dist)) {
                                Seg sg;
                                while (it.next(sg) && T > 0.0f) {
                                    const float m = sg.maj * sigma;
                                    if (m <= 0.0f)
                                        continue;
                                    for (float t = sg.t0;;) {
                                        t += rng.expo(m);
                                        if (t >= sg.t1)
                                            break;
                                        T *= 1.0f - Ld(at(ir, t)) * sigma / m;
                                    }
                                }
                            }
                            float w = 1.0f;
                            int c[3] = {cx, cy, cz};
                            if (strat == 2) {
                                const float pU = cosT / kPi / (dist * dist) * maj.maxv[maj.index(c[0], c[1], c[2])] * sigma;
                                w = pN / (pN + pU);
                            }
                            v += T * sa * fl * cosT / (dist * dist) / pN * w;
                        }
                    }
                }
                s += v;
                s2 += v * v;
            }
            times[strat] = seconds(t0, Clock::now());
            res[strat][0] = s / spp;
            res[strat][1] = std::max(0.0, s2 / spp - res[strat][0] * res[strat][0]);
        }
        std::printf("point x=%.1fW: E unidirectional %.5f (var %.4g, %.2f us), NEE %.5f (var %.4g, %.2f us), MIS %.5f (var %.4g, %.2f us)\n",
                    px, res[0][0], res[0][1], times[0] * 1e6 / spp, res[1][0], res[1][1], times[1] * 1e6 / spp, res[2][0],
                    res[2][1], times[2] * 1e6 / spp);
        for (int s = 0; s < 3; ++s)
            std::printf("    efficiency 1/(var*t) %s: %.3g\n", s == 0 ? "unidirectional" : s == 1 ? "NEE" : "MIS",
                        1.0 / (res[s][1] * times[s] / spp));
    }
}

// ===================================================================== E9 ==
// Lookup cost: trilinear (8 voxels through the accessor) vs stochastic
// trilinear (one voxel picked with the trilinear weights: an unbiased estimate
// of the trilinear value, the "stochastic interpolation" Cycles 5.0 uses) vs
// nearest; and ratio tracking with stochastic lookups against the exact
// optical depth (unbiased because the estimators are linear in sigma and every
// voxel value is below the dilated majorant).
struct StochasticLookup {
    Acc acc;
    uint64_t count = 0;
    explicit StochasticLookup(const FloatGrid *g) : acc(g->getAccessor()) {}
    float operator()(V3 p, Rng &rng) {
        ++count;
        const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
        const int x = int(fx) + (rng.uni() < p.x - fx), y = int(fy) + (rng.uni() < p.y - fy),
                  z = int(fz) + (rng.uni() < p.z - fz);
        return std::max(0.0f, acc.getValue(nanovdb::Coord(x, y, z)));
    }
};

void experimentLookups(const Volume &vol, const MajorantGrid &g8, const MajorantGrid &unit, float sigma) {
    std::printf("\n== E9 lookup cost and stochastic trilinear lookups ==\n");
    const std::vector<TestRay> rays = makeRays(vol, 600, 9);
    // points inside the active region, along the test rays
    std::vector<V3> pts;
    {
        Rng rng(5);
        Lookup L(vol.grid);
        while (pts.size() < (1u << 20)) {
            const TestRay &tr = rays[std::min(rays.size() - 1, size_t(rng.uni() * rays.size()))];
            const IRay r = toIndex(vol, tr.o, tr.d);
            const V3 p = at(r, 3.0f * rng.uni() * vol.h * vol.N); // the whole span through the box
            if (L(p) > 0.0f)
                pts.push_back(p);
        }
    }
    double sink = 0.0;
    for (int rep = 0; rep < 2; ++rep) { // second pass is the measured one (warm caches)
        Lookup L(vol.grid);
        StochasticLookup S(vol.grid);
        Rng rng(77);
        Acc acc = vol.grid->getAccessor();
        nanovdb::SampleFromVoxels<Acc, 0, true> nearest(acc);
        auto t0 = Clock::now();
        for (const V3 &p : pts)
            sink += L(p);
        auto t1 = Clock::now();
        for (const V3 &p : pts)
            sink += S(p, rng);
        auto t2 = Clock::now();
        for (const V3 &p : pts)
            sink += float(nearest(nanovdb::Vec3f(p.x, p.y, p.z)));
        auto t3 = Clock::now();
        if (rep == 1)
            std::printf("ns per lookup (random points along rays, 1 thread): trilinear %.1f, stochastic trilinear %.1f, "
                        "nearest %.1f\n",
                        seconds(t0, t1) * 1e9 / pts.size(), seconds(t1, t2) * 1e9 / pts.size(),
                        seconds(t2, t3) * 1e9 / pts.size());
    }
    // ratio tracking with stochastic vs trilinear lookups on the same rays
    std::vector<double> Tref(rays.size());
    {
        Lookup Lx(vol.grid);
        for (size_t i = 0; i < rays.size(); ++i)
            Tref[i] = std::exp(-exactTau(vol, unit, Lx, toIndex(vol, rays[i].o, rays[i].d), 0.0f, kInf, sigma));
    }
    for (int stochastic = 1; stochastic >= 0; --stochastic) {
        double sumZ = 0.0, sumV = 0.0, rv = 0.0;
        int rn = 0;
        StochasticLookup S(vol.grid);
        Lookup Lt(vol.grid);
        const int per = 64;
        const auto t0 = Clock::now();
        for (size_t i = 0; i < rays.size(); ++i) {
            const IRay r = toIndex(vol, rays[i].o, rays[i].d);
            Rng rng(4242 + i + gSeed * 1000003ull + 7777ull * stochastic);
            double s = 0, s2 = 0;
            for (int j = 0; j < per; ++j) {
                MajIter it;
                float T = 1.0f;
                if (it.init(g8, r.o, r.d, 0.0f, kInf)) {
                    Seg sg;
                    while (it.next(sg)) {
                        const float m = sg.maj * sigma;
                        if (m <= 0.0f)
                            continue;
                        for (float t = sg.t0;;) {
                            t += rng.expo(m);
                            if (t >= sg.t1)
                                break;
                            const float dens = stochastic ? S(at(r, t), rng) : Lt(at(r, t));
                            T *= 1.0f - dens * sigma / m;
                        }
                    }
                }
                s += T;
                s2 += double(T) * T;
            }
            const double mean = s / per;
            sumZ += mean - Tref[i];
            sumV += std::max(0.0, s2 / per - mean * mean) / per;
            if (Tref[i] > 1e-2) {
                rv += (s2 / per - 2 * mean * Tref[i] + Tref[i] * Tref[i]) / (Tref[i] * Tref[i]);
                ++rn;
            }
        }
        const double ns = seconds(t0, Clock::now()) * 1e9 / (double(rays.size()) * per);
        std::printf("ratio tracking (k=8, sigma %.0f), %-20s bias z %6.2f, relVar %.4f, %.0f ns/estimate\n", sigma,
                    stochastic ? "stochastic lookups:" : "trilinear lookups:", sumZ / std::sqrt(std::max(sumV, 1e-30)),
                    rv / std::max(1, rn), ns);
    }
    if (sink == 42.0)
        std::printf(" ");
}

} // namespace va

int main(int argc, char **argv) {
    using namespace va;
    const int N = argc > 1 ? std::atoi(argv[1]) : 160;
    gThreads = argc > 2 ? std::atoi(argv[2]) : 0;
    if (gThreads <= 0)
        gThreads = int(std::max(1u, std::thread::hardware_concurrency()));
    const float worldSize = 2.0f;
    std::printf("NanoVDB %d.%d.%d headers; grid %d^3, world size %.1f, %d threads\n", NANOVDB_MAJOR_VERSION_NUMBER,
                NANOVDB_MINOR_VERSION_NUMBER, NANOVDB_PATCH_VERSION_NUMBER, N, worldSize, gThreads);
    auto tb = Clock::now();
    Volume dens = buildVolume(N, worldSize, smokeDensity, "density");
    Volume flame = buildVolume(N, worldSize, flameField, "flame");
    std::printf("built NanoVDB grids in %.2f s: density %llu active voxels (%.1f %% of the box), %.1f MB; flame %llu, %.1f MB\n",
                seconds(tb, Clock::now()), (unsigned long long)dens.grid->activeVoxelCount(),
                100.0 * dens.grid->activeVoxelCount() / (double(N) * N * N), dens.handle.size() / 1048576.0,
                (unsigned long long)flame.grid->activeVoxelCount(), flame.handle.size() / 1048576.0);
    {
        const auto &root = dens.grid->tree().root();
        std::printf("density stats from the root: min %.3f max %.3f mean(active) %.3f\n", root.minimum(), root.maximum(),
                    root.average());
    }
    Lookup dense(dens.grid);
    std::vector<MajorantGrid> grids;
    std::vector<std::string> names;
    tb = Clock::now();
    grids.push_back(buildMajorantGrid(dens, 4096, true, &dense)); // one cell: the global majorant, clipped to the bbox
    names.push_back("global");
    for (int k : {32, 16, 8, 4, 2}) {
        const auto t0 = Clock::now();
        grids.push_back(buildMajorantGrid(dens, k, false));
        const double tMax = seconds(t0, Clock::now());
        // min grid for residual tracking (dense, prototype only)
        MajorantGrid withMin = buildMajorantGrid(dens, k, true, &dense);
        grids.back().minv = withMin.minv;
        names.push_back("k=" + std::to_string(k));
        std::printf("majorant grid k=%2d: %d x %d x %d cells, %.2f MB, build (sparse scatter) %.1f ms\n", k,
                    grids.back().res[0], grids.back().res[1], grids.back().res[2],
                    grids.back().maxv.size() * 4.0 / 1048576.0, tMax * 1e3);
    }
    MajorantGrid unit = buildMajorantGrid(dens, 1, false);
    const MajorantGrid &g8 = grids[3];

    const float sigma = 8.0f; // extinction per world unit at density 1
    const std::string mode = argc > 3 ? argv[3] : "all";
    gSeed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 0;
    const int scale = argc > 5 ? std::atoi(argv[5]) : 1; // sample multiplier for E4/E5/E8
    const bool all = mode == "all";
    if (all || mode == "e1") {
        experimentTransmittance(dens, grids, unit, names, sigma, 1500, 64);
        experimentTransmittance(dens, grids, unit, names, 32.0f, 600, 64);
    }
    if (all || mode == "e2")
        experimentNodeStats(dens, g8, 2000000);
    if (all || mode == "e3")
        experimentFreeFlight(dens, g8, unit, sigma);
    if (all || mode == "e4") {
        experimentFurnace(dens, g8, sigma, V3{1, 1, 1}, V3{0.9f, 0.9f, 0.9f}, 0.5f, 400, 64 * scale, "grey");
        experimentFurnace(dens, g8, sigma, V3{1.0f, 0.45f, 0.15f}, V3{0.95f, 0.7f, 0.99f}, 0.5f, 400, 64 * scale, "chromatic");
        experimentFurnace(dens, g8, sigma, V3{1, 1, 1}, V3{0.9f, 0.9f, 0.9f}, 0.5f, 400, 64 * scale, "grey", 0.0f);
        experimentFurnace(dens, g8, 32.0f, V3{1.0f, 0.45f, 0.15f}, V3{0.99f, 0.99f, 0.99f}, 0.7f, 200, 32 * scale,
                          "dense chromatic, albedo .99", 0.0f);
        experimentFurnace(dens, g8, 32.0f, V3{1.0f, 0.45f, 0.15f}, V3{0.99f, 0.99f, 0.99f}, 0.7f, 200, 32 * scale,
                          "dense chromatic, albedo .99");
    }
    if (all || mode == "e5")
        experimentSingleScatter(scale);
    if (all || mode == "e6")
        experimentEquiangular();
    if (all || mode == "e7")
        experimentEmission(dens, flame, g8, unit, sigma, 600, 64);
    if (all || mode == "e8")
        experimentEmissionLight(dens, flame, g8, sigma, (1 << 18) * scale);
    if (all || mode == "e9")
        experimentLookups(dens, g8, unit, sigma);
    return 0;
}
