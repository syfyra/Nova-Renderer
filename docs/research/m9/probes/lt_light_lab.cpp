// M9 research prototype (docs/research/m9/lights.md, section 2): light selection for
// many lights. Stand-alone, no dependencies; build with lt_build.bat.
//
// Compares, at equal numbers of light samples, the selection strategies
//   uniform   every light with probability 1/N,
//   power     Nova's current alias table: probability proportional to power,
//   bvh       a light BVH after pbrt-v4 (LightBounds with orientation cones,
//             SAOH build with 12 buckets, one light per leaf, stochastic
//             traversal by importance, PMF by bit trail),
//   split     the BVH with adaptive splitting (an approximation of Conty &
//             Kulla 2018: both children are visited while a node subtends more
//             than a threshold angle, up to a sample budget),
// on scenes of 1k-100k small lights, without occlusion (the variance of light
// selection alone). Experiments:
//   E1 variance and cost per scene and N; E2 PMF consistency (sum of PMF over
//   all lights, frequency test); E3 build, refit and rebuild after edits;
//   E4 BSDF rays finding the lights they cross (brute force vs a ray BVH vs
//   the selection BVH);
//   E5 a distant light next to many local lights (probability of the
//   infinite part); E6 float vs double random number reuse in the traversal;
//   E7 splitting criteria and a bias check over seeds with a finer reference;
//   E8 the receiver term (one-sided vs |cos|) for receivers with random normals.
// Usage: lt_light_lab.exe [e1|e2|e3|e4|e5|e6|e7|e8|all] [threads]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(V3 a) { return a * (1.0f / length(a)); }
inline V3 vmin(V3 a, V3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline V3 vmax(V3 a, V3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline float comp(V3 a, int i) { return i == 0 ? a.x : i == 1 ? a.y : a.z; }
inline float safeSqrt(float x) { return std::sqrt(std::max(0.0f, x)); }
inline float safeAcos(float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); }

struct Rng { // PCG32
    uint64_t s, inc;
    explicit Rng(uint64_t seed, uint64_t seq = 1) : s(0), inc((seq << 1u) | 1u) {
        next();
        s += seed;
        next();
    }
    uint32_t next() {
        uint64_t old = s;
        s = old * 6364136223846793005ULL + inc;
        uint32_t x = uint32_t(((old >> 18u) ^ old) >> 27u), r = uint32_t(old >> 59u);
        return (x >> r) | (x << ((32 - r) & 31));
    }
    float uf() { return float(next() >> 8) * (1.0f / 16777216.0f); }
    double ud() { return (double(next() >> 5) * 67108864.0 + double(next() >> 6)) * (1.0 / 9007199254740992.0); }
};

void frame(V3 n, V3 &t, V3 &b) {
    float s = n.z >= 0 ? 1.0f : -1.0f, a = -1.0f / (s + n.z), c = n.x * n.y * a;
    t = {1 + s * n.x * n.x * a, s * c, -s * n.x};
    b = {c, s + n.y * n.y * a, -n.y};
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void parallelFor(int n, int threads, const std::function<void(int)> &f) {
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int i; (i = next++) < n;)
                f(i);
        });
    for (auto &th : pool)
        th.join();
}

// ---------------------------------------------------------------------------
// Lights: spheres (emit everywhere) and one-sided triangles.
struct Light {
    int type = 0; // 0 sphere, 1 triangle
    V3 p;         // sphere centre / triangle v0
    float r = 0;  // sphere radius
    V3 e1, e2, n; // triangle edges and unit normal (emitting side)
    float area = 0;
    float L = 1;  // radiance
    float power() const { return kPi * L * area; }
};

Light sphereLight(V3 c, float r, float L) {
    Light l;
    l.type = 0;
    l.p = c;
    l.r = r;
    l.area = 4 * kPi * r * r;
    l.L = L;
    return l;
}

Light triLight(V3 v0, V3 v1, V3 v2, float L) {
    Light l;
    l.type = 1;
    l.p = v0;
    l.e1 = v1 - v0;
    l.e2 = v2 - v0;
    V3 c = cross(l.e1, l.e2);
    l.area = 0.5f * length(c);
    l.n = normalize(c);
    l.L = L;
    return l;
}

// One light sample of the irradiance E = int L cos+ dw at (x, n): f / pdf.
float sampleLightIrr(const Light &l, V3 x, V3 n, float u1, float u2) {
    if (l.type == 0) {
        V3 d = l.p - x;
        float d2 = dot(d, d), r2 = l.r * l.r;
        if (d2 <= r2)
            return 0;
        float sin2 = r2 / d2, omc = sin2 / (1 + safeSqrt(1 - sin2)); // 1 - cos(theta_max)
        float cosT = 1 - u1 * omc, sinT = safeSqrt(1 - cosT * cosT), phi = 2 * kPi * u2;
        V3 w = d * (1.0f / std::sqrt(d2)), t, b;
        frame(w, t, b);
        V3 wi = t * (sinT * std::cos(phi)) + b * (sinT * std::sin(phi)) + w * cosT;
        float c = dot(n, wi);
        return c > 0 ? l.L * c * (2 * kPi * omc) : 0;
    }
    float su = std::sqrt(u1);
    V3 q = l.p + l.e1 * (su * (1 - u2)) + l.e2 * (su * u2);
    V3 d = q - x;
    float d2 = dot(d, d);
    V3 wi = d * (1.0f / std::sqrt(d2));
    float cl = -dot(wi, l.n), c = dot(n, wi);
    if (cl <= 0 || c <= 0)
        return 0;
    return l.L * c * cl * l.area / d2;
}

// ---------------------------------------------------------------------------
// Light bounds (pbrt-v4): box, power, emission cone (w, theta_o) and falloff theta_e.
struct LB {
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    float phi = 0;
    V3 w{0, 0, 1};
    float cosO = 1, cosE = 1;
    bool empty() const { return phi == 0 && lo.x > hi.x; }
};

LB boundsOf(const Light &l) {
    LB b;
    if (l.type == 0) {
        V3 r{l.r, l.r, l.r};
        b.lo = l.p - r;
        b.hi = l.p + r;
        b.w = {0, 0, 1};
        b.cosO = -1; // all directions
        b.cosE = 0;  // pi/2
    } else {
        V3 v1 = l.p + l.e1, v2 = l.p + l.e2;
        b.lo = vmin(l.p, vmin(v1, v2));
        b.hi = vmax(l.p, vmax(v1, v2));
        b.w = l.n;
        b.cosO = 1;
        b.cosE = 0;
    }
    b.phi = l.power();
    return b;
}

V3 rotate(V3 v, V3 axis, float angle) { // Rodrigues
    axis = normalize(axis);
    float c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1 - c));
}

// Union of two direction cones (pbrt-v4 DirectionCone Union).
void coneUnion(V3 wa, float cosa, V3 wb, float cosb, V3 &w, float &cosOut) {
    float ta = safeAcos(cosa), tb = safeAcos(cosb), td = safeAcos(dot(wa, wb));
    if (std::min(td + tb, kPi) <= ta) {
        w = wa;
        cosOut = cosa;
        return;
    }
    if (std::min(td + ta, kPi) <= tb) {
        w = wb;
        cosOut = cosb;
        return;
    }
    float to = 0.5f * (ta + td + tb);
    if (to >= kPi) {
        w = wa;
        cosOut = -1;
        return;
    }
    float tr = to - ta;
    V3 wr = cross(wa, wb);
    if (dot(wr, wr) < 1e-12f) {
        w = wa;
        cosOut = -1;
        return;
    }
    w = normalize(rotate(wa, wr, tr));
    cosOut = std::cos(to);
}

LB unite(const LB &a, const LB &b) {
    if (a.phi == 0)
        return b;
    if (b.phi == 0)
        return a;
    LB u;
    u.lo = vmin(a.lo, b.lo);
    u.hi = vmax(a.hi, b.hi);
    u.phi = a.phi + b.phi;
    coneUnion(a.w, a.cosO, b.w, b.cosO, u.w, u.cosO);
    u.cosE = std::min(a.cosE, b.cosE);
    return u;
}

bool g_oneSided = true;

// pbrt-v4 LightBounds::Importance (including its clamp of d^2 against half the
// box diagonal, unsquared).
float importance(const LB &b, V3 p, V3 n) {
    V3 pc = (b.lo + b.hi) * 0.5f;
    V3 diag = b.hi - b.lo;
    float d2 = dot(p - pc, p - pc);
    d2 = std::max(d2, length(diag) * 0.5f);
    float cosO = b.cosO, sinO = safeSqrt(1 - cosO * cosO);
    V3 wi = normalize(p - pc);
    float cosW = dot(b.w, wi), sinW = safeSqrt(1 - cosW * cosW);
    // Directions to the box from p (bounding sphere; everything if inside).
    float cosB;
    if (p.x >= b.lo.x && p.y >= b.lo.y && p.z >= b.lo.z && p.x <= b.hi.x && p.y <= b.hi.y && p.z <= b.hi.z)
        cosB = -1;
    else {
        float r2 = dot(diag, diag) * 0.25f, dd = dot(p - pc, p - pc);
        cosB = dd < r2 ? -1 : safeSqrt(1 - r2 / dd);
    }
    float sinB = safeSqrt(1 - cosB * cosB);
    auto cosSub = [](float sa, float ca, float sb, float cb) { return ca > cb ? 1.0f : ca * cb + sa * sb; };
    auto sinSub = [](float sa, float ca, float sb, float cb) { return ca > cb ? 0.0f : sa * cb - ca * sb; };
    float cosX = cosSub(sinW, cosW, sinO, cosO), sinX = sinSub(sinW, cosW, sinO, cosO);
    float cosP = cosSub(sinX, cosX, sinB, cosB);
    if (cosP <= b.cosE)
        return 0;
    float imp = b.phi * cosP / d2;
    // Receiver term: wi points from the light to p, so the incident direction is -wi.
    // pbrt-v4 uses |cos| (two-sided receivers); g_oneSided bounds the angle to the
    // upper hemisphere instead (Cycles does so unless the BSDF transmits).
    float cosI = std::fabs(dot(wi, n)), sinI = safeSqrt(1 - cosI * cosI);
    if (g_oneSided && dot(-wi, n) <= 0) {
        // The cluster's centre is below the horizon: only the angular margin of the box helps.
        float c = cosSub(sinI, -cosI, sinB, cosB);
        imp *= std::max(0.0f, c);
    } else {
        imp *= cosSub(sinI, cosI, sinB, cosB);
    }
    return std::max(imp, 0.0f);
}

// ---------------------------------------------------------------------------
struct Node {
    LB b;
    uint32_t child = 0; // second child (interior) or light index (leaf)
    uint32_t parent = 0xffffffffu;
    bool leaf = false;
};

float evalCost(const LB &b, const LB &all, int dim) {
    float tO = safeAcos(b.cosO), tE = safeAcos(b.cosE);
    float tW = std::min(tO + tE, kPi), sinO = std::sin(tO);
    float mO = 2 * kPi * (1 - b.cosO) + kPi / 2 * (2 * tW * sinO - std::cos(tO - 2 * tW) - 2 * tO * sinO + b.cosO);
    V3 d = all.hi - all.lo;
    float kr = std::max(d.x, std::max(d.y, d.z)) / std::max(comp(d, dim), 1e-20f);
    V3 e = b.hi - b.lo;
    float area = 2 * (e.x * e.y + e.y * e.z + e.z * e.x);
    return b.phi * mO * kr * area;
}

struct LightBVH {
    std::vector<Node> nodes;
    std::vector<uint64_t> trail;   // per light
    std::vector<uint32_t> leafOf;  // per light: its leaf node
    const std::vector<Light> *lights = nullptr;

    void build(const std::vector<Light> &ls) {
        lights = &ls;
        nodes.clear();
        nodes.reserve(2 * ls.size());
        trail.assign(ls.size(), 0);
        leafOf.assign(ls.size(), 0);
        std::vector<std::pair<uint32_t, LB>> items(ls.size());
        for (uint32_t i = 0; i < ls.size(); ++i)
            items[i] = {i, boundsOf(ls[i])};
        if (!items.empty())
            rec(items, 0, int(items.size()), 0, 0, 0xffffffffu);
    }

    uint32_t rec(std::vector<std::pair<uint32_t, LB>> &it, int begin, int end, uint64_t bits, int depth,
                 uint32_t parent) {
        uint32_t idx = uint32_t(nodes.size());
        nodes.emplace_back();
        nodes[idx].parent = parent;
        if (end - begin == 1) {
            nodes[idx].b = it[begin].second;
            nodes[idx].leaf = true;
            nodes[idx].child = it[begin].first;
            trail[it[begin].first] = bits;
            leafOf[it[begin].first] = idx;
            return idx;
        }
        LB all, cb;
        V3 clo{1e30f, 1e30f, 1e30f}, chi{-1e30f, -1e30f, -1e30f};
        for (int i = begin; i < end; ++i) {
            all = unite(all, it[i].second);
            V3 c = (it[i].second.lo + it[i].second.hi) * 0.5f;
            clo = vmin(clo, c);
            chi = vmax(chi, c);
        }
        float best = 1e38f;
        int bestDim = -1, bestBucket = -1;
        constexpr int kB = 12;
        for (int dim = 0; dim < 3; ++dim) {
            float lo = comp(clo, dim), hi = comp(chi, dim);
            if (hi == lo)
                continue;
            LB bk[kB];
            for (int i = begin; i < end; ++i) {
                float c = comp((it[i].second.lo + it[i].second.hi) * 0.5f, dim);
                int b = std::min(kB - 1, int(kB * (c - lo) / (hi - lo)));
                bk[b] = unite(bk[b], it[i].second);
            }
            for (int s = 0; s < kB - 1; ++s) {
                LB a, c;
                for (int i = 0; i <= s; ++i)
                    a = unite(a, bk[i]);
                for (int i = s + 1; i < kB; ++i)
                    c = unite(c, bk[i]);
                if (a.phi == 0 || c.phi == 0)
                    continue;
                float cost = evalCost(a, all, dim) + evalCost(c, all, dim);
                if (cost > 0 && cost < best) {
                    best = cost;
                    bestDim = dim;
                    bestBucket = s;
                }
            }
        }
        int mid;
        if (bestDim < 0) {
            mid = (begin + end) / 2;
        } else {
            float lo = comp(clo, bestDim), hi = comp(chi, bestDim);
            auto pm = std::partition(it.begin() + begin, it.begin() + end, [&](const std::pair<uint32_t, LB> &x) {
                float c = comp((x.second.lo + x.second.hi) * 0.5f, bestDim);
                return std::min(kB - 1, int(kB * (c - lo) / (hi - lo))) <= bestBucket;
            });
            mid = int(pm - it.begin());
            if (mid == begin || mid == end)
                mid = (begin + end) / 2;
        }
        rec(it, begin, mid, bits, depth + 1, idx);
        uint32_t second = rec(it, mid, end, bits | (uint64_t(1) << depth), depth + 1, idx);
        nodes[idx].b = all;
        nodes[idx].child = second;
        return idx;
    }

    // Bottom-up refit after light edits (nodes are in depth-first order:
    // children have larger indices than their parent).
    void refit() {
        for (size_t k = nodes.size(); k-- > 0;) {
            Node &n = nodes[k];
            if (n.leaf)
                n.b = boundsOf((*lights)[n.child]);
            else
                n.b = unite(nodes[k + 1].b, nodes[n.child].b);
        }
    }

    template <typename U>
    int sample(V3 p, V3 n, U u, float &pmf, uint32_t start = 0) const {
        uint32_t k = start;
        pmf = 1;
        while (!nodes[k].leaf) {
            float c0 = importance(nodes[k + 1].b, p, n), c1 = importance(nodes[nodes[k].child].b, p, n);
            if (c0 <= 0 && c1 <= 0)
                return -1;
            U p0 = U(c0) / (U(c0) + U(c1));
            if (u < p0) {
                u = std::min(u / p0, U(1) - std::numeric_limits<U>::epsilon());
                pmf *= float(p0);
                k = k + 1;
            } else {
                u = std::min((u - p0) / (U(1) - p0), U(1) - std::numeric_limits<U>::epsilon());
                pmf *= float(U(1) - p0);
                k = nodes[k].child;
            }
        }
        return int(nodes[k].child);
    }

    float pmf(V3 p, V3 n, uint32_t light) const {
        uint64_t bits = trail[light];
        uint32_t k = 0;
        float pm = 1;
        while (!nodes[k].leaf) {
            float c0 = importance(nodes[k + 1].b, p, n), c1 = importance(nodes[nodes[k].child].b, p, n);
            float s = c0 + c1;
            if (!(s > 0))
                return 0;
            if (bits & 1) {
                pm *= c1 / s;
                k = nodes[k].child;
            } else {
                pm *= c0 / s;
                k = k + 1;
            }
            bits >>= 1;
        }
        return pm;
    }
};

// Walker alias table (Nova's LightSampler).
struct Alias {
    std::vector<float> pmf, thr;
    std::vector<uint32_t> alias;
    void build(const std::vector<float> &w) {
        size_t n = w.size();
        double tot = 0;
        for (float x : w)
            tot += x;
        pmf.resize(n);
        thr.assign(n, 1);
        alias.resize(n);
        std::vector<double> sc(n);
        std::vector<uint32_t> small, large;
        for (size_t i = 0; i < n; ++i) {
            pmf[i] = float(w[i] / tot);
            sc[i] = w[i] / tot * double(n);
            (sc[i] < 1 ? small : large).push_back(uint32_t(i));
            alias[i] = uint32_t(i);
        }
        while (!small.empty() && !large.empty()) {
            uint32_t s = small.back(), l = large.back();
            small.pop_back();
            thr[s] = float(sc[s]);
            alias[s] = l;
            sc[l] -= 1 - sc[s];
            if (sc[l] < 1) {
                large.pop_back();
                small.push_back(l);
            }
        }
    }
    uint32_t sample(float u, float &p) const {
        float s = u * float(pmf.size());
        uint32_t c = std::min(uint32_t(s), uint32_t(pmf.size() - 1));
        uint32_t i = (s - float(c)) < thr[c] ? c : alias[c];
        p = pmf[i];
        return i;
    }
};

// ---------------------------------------------------------------------------
// Scenes and shading points.
struct Scene {
    std::string name;
    std::vector<Light> lights;
    std::vector<V3> P, N; // shading points
};

float logUniform(Rng &r, float lo, float hi) { return lo * std::pow(hi / lo, r.uf()); }

V3 randomDir(Rng &r) {
    float z = 1 - 2 * r.uf(), ph = 2 * kPi * r.uf(), s = safeSqrt(1 - z * z);
    return {s * std::cos(ph), s * std::sin(ph), z};
}

Scene makeScene(int kind, int n, int m, uint64_t seed) {
    Rng r(seed, 7);
    Scene s;
    if (kind == 0) { // "street": spheres 0.5-1.5 above a ground of constant light density
        float W = 2.0f * std::sqrt(float(n));
        s.name = "street";
        for (int i = 0; i < n; ++i)
            s.lights.push_back(sphereLight({W * r.uf(), 0.5f + r.uf(), W * r.uf()}, 0.05f, logUniform(r, 10, 100)));
        for (int i = 0; i < m; ++i) {
            s.P.push_back({W * (0.1f + 0.8f * r.uf()), 0, W * (0.1f + 0.8f * r.uf())});
            s.N.push_back({0, 1, 0});
        }
    } else if (kind == 1) { // "cloud": spheres in a cube, random receivers inside
        float W = 2.0f * std::cbrt(float(n));
        s.name = "cloud";
        for (int i = 0; i < n; ++i)
            s.lights.push_back(sphereLight({W * r.uf(), W * r.uf(), W * r.uf()}, 0.05f, logUniform(r, 10, 100)));
        for (int i = 0; i < m; ++i) {
            V3 p;
            bool ok;
            do {
                p = {W * (0.1f + 0.8f * r.uf()), W * (0.1f + 0.8f * r.uf()), W * (0.1f + 0.8f * r.uf())};
                ok = true;
                for (const Light &l : s.lights)
                    if (dot(l.p - p, l.p - p) < 0.04f) {
                        ok = false;
                        break;
                    }
            } while (!ok);
            s.P.push_back(p);
            s.N.push_back(randomDir(r));
        }
    } else { // "tris": one-sided emissive triangles (a mesh emitter), random orientation, above ground
        float W = 2.0f * std::sqrt(float(n));
        s.name = "tris";
        for (int i = 0; i < n; ++i) {
            V3 c{W * r.uf(), 0.5f + r.uf(), W * r.uf()};
            V3 nn = randomDir(r), t, b;
            frame(nn, t, b);
            float a = 0.15f;
            s.lights.push_back(triLight(c + t * a, c + b * a - t * (0.5f * a), c - b * a - t * (0.5f * a),
                                        logUniform(r, 10, 100)));
        }
        for (int i = 0; i < m; ++i) {
            s.P.push_back({W * (0.1f + 0.8f * r.uf()), 0, W * (0.1f + 0.8f * r.uf())});
            s.N.push_back({0, 1, 0});
        }
    }
    return s;
}

// Exact (up to tiny per-light noise) irradiance: every light, `spl` samples each.
std::vector<double> reference(const Scene &s, int threads, int spl = 16) {
    std::vector<double> ref(s.P.size());
    parallelFor(int(s.P.size()), threads, [&](int i) {
        Rng r(1000 + i, 3);
        double sum = 0;
        for (const Light &l : s.lights) {
            double e = 0;
            for (int k = 0; k < spl; ++k)
                e += sampleLightIrr(l, s.P[i], s.N[i], r.uf(), r.uf());
            sum += e / spl;
        }
        ref[i] = sum;
    });
    return ref;
}

// Splitting: visit both children while the node subtends more than `tau`
// (sin of the half-angle of its bounding sphere) and the budget allows.
float estimateSplit(const LightBVH &bvh, const std::vector<Light> &ls, uint32_t k, V3 p, V3 n, Rng &r, float tau,
                    int &budget, int &used) {
    const Node &nd = bvh.nodes[k];
    if (!nd.leaf && budget > 1) {
        V3 pc = (nd.b.lo + nd.b.hi) * 0.5f, d = nd.b.hi - nd.b.lo;
        float rr = 0.5f * length(d), dist = length(p - pc);
        if (rr > tau * dist) {
            --budget; // one more sample
            return estimateSplit(bvh, ls, k + 1, p, n, r, tau, budget, used) +
                   estimateSplit(bvh, ls, nd.child, p, n, r, tau, budget, used);
        }
    }
    float pm;
    int li = bvh.sample(p, n, r.uf(), pm, k);
    ++used;
    if (li < 0)
        return 0;
    return sampleLightIrr(ls[li], p, n, r.uf(), r.uf()) / pm;
}

struct Stats {
    double relVar = 0, z = 0, nsPerEst = 0, samplesPerEst = 1;
};

uint64_t g_seed = 0; // varies the estimators' random streams (E7)

// K estimates per point; relative MSE averaged over points; bias z over all.
template <typename F>
Stats measure(const Scene &s, const std::vector<double> &ref, int K, int threads, F est) {
    std::vector<double> rv(s.P.size()), dm(s.P.size()), dv(s.P.size()), used(s.P.size());
    parallelFor(int(s.P.size()), threads, [&](int i) {
        Rng r(77 + 1315423911ull * i + 7919ull * g_seed, 11 + g_seed);
        double sum = 0, sum2 = 0;
        int u = 0;
        for (int k = 0; k < K; ++k) {
            int uk = 1;
            double e = est(i, r, uk);
            u += uk;
            sum += e;
            sum2 += e * e;
        }
        double mean = sum / K, var = std::max(0.0, sum2 / K - mean * mean);
        rv[i] = ref[i] > 0 ? (var + (mean - ref[i]) * (mean - ref[i])) / (ref[i] * ref[i]) : 0;
        dm[i] = mean - ref[i];
        dv[i] = var / K;
        used[i] = double(u) / K;
    });
    Stats st;
    double sdm = 0, sdv = 0;
    for (size_t i = 0; i < s.P.size(); ++i) {
        st.relVar += rv[i];
        sdm += dm[i];
        sdv += dv[i];
        st.samplesPerEst += used[i];
    }
    st.relVar /= double(s.P.size());
    st.samplesPerEst = (st.samplesPerEst - 1) / double(s.P.size());
    st.z = sdv > 0 ? sdm / std::sqrt(sdv) : 0;
    // Timing: one thread, a subset of points.
    int tp = std::min<int>(64, int(s.P.size())), tk = 2048;
    volatile double sink = 0;
    double t0 = now();
    for (int i = 0; i < tp; ++i) {
        Rng r(5 + i, 13);
        for (int k = 0; k < tk; ++k) {
            int uk = 1;
            sink = sink + est(i, r, uk);
        }
    }
    st.nsPerEst = (now() - t0) / (double(tp) * tk) * 1e9;
    return st;
}

int g_threads = 8;

void e1() {
    std::printf("\n== E1: relative variance (MSE / E^2, mean over 256 receivers) at one light sample per estimate\n");
    std::printf("   (split: samples per estimate in brackets; eff = 1 / (relVar x samples), normalised to power)\n");
    for (int kind = 0; kind < 3; ++kind) {
        for (int n : {1000, 10000, 100000}) {
            Scene s = makeScene(kind, n, 256, 42 + kind);
            double t0 = now();
            std::vector<double> ref = reference(s, g_threads);
            double tref = now() - t0;
            std::vector<float> w(n);
            for (int i = 0; i < n; ++i)
                w[i] = s.lights[i].power();
            Alias al;
            t0 = now();
            al.build(w);
            double tal = now() - t0;
            LightBVH bvh;
            t0 = now();
            bvh.build(s.lights);
            double tbvh = now() - t0;
            const int K = 4096;
            Stats su = measure(s, ref, K, g_threads, [&](int i, Rng &r, int &) {
                int li = std::min(int(r.uf() * n), n - 1);
                return double(sampleLightIrr(s.lights[li], s.P[i], s.N[i], r.uf(), r.uf())) * n;
            });
            Stats sp = measure(s, ref, K, g_threads, [&](int i, Rng &r, int &) {
                float pm;
                uint32_t li = al.sample(r.uf(), pm);
                return double(sampleLightIrr(s.lights[li], s.P[i], s.N[i], r.uf(), r.uf())) / pm;
            });
            Stats sb = measure(s, ref, K, g_threads, [&](int i, Rng &r, int &) {
                float pm;
                int li = bvh.sample(s.P[i], s.N[i], r.uf(), pm);
                return li < 0 ? 0.0 : double(sampleLightIrr(s.lights[li], s.P[i], s.N[i], r.uf(), r.uf())) / pm;
            });
            Stats ss[2];
            const float taus[2] = {0.5f, 0.25f};
            for (int t = 0; t < 2; ++t)
                ss[t] = measure(s, ref, K, g_threads, [&](int i, Rng &r, int &used) {
                    int budget = 8;
                    used = 0;
                    return double(estimateSplit(bvh, s.lights, 0, s.P[i], s.N[i], r, taus[t], budget, used));
                });
            // PMF evaluation cost (as for MIS of a light hit by a BSDF ray).
            volatile float sink = 0;
            t0 = now();
            Rng rr(9, 9);
            const int np = 200000;
            for (int k = 0; k < np; ++k) {
                int i = k & 255;
                sink = sink + bvh.pmf(s.P[i], s.N[i], uint32_t(rr.next() % n));
            }
            double nsPmf = (now() - t0) / np * 1e9;
            int depth = 0;
            for (int i = 0; i < n; ++i) {
                int d = 0;
                for (uint32_t k = bvh.leafOf[i]; bvh.nodes[k].parent != 0xffffffffu; k = bvh.nodes[k].parent)
                    ++d;
                depth = std::max(depth, d);
            }
            auto eff = [&](const Stats &x) { return (sp.relVar * 1.0) / (x.relVar * x.samplesPerEst); };
            std::printf("   %-6s N=%6d  ref %.2fs | build alias %.2f ms, bvh %.1f ms (%zu nodes, depth %d) | pmf eval %.0f ns\n",
                        s.name.c_str(), n, tref, tal * 1e3, tbvh * 1e3, bvh.nodes.size(), depth, nsPmf);
            std::printf("     uniform  relVar %10.4g  z %+5.2f  %6.0f ns/est  eff %7.3f\n", su.relVar, su.z, su.nsPerEst, eff(su));
            std::printf("     power    relVar %10.4g  z %+5.2f  %6.0f ns/est  eff %7.3f\n", sp.relVar, sp.z, sp.nsPerEst, eff(sp));
            std::printf("     bvh      relVar %10.4g  z %+5.2f  %6.0f ns/est  eff %7.3f\n", sb.relVar, sb.z, sb.nsPerEst, eff(sb));
            for (int t = 0; t < 2; ++t)
                std::printf("     split%.2f relVar %10.4g  z %+5.2f  %6.0f ns/est  eff %7.3f  [%.2f samples]\n", taus[t],
                            ss[t].relVar, ss[t].z, ss[t].nsPerEst, eff(ss[t]), ss[t].samplesPerEst);
            std::fflush(stdout);
        }
    }
}

void e2() {
    std::printf("\n== E2: PMF consistency of the BVH (sum over all lights; frequency test, 20M draws per receiver)\n");
    for (int kind = 0; kind < 3; ++kind) {
        Scene s = makeScene(kind, 10000, 8, 4242 + kind);
        LightBVH bvh;
        bvh.build(s.lights);
        int n = int(s.lights.size());
        for (int i = 0; i < 4; ++i) {
            double sum = 0;
            std::vector<double> pm(n);
            for (int l = 0; l < n; ++l)
                sum += pm[l] = bvh.pmf(s.P[i], s.N[i], uint32_t(l));
            const int draws = 20000000;
            std::vector<uint32_t> cnt(n, 0);
            Rng r(31 + i, 5);
            double maxRelPmfErr = 0;
            for (int k = 0; k < draws; ++k) {
                float p;
                int li = bvh.sample(s.P[i], s.N[i], r.uf(), p);
                if (li >= 0) {
                    ++cnt[li];
                    if (k < 100000)
                        maxRelPmfErr = std::max(maxRelPmfErr, std::fabs(p - pm[li]) / pm[li]);
                }
            }
            double chi = 0, maxz = 0;
            int dof = 0;
            for (int l = 0; l < n; ++l) {
                double e = pm[l] * draws;
                if (e >= 20) {
                    double z = (cnt[l] - e) / std::sqrt(e * (1 - pm[l]));
                    chi += z * z;
                    maxz = std::max(maxz, std::fabs(z));
                    ++dof;
                }
            }
            std::printf("   %-6s rcv %d: sum PMF = %.7f; sample-vs-PMF rel. err max %.2e; chi2/dof = %.3f (dof %d), max |z| %.2f\n",
                        s.name.c_str(), i, sum, maxRelPmfErr, chi / std::max(dof, 1), dof, maxz);
        }
    }
}

void e3() {
    std::printf("\n== E3: build / refit after edits (street, single thread) and the variance afterwards\n");
    for (int n : {10000, 100000}) {
        Scene s = makeScene(0, n, 256, 99);
        std::vector<double> ref = reference(s, g_threads);
        LightBVH bvh;
        double t0 = now();
        bvh.build(s.lights);
        double tb = now() - t0;
        auto relVar = [&](const LightBVH &b, const Scene &sc, const std::vector<double> &rf) {
            return measure(sc, rf, 2048, g_threads, [&](int i, Rng &r, int &) {
                float pm;
                int li = b.sample(sc.P[i], sc.N[i], r.uf(), pm);
                return li < 0 ? 0.0 : double(sampleLightIrr(sc.lights[li], sc.P[i], sc.N[i], r.uf(), r.uf())) / pm;
            }).relVar;
        };
        double v0 = relVar(bvh, s, ref);
        std::printf("   N=%6d build %.1f ms, relVar %.4g\n", n, tb * 1e3, v0);
        // (a) intensity edit of every light
        Rng r(5, 5);
        for (Light &l : s.lights)
            l.L *= 0.5f + 1.5f * r.uf();
        std::vector<double> ref2 = reference(s, g_threads);
        t0 = now();
        bvh.refit();
        double tr = now() - t0;
        double va = relVar(bvh, s, ref2);
        LightBVH fresh;
        fresh.build(s.lights);
        double vfa = relVar(fresh, s, ref2);
        std::printf("     all intensities x U[0.5,2]: refit %.2f ms -> relVar %.4g; rebuild -> %.4g\n", tr * 1e3, va, vfa);
        // (b) move 1% and 10% of the lights to random places
        for (float frac : {0.01f, 0.1f}) {
            Scene s2 = s;
            float W = 2.0f * std::sqrt(float(n));
            for (Light &l : s2.lights)
                if (r.uf() < frac)
                    l.p = {W * r.uf(), 0.5f + r.uf(), W * r.uf()};
            std::vector<double> ref3 = reference(s2, g_threads);
            LightBVH moved = bvh;
            moved.lights = &s2.lights;
            t0 = now();
            moved.refit();
            double tm = now() - t0;
            double vm = relVar(moved, s2, ref3);
            LightBVH fb;
            fb.build(s2.lights);
            double vfb = relVar(fb, s2, ref3);
            std::printf("     %.0f%% of the lights moved anywhere: refit %.2f ms -> relVar %.4g; rebuild -> %.4g\n",
                        frac * 100, tm * 1e3, vm, vfb);
        }
    }
}

// ---------------------------------------------------------------------------
// E4: BSDF rays collecting every light they cross (lights do not block).
struct RayBVH {
    struct N4 {
        V3 lo, hi;
        uint32_t first, count; // leaf: first item, count > 0; interior: first = right child, count 0
    };
    std::vector<N4> nodes;
    std::vector<uint32_t> items;
    void build(const std::vector<Light> &ls) {
        items.resize(ls.size());
        std::iota(items.begin(), items.end(), 0u);
        std::vector<LB> b(ls.size());
        for (size_t i = 0; i < ls.size(); ++i)
            b[i] = boundsOf(ls[i]);
        nodes.clear();
        nodes.reserve(2 * ls.size());
        rec(b, 0, uint32_t(ls.size()));
    }
    uint32_t rec(const std::vector<LB> &b, uint32_t begin, uint32_t end) {
        uint32_t idx = uint32_t(nodes.size());
        nodes.push_back({});
        V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, clo = lo, chi = hi;
        for (uint32_t i = begin; i < end; ++i) {
            lo = vmin(lo, b[items[i]].lo);
            hi = vmax(hi, b[items[i]].hi);
            V3 c = (b[items[i]].lo + b[items[i]].hi) * 0.5f;
            clo = vmin(clo, c);
            chi = vmax(chi, c);
        }
        nodes[idx].lo = lo;
        nodes[idx].hi = hi;
        if (end - begin <= 4) {
            nodes[idx].first = begin;
            nodes[idx].count = end - begin;
            return idx;
        }
        // Binned SAH, 12 bins.
        auto area = [](V3 a, V3 c) {
            V3 e = c - a;
            return 2 * (e.x * e.y + e.y * e.z + e.z * e.x);
        };
        float best = 1e38f;
        int bd = -1, bs = -1;
        for (int dim = 0; dim < 3; ++dim) {
            float l0 = comp(clo, dim), h0 = comp(chi, dim);
            if (h0 == l0)
                continue;
            V3 blo[12], bhi[12];
            int bc[12] = {};
            for (int k = 0; k < 12; ++k) {
                blo[k] = {1e30f, 1e30f, 1e30f};
                bhi[k] = {-1e30f, -1e30f, -1e30f};
            }
            for (uint32_t i = begin; i < end; ++i) {
                float c = comp((b[items[i]].lo + b[items[i]].hi) * 0.5f, dim);
                int k = std::min(11, int(12 * (c - l0) / (h0 - l0)));
                blo[k] = vmin(blo[k], b[items[i]].lo);
                bhi[k] = vmax(bhi[k], b[items[i]].hi);
                ++bc[k];
            }
            for (int s = 0; s < 11; ++s) {
                V3 al{1e30f, 1e30f, 1e30f}, ah{-1e30f, -1e30f, -1e30f}, cl = al, ch = ah;
                int na = 0, nc = 0;
                for (int k = 0; k <= s; ++k)
                    if (bc[k]) {
                        al = vmin(al, blo[k]);
                        ah = vmax(ah, bhi[k]);
                        na += bc[k];
                    }
                for (int k = s + 1; k < 12; ++k)
                    if (bc[k]) {
                        cl = vmin(cl, blo[k]);
                        ch = vmax(ch, bhi[k]);
                        nc += bc[k];
                    }
                if (!na || !nc)
                    continue;
                float cost = na * area(al, ah) + nc * area(cl, ch);
                if (cost < best) {
                    best = cost;
                    bd = dim;
                    bs = s;
                }
            }
        }
        uint32_t mid = (begin + end) / 2;
        if (bd >= 0) {
            float l0 = comp(clo, bd), h0 = comp(chi, bd);
            auto pm = std::partition(items.begin() + begin, items.begin() + end, [&](uint32_t i) {
                float c = comp((b[i].lo + b[i].hi) * 0.5f, bd);
                return std::min(11, int(12 * (c - l0) / (h0 - l0))) <= bs;
            });
            mid = uint32_t(pm - items.begin());
            if (mid == begin || mid == end)
                mid = (begin + end) / 2;
        }
        rec(b, begin, mid);
        uint32_t right = rec(b, mid, end);
        nodes[idx].first = right;
        nodes[idx].count = 0;
        return idx;
    }
};

bool hitLight(const Light &l, V3 o, V3 d, float &t) {
    if (l.type == 0) {
        V3 oc = o - l.p;
        float b = dot(oc, d), c = dot(oc, oc) - l.r * l.r, disc = b * b - c;
        if (disc < 0)
            return false;
        float s = std::sqrt(disc);
        t = -b - s;
        if (t <= 0)
            t = -b + s;
        return t > 0;
    }
    V3 pv = cross(d, l.e2);
    float det = dot(l.e1, pv);
    if (std::fabs(det) < 1e-12f)
        return false;
    float inv = 1 / det;
    V3 tv = o - l.p;
    float u = dot(tv, pv) * inv;
    if (u < 0 || u > 1)
        return false;
    V3 qv = cross(tv, l.e1);
    float v = dot(d, qv) * inv;
    if (v < 0 || u + v > 1)
        return false;
    t = dot(l.e2, qv) * inv;
    return t > 0;
}

inline bool slab(V3 o, V3 inv, V3 lo, V3 hi) {
    float t0 = 0, t1 = 1e30f;
    for (int a = 0; a < 3; ++a) {
        float tn = (comp(lo, a) - comp(o, a)) * comp(inv, a), tf = (comp(hi, a) - comp(o, a)) * comp(inv, a);
        if (tn > tf)
            std::swap(tn, tf);
        t0 = std::max(t0, tn);
        t1 = std::min(t1, tf);
    }
    return t0 <= t1;
}

void e4() {
    std::printf("\n== E4: BSDF (cosine) rays from the receivers collecting every light they cross\n");
    for (int kind = 0; kind < 3; ++kind) {
        for (int n : {1000, 10000, 100000}) {
            Scene s = makeScene(kind, n, 256, 42 + kind);
            RayBVH rb;
            double t0 = now();
            rb.build(s.lights);
            double tb = now() - t0;
            const int R = kind == 1 ? 20000 : 20000;
            std::vector<V3> O(R), D(R);
            Rng r(3, 3);
            for (int i = 0; i < R; ++i) {
                int k = i % int(s.P.size());
                V3 nn = s.N[k], t, b;
                frame(nn, t, b);
                float u1 = r.uf(), u2 = r.uf(), rr = std::sqrt(u1), ph = 2 * kPi * u2;
                O[i] = s.P[k] + nn * 1e-4f;
                D[i] = normalize(t * (rr * std::cos(ph)) + b * (rr * std::sin(ph)) + nn * safeSqrt(1 - u1));
            }
            long long hb = 0, hv = 0;
            int bruteRays = n >= 100000 ? 2000 : R;
            t0 = now();
            for (int i = 0; i < bruteRays; ++i)
                for (const Light &l : s.lights) {
                    float t;
                    hb += hitLight(l, O[i], D[i], t);
                }
            double tbr = (now() - t0) / bruteRays * 1e9;
            t0 = now();
            std::vector<uint32_t> stack(128);
            long long visited = 0;
            for (int i = 0; i < R; ++i) {
                V3 inv{1 / D[i].x, 1 / D[i].y, 1 / D[i].z};
                int sp = 0;
                stack[sp++] = 0;
                while (sp) {
                    const RayBVH::N4 &nd = rb.nodes[stack[--sp]];
                    ++visited;
                    if (!slab(O[i], inv, nd.lo, nd.hi))
                        continue;
                    if (nd.count) {
                        for (uint32_t k = 0; k < nd.count; ++k) {
                            float t;
                            hv += hitLight(s.lights[rb.items[nd.first + k]], O[i], D[i], t);
                        }
                    } else {
                        stack[sp++] = uint32_t(&nd - rb.nodes.data()) + 1;
                        stack[sp++] = nd.first;
                    }
                }
            }
            double tbv = (now() - t0) / R * 1e9;
            // The same query through the light-selection BVH (SAOH, one light per leaf).
            LightBVH lb;
            lb.build(s.lights);
            long long hl = 0, visitedL = 0;
            t0 = now();
            for (int i = 0; i < R; ++i) {
                V3 inv{1 / D[i].x, 1 / D[i].y, 1 / D[i].z};
                int sp = 0;
                stack[sp++] = 0;
                while (sp) {
                    const uint32_t k = stack[--sp];
                    const Node &nd = lb.nodes[k];
                    ++visitedL;
                    if (!slab(O[i], inv, nd.b.lo, nd.b.hi))
                        continue;
                    if (nd.leaf) {
                        float t;
                        hl += hitLight(s.lights[nd.child], O[i], D[i], t);
                    } else {
                        stack[sp++] = k + 1;
                        stack[sp++] = nd.child;
                    }
                }
            }
            double tlb = (now() - t0) / R * 1e9;
            std::printf("   %-6s N=%6d  brute %10.0f ns/ray (%.4f hits/ray) | ray BVH build %.1f ms, %7.0f ns/ray (%.4f hits/ray, %.1f nodes/ray)"
                        " | selection BVH %6.0f ns/ray (%.4f hits/ray, %.1f nodes/ray)\n",
                        s.name.c_str(), n, tbr, double(hb) / bruteRays, tb * 1e3, tbv, double(hv) / R,
                        double(visited) / R, tlb, double(hl) / R, double(visitedL) / R);
        }
    }
}

// E5: a distant light (irradiance Es at normal incidence) next to the street
// lights. Estimators choose the distant light with probability q:
//   half     q = 1/2 (pbrt-v4: infinite lights uniform among themselves and the BVH)
//   capped   q = the power share with the distant power capped at the local power (Nova today)
//   import   q = I_sun / (I_sun + I_root): the distant light's importance Es cos+ times the
//            factor that puts it into the BVH's units (sphere lights: phi/d^2 = 4 pi E)
void e5() {
    std::printf("\n== E5: a distant light next to N street lights: probability of picking it\n");
    for (int n : {1000, 10000}) {
        for (float share : {0.1f, 0.5f, 0.9f}) {
            Scene s = makeScene(0, n, 256, 555);
            std::vector<double> loc = reference(s, g_threads);
            double meanLoc = 0;
            for (double x : loc)
                meanLoc += x / loc.size();
            V3 ws = normalize(V3{0.3f, 1.0f, 0.2f});
            float cs = dot(ws, V3{0, 1, 0});
            // Es so that the sun's share of the mean irradiance is `share`.
            float Es = float(meanLoc * share / (1 - share) / cs);
            std::vector<double> ref(loc.size());
            for (size_t i = 0; i < loc.size(); ++i)
                ref[i] = loc[i] + Es * cs;
            LightBVH bvh;
            bvh.build(s.lights);
            double localPower = 0;
            for (const Light &l : s.lights)
                localPower += l.power();
            float W = 2.0f * std::sqrt(float(n));
            double sceneR = 0.5 * std::sqrt(2.0 * W * W + 2.25);
            double sunPower = Es * kPi * sceneR * sceneR;
            double qCap = std::min(sunPower, localPower) / (std::min(sunPower, localPower) + localPower);
            std::vector<uint32_t> cut{0};
            for (int level = 0; level < 4; ++level) {
                std::vector<uint32_t> next;
                for (uint32_t k : cut)
                    if (bvh.nodes[k].leaf)
                        next.push_back(k);
                    else {
                        next.push_back(k + 1);
                        next.push_back(bvh.nodes[k].child);
                    }
                cut.swap(next);
            }
            auto run = [&](int mode) {
                return measure(s, ref, 4096, g_threads, [&](int i, Rng &r, int &) {
                    double q;
                    if (mode == 0)
                        q = 0.5;
                    else if (mode == 1)
                        q = qCap;
                    else if (mode == 2 || mode == 3) {
                        double is = 4 * kPi * Es * std::max(0.0f, dot(ws, s.N[i]));
                        double ir = 0;
                        if (mode == 2)
                            ir = importance(bvh.nodes[0].b, s.P[i], s.N[i]);
                        else // the 16 nodes at depth 4 (a cut through the tree)
                            for (uint32_t k : cut)
                                ir += importance(bvh.nodes[k].b, s.P[i], s.N[i]);
                        q = is / (is + ir);
                    } else { // oracle: the true share of the distant light
                        q = Es * std::max(0.0f, dot(ws, s.N[i])) / ref[i];
                    }
                    if (r.uf() < q)
                        return Es * std::max(0.0f, dot(ws, s.N[i])) / q;
                    float pm;
                    int li = bvh.sample(s.P[i], s.N[i], r.uf(), pm);
                    return li < 0 ? 0.0
                                  : double(sampleLightIrr(s.lights[li], s.P[i], s.N[i], r.uf(), r.uf())) / pm / (1 - q);
                });
            };
            Stats a = run(0), b = run(1), c = run(2), d = run(3), o = run(4);
            std::printf("   N=%6d sun share %.1f: relVar half %.4g | capped power (q=%.3f) %.4g | importance root %.4g | "
                        "cut16 %.4g | oracle %.4g  (z %+.2f %+.2f %+.2f %+.2f %+.2f)\n",
                        n, share, a.relVar, qCap, b.relVar, c.relVar, d.relVar, o.relVar, a.z, b.z, c.z, d.z, o.z);
        }
    }
}

// E6: the traversal reuses one uniform number down the tree (pbrt-v4). With
// float, the number loses about one bit per level; compare with double.
void e6() {
    std::printf("\n== E6: float vs double reuse of the uniform number during traversal (street, 100k lights)\n");
    Scene s = makeScene(0, 100000, 4, 4243);
    LightBVH bvh;
    bvh.build(s.lights);
    int n = int(s.lights.size());
    for (int i = 0; i < 2; ++i) {
        std::vector<double> pm(n);
        for (int l = 0; l < n; ++l)
            pm[l] = bvh.pmf(s.P[i], s.N[i], uint32_t(l));
        for (int prec = 0; prec < 2; ++prec) {
            const int draws = 20000000;
            std::vector<uint32_t> cnt(n, 0);
            Rng r(31 + i, 5);
            for (int k = 0; k < draws; ++k) {
                float p;
                int li = prec == 0 ? bvh.sample(s.P[i], s.N[i], r.uf(), p) : bvh.sample(s.P[i], s.N[i], r.ud(), p);
                if (li >= 0)
                    ++cnt[li];
            }
            double chi = 0, maxz = 0;
            int dof = 0, reachable = 0, never = 0;
            for (int l = 0; l < n; ++l) {
                double e = pm[l] * draws;
                if (pm[l] > 0)
                    ++reachable;
                if (e >= 5 && cnt[l] == 0)
                    ++never;
                if (e >= 20) {
                    double z = (cnt[l] - e) / std::sqrt(e * (1 - pm[l]));
                    chi += z * z;
                    maxz = std::max(maxz, std::fabs(z));
                    ++dof;
                }
            }
            std::printf("   rcv %d %-6s: chi2/dof %.3f (dof %d), max |z| %.2f, lights expected >= 5 times but never drawn: %d\n",
                        i, prec ? "double" : "float", chi / std::max(dof, 1), dof, maxz, never);
        }
    }
}


// E8: the receiver term of the importance for one-sided receivers (cloud:
// random normals): bounded to the upper hemisphere vs pbrt-v4's |cos|.
void e8() {
    std::printf("\n== E8: receiver bound, one-sided (upper hemisphere) vs pbrt-v4 |cos| (cloud)\n");
    for (int n : {1000, 10000, 100000}) {
        Scene s = makeScene(1, n, 256, 43);
        std::vector<double> ref = reference(s, g_threads);
        LightBVH bvh;
        bvh.build(s.lights);
        double rv[2], spl[2];
        for (int mode = 0; mode < 2; ++mode) {
            g_oneSided = mode == 0;
            std::atomic<long long> none{0};
            Stats st = measure(s, ref, 4096, g_threads, [&](int i, Rng &r, int &) {
                float pm;
                int li = bvh.sample(s.P[i], s.N[i], r.uf(), pm);
                if (li < 0)
                    ++none;
                return li < 0 ? 0.0 : double(sampleLightIrr(s.lights[li], s.P[i], s.N[i], r.uf(), r.uf())) / pm;
            });
            rv[mode] = st.relVar;
            spl[mode] = 1.0 - double(none.load()) / (256.0 * 4096.0 + 64.0 * 2048.0);
        }
        g_oneSided = true;
        std::printf("   N=%6d relVar one-sided %.4g | |cos| %.4g  (draws that yield a light: %.3f | %.3f)\n", n, rv[0], rv[1],
                    spl[0], spl[1]);
    }
}

// E7: splitting criteria and an unbiasedness check with a finer reference
// (64 samples per light) and three seeds.
void e7() {
    std::printf("\n== E7: splitting criteria (sin half-angle threshold tau, sample budget) and bias over 3 seeds\n");
    struct Cfg {
        int kind, n;
    };
    for (Cfg c : {Cfg{0, 10000}, Cfg{1, 1000}, Cfg{2, 10000}, Cfg{2, 100000}}) {
        Scene s = makeScene(c.kind, c.n, 256, 42 + c.kind);
        std::vector<double> ref = reference(s, g_threads, 64);
        LightBVH bvh;
        bvh.build(s.lights);
        std::vector<float> w(c.n);
        for (int i = 0; i < c.n; ++i)
            w[i] = s.lights[i].power();
        Alias al;
        al.build(w);
        std::printf("   %-6s N=%6d\n", s.name.c_str(), c.n);
        struct Split {
            float tau;
            int budget;
        };
        const Split splits[] = {{1e9f, 1}, {0.5f, 8}, {1.0f, 4}, {2.0f, 4}, {2.0f, 2}};
        double base = 0;
        for (const Split &sp : splits) {
            double zs[3], rv = 0, spe = 0, ns = 0;
            for (int seed = 0; seed < 3; ++seed) {
                g_seed = uint64_t(seed + 1);
                Stats st = measure(s, ref, 4096, g_threads, [&](int i, Rng &r, int &used) {
                    int budget = sp.budget;
                    used = 0;
                    return double(estimateSplit(bvh, s.lights, 0, s.P[i], s.N[i], r, sp.tau, budget, used));
                });
                zs[seed] = st.z;
                rv += st.relVar / 3;
                spe += st.samplesPerEst / 3;
                ns += st.nsPerEst / 3;
            }
            g_seed = 0;
            if (sp.budget == 1)
                base = rv;
            std::printf("     %s relVar %8.4g  [%.2f samples]  eff vs bvh %.3f  %6.0f ns/est  z %+.2f %+.2f %+.2f\n",
                        sp.budget == 1 ? "bvh            " : (std::string("split tau ") + std::to_string(sp.tau).substr(0, 4) + " b" + std::to_string(sp.budget)).c_str(),
                        rv, spe, base / (rv * spe), ns, zs[0], zs[1], zs[2]);
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    std::string what = argc > 1 ? argv[1] : "all";
    if (argc > 2)
        g_threads = std::max(1, std::atoi(argv[2]));
    else
        g_threads = std::max(1u, std::thread::hardware_concurrency());
    std::printf("lt_light_lab: %s, %d threads\n", what.c_str(), g_threads);
    if (what == "e1" || what == "all")
        e1();
    if (what == "e2" || what == "all")
        e2();
    if (what == "e3" || what == "all")
        e3();
    if (what == "e4" || what == "all")
        e4();
    if (what == "e5" || what == "all")
        e5();
    if (what == "e6" || what == "all")
        e6();
    if (what == "e7" || what == "all")
        e7();
    if (what == "e8" || what == "all")
        e8();
    return 0;
}
