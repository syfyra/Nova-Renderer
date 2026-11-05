// M9 research prototype (docs/research/m9/sss.md): random-walk subsurface
// scattering in analytic geometry (half space, finite slab, sphere), no
// renderer around it. Standalone C++20, no dependencies.
//
// Experiments (each prints one block; the report quotes the numbers):
//   E1  albedo inversion: total diffuse reflectance of a half space for the
//       single-scattering albedo that the inversions give for a target colour
//       A (van de Hulst / Chiang et al. 2016 / OpenPBR 2025 eq. 46-50, used by
//       Karma; Chiang et al.'s searchlight fit; Cycles' anisotropic fit as
//       recalled), for several entry/exit boundary models and HG anisotropy;
//   E2  plain vs Dwivedi-guided walks (one-sample MIS of both): variance,
//       segments and efficiency, half space and finite sphere;
//   E3  chromatic mean free paths: one walk with per-step spectral MIS
//       (uniform or throughput-weighted channel choice), a per-walk hero
//       channel with path-level spectral MIS, and three separate walks;
//   E4  step counts and the energy carried by walks longer than a cap
//       (Karma's entry model: cosine entry, matched boundary);
//   E5  a sphere in a white furnace vs radius / mfp (the Karma scenes f_*);
//   E6  finite slabs lit from behind and in front (the Karma scenes slab_*),
//       transmitted radiance along the normal, plus the angular behaviour of
//       the half space under a light from the viewing direction (scene head).
//
// Build: sss_build.bat <outdir>   (cl /O2 /std:c++20)
// Run:   sss_lab.exe [all|e1..e6] [threads=0] [scale=1] [seed=1]

#define NOMINMAX
#define _USE_MATH_DEFINES
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static const double kPi = 3.14159265358979323846;
static const double kInf = 1e300;
static int gThreads = 0;
static double gScale = 1.0;
static uint64_t gSeed = 1;

// ---------------------------------------------------------------- basics
struct V3 {
    double x = 0, y = 0, z = 0;
};
static inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
static inline V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
static inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 normalize(V3 a) { return a * (1.0 / std::sqrt(dot(a, a))); }

struct Rng { // PCG32
    uint64_t state, inc;
    Rng(uint64_t seed, uint64_t stream) : state(0), inc((stream << 1u) | 1u) {
        next();
        state += seed;
        next();
    }
    uint32_t next() {
        uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        uint32_t xs = uint32_t(((old >> 18u) ^ old) >> 27u), rot = uint32_t(old >> 59u);
        return (xs >> rot) | (xs << ((~rot + 1u) & 31));
    }
    double u() { // [0, 1) with 53 bits
        uint64_t a = next(), b = next();
        return double((a << 21) ^ b) * (1.0 / 9007199254740992.0);
    }
};

static void basis(V3 n, V3 &t, V3 &b) {
    const double s = n.z >= 0 ? 1.0 : -1.0, a = -1.0 / (s + n.z), c = n.x * n.y * a;
    t = {1 + s * n.x * n.x * a, s * c, -s * n.x};
    b = {c, s + n.y * n.y * a, -n.y};
}
static V3 fromLocal(V3 n, double ct, double phi) {
    V3 t, b;
    basis(n, t, b);
    const double st = std::sqrt(std::max(0.0, 1 - ct * ct));
    return t * (st * std::cos(phi)) + b * (st * std::sin(phi)) + n * ct;
}
static double hgPdf(double ct, double g) {
    const double d = 1 + g * g - 2 * g * ct;
    return (1 - g * g) / (4 * kPi * d * std::sqrt(d));
}
// cos of the angle to the travel direction (g > 0 forward)
static double hgSampleCos(double g, double u) {
    if (std::fabs(g) < 1e-4)
        return 1 - 2 * u;
    const double s = (1 - g * g) / (1 - g + 2 * g * u);
    return std::clamp((1 + g * g - s * s) / (2 * g), -1.0, 1.0);
}
static V3 sampleCosine(V3 n, Rng &r) { return fromLocal(n, std::sqrt(r.u()), 2 * kPi * r.u()); }
static double fresnelDielectric(double cosI, double eta /* n_t / n_i */) {
    cosI = std::clamp(cosI, 0.0, 1.0);
    const double s2 = (1 - cosI * cosI) / (eta * eta);
    if (s2 >= 1)
        return 1;
    const double cosT = std::sqrt(1 - s2);
    const double rs = (cosI - eta * cosT) / (cosI + eta * cosT), rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5 * (rs * rs + rp * rp);
}

// ---------------------------------------------------------------- parallel
template <class F> void parallel(int tasks, F &&f) {
    const int n = gThreads > 0 ? gThreads : std::max(1u, std::thread::hardware_concurrency());
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t)
        pool.emplace_back([&] {
            for (int i; (i = next++) < tasks;)
                f(i);
        });
    for (auto &th : pool)
        th.join();
}
static double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- albedo inversions
// van de Hulst's semi-infinite diffuse albedo as a function of the similarity
// parameter s = sqrt((1 - a) / (1 - g a)), and its exact inverse (the
// coefficients of Chiang et al. 2016 eq. and OpenPBR 2025 eq. 47).
static double vdhAlbedo(double s) { return (1 - s) * (1 - 0.139 * s) / (1 + 1.17 * s); }
static double vdhInverseS(double A) { return 4.09712 + 4.20863 * A - std::sqrt(9.59217 + A * (41.6808 + 17.7126 * A)); }
// OpenPBR 2025 (Karma 22.0 mxpbr): sigma_t = 1 / mfp, alpha from eq. 46-50.
static double alphaOpenPbr(double A, double g) {
    const double s = vdhInverseS(A), a = 1 - s * s;
    return a / (1 - g * (1 - a));
}
// Chiang, Kutz, Burley 2016 searchlight fit (Karma's disabled branch): alpha
// and an mfp scale s so that sigma_t = 1 / (d * s) for a Burley-style d.
static double alphaChiang16(double A) { return 1 - std::exp(-5.09406 * A + 2.61188 * A * A - 4.31805 * A * A * A); }
static double mfpScaleChiang16(double A) { return 1.9 - A + 3.5 * (A - 0.8) * (A - 0.8); }
// Cycles' random walk remap (subsurface_random_walk_remap), coefficients as
// recalled for this research; sigma_t = 1 / (d (1 - g)).
static double alphaCycles(double A, double g) {
    const double g2 = g * g, g3 = g2 * g, g4 = g3 * g, g5 = g4 * g, g6 = g5 * g, g7 = g6 * g;
    const double a = 1.8260523782 - 1.28451056436 * g - 1.79904629312 * g2 + 9.19393289202 * g3 - 22.8215585862 * g4 +
                     32.0234874259 * g5 - 23.6264803333 * g6 + 7.21067002658 * g7;
    const double b = 4.98511194385 + 0.127355959438 * std::exp(31.1491581433 * g - 201.847017512 * g2 + 841.576016723 * g3 -
                                                               2018.09288505 * g4 + 2731.71560286 * g5 -
                                                               1935.41424244 * g6 + 559.009054474 * g7);
    const double c = 1.09686102424 - 0.394704063468 * g + 1.05258115941 * g2 - 8.83963712726 * g3 + 28.8643230661 * g4 -
                     46.8802913581 * g5 + 38.5402837518 * g6 - 12.7181042538 * g7;
    const double d = 0.496310210422 + 0.360146581622 * g - 2.15139309747 * g2 + 17.8896899217 * g3 - 55.2984010333 * g4 +
                     82.065982243 * g5 - 58.5106008578 * g6 + 15.8478295021 * g7;
    const double e = 4.23190299701 + 0.00310603949088 * std::exp(76.7316253952 * g - 594.356773233 * g2 + 2448.8834203 * g3 -
                                                                 5576.68528998 * g4 + 7116.60171912 * g5 -
                                                                 4763.54467887 * g6 + 1303.5318055 * g7);
    const double f = 2.40602999408 - 2.51814844609 * g + 9.18494908356 * g2 - 79.2191708682 * g3 + 259.082868209 * g4 -
                     403.613804597 * g5 + 302.85712436 * g6 - 87.4370473567 * g7;
    const double blend = std::pow(A, 0.25);
    const double al = (1 - blend) * a * std::pow(std::atan(b * A), c) + blend * d * std::pow(std::atan(e * A), f);
    return std::clamp(al, 0.0, 0.999999);
}

// Dwivedi's diffusion eigenvalue for isotropic scattering: alpha nu atanh(1/nu) = 1.
static double dwivediNu(double alpha) {
    if (alpha >= 1)
        return 1e9;
    double lo = 1 + 1e-12, hi = 1e9;
    for (int i = 0; i < 200; ++i) {
        const double m = std::sqrt(lo * hi) > 2 ? std::sqrt(lo * hi) : 0.5 * (lo + hi);
        const double v = alpha * m * std::atanh(1 / m);
        (v > 1 ? lo : hi) = m; // the left side falls towards alpha as nu grows
    }
    return 0.5 * (lo + hi);
}
// Meng, Hanika, Dachsbacher 2016 approximation (as used by Cycles, recalled).
static double dwivediNuApprox(double alpha) {
    return 1 / std::sqrt(1 - std::pow(alpha, 2.44294 - 0.0215813 * alpha + 0.578637 / alpha));
}

// ---------------------------------------------------------------- geometry
enum class Shape { HalfSpace, Slab, Sphere };
struct Geo {
    Shape shape = Shape::HalfSpace;
    double d = 1;   // slab thickness (medium -d < z < 0)
    double R = 1;   // sphere radius (centre 0)
    // distance along dir to the boundary, the outward normal there and the face (0 top, 1 bottom)
    double exitT(V3 p, V3 dir, V3 &n, int &face) const {
        switch (shape) {
        case Shape::HalfSpace:
            n = {0, 0, 1};
            face = 0;
            return dir.z > 0 ? -p.z / dir.z : kInf;
        case Shape::Slab:
            if (dir.z > 0) {
                n = {0, 0, 1};
                face = 0;
                return -p.z / dir.z;
            }
            if (dir.z < 0) {
                n = {0, 0, -1};
                face = 1;
                return (-d - p.z) / dir.z;
            }
            return kInf;
        default: {
            const double b = dot(p, dir), c = dot(p, p) - R * R;
            const double t = -b + std::sqrt(std::max(0.0, b * b - c));
            n = normalize(p + dir * t);
            face = 0;
            return std::max(t, 0.0);
        }
        }
    }
};

// ---------------------------------------------------------------- the mono walk
struct Medium {
    double sigt = 1, alpha = 0.9, g = 0;
    double eta = 1; // index inside / outside: 1 = matched (no internal reflection)
};
struct Guide {
    bool on = false;
    double pg = 0.75; // probability of the guided strategy per segment
    double nu = 0;    // Dwivedi eigenvalue
    V3 axis{0, 0, 1}; // towards the entry surface
};
struct WalkOut {
    double w = 0;  // escape weight (0: absorbed or killed)
    int segs = 0;  // segments traced (closest-hit queries)
    int face = -1; // exit face
    V3 dir;        // exit direction
    bool ballistic = false;
};

// One walk from p (on the boundary) in direction dir (into the medium).
// Throughput weighted by alpha per scattering, Russian roulette below 0.1.
static WalkOut walkMono(const Geo &geo, const Medium &m, V3 p, V3 dir, Rng &r, const Guide &gd, int cap = 1 << 30) {
    WalkOut o;
    double w = 1;
    bool scattered = false, first = true;
    const double L = gd.on ? std::log((gd.nu + 1) / (gd.nu - 1)) : 0;
    double phasePdfPrev = 0; // HG pdf of dir given the previous direction (classical density)
    V3 prevDir = dir;
    for (int seg = 0; seg < cap; ++seg) {
        // Direction: the first one is given; later ones from HG or Dwivedi.
        double pdfDirD = 0, pdfDirC = 0;
        if (!first) {
            const bool guided = gd.on && r.u() < gd.pg;
            if (guided) {
                const double mu = gd.nu - (gd.nu + 1) * std::exp(-r.u() * L);
                dir = fromLocal(gd.axis, std::clamp(mu, -1.0, 1.0), 2 * kPi * r.u());
            } else {
                dir = fromLocal(prevDir, hgSampleCos(m.g, r.u()), 2 * kPi * r.u());
            }
            pdfDirC = hgPdf(dot(prevDir, dir), m.g);
            if (gd.on)
                pdfDirD = 1 / (2 * kPi * L * (gd.nu - dot(dir, gd.axis)));
        }
        // Distance: classical sigma_t or Dwivedi's stretched sigma'.
        const double sigD = gd.on ? m.sigt * (1 - dot(dir, gd.axis) / gd.nu) : m.sigt;
        const bool guidedT = gd.on && r.u() < gd.pg;
        const double t = -std::log(1 - r.u()) / (guidedT ? sigD : m.sigt);
        V3 n;
        int face;
        const double tb = geo.exitT(p, dir, n, face);
        ++o.segs;
        const double pc = first ? 1.0 : pdfDirC, pd = first ? 1.0 : pdfDirD;
        if (t >= tb) {
            // Reaches the boundary.
            const double f = (first ? 1.0 : pdfDirC) * std::exp(-m.sigt * tb);
            const double q = gd.on ? gd.pg * pd * std::exp(-sigD * tb) + (1 - gd.pg) * pc * std::exp(-m.sigt * tb)
                                   : pc * std::exp(-m.sigt * tb);
            w *= f / q;
            p = p + dir * tb;
            if (m.eta != 1) {
                const double F = fresnelDielectric(dot(dir, n), 1 / m.eta);
                if (r.u() < F) { // internal reflection, continue inside
                    dir = dir - n * (2 * dot(dir, n));
                    prevDir = dir;
                    first = true; // the next direction is given, not sampled
                    continue;
                }
                // refracted exit direction
                const double ci = dot(dir, n), e = m.eta, s2 = e * e * (1 - ci * ci);
                dir = normalize(dir * e + n * (std::sqrt(std::max(0.0, 1 - s2)) - e * ci));
            }
            o.w = w;
            o.face = face;
            o.dir = dir;
            o.ballistic = !scattered;
            return o;
        }
        // Scatters at t.
        const double f = (first ? 1.0 : pdfDirC) * m.sigt * m.alpha * std::exp(-m.sigt * t);
        const double q = gd.on ? gd.pg * pd * sigD * std::exp(-sigD * t) + (1 - gd.pg) * pc * m.sigt * std::exp(-m.sigt * t)
                               : pc * m.sigt * std::exp(-m.sigt * t);
        w *= f / q;
        p = p + dir * t;
        prevDir = dir;
        first = false;
        scattered = true;
        if (w < 0.1) {
            if (r.u() < w / 0.1)
                w = 0.1;
            else
                return o;
        }
    }
    o.segs = cap;
    return o; // killed at the cap
}

// ---------------------------------------------------------------- statistics
struct Acc {
    double n = 0, s = 0, s2 = 0, segs = 0, time = 0;
    void add(double x) {
        n += 1;
        s += x;
        s2 += x * x;
    }
    void merge(const Acc &o) {
        n += o.n;
        s += o.s;
        s2 += o.s2;
        segs += o.segs;
    }
    double mean() const { return s / n; }
    double var() const { return std::max(0.0, s2 / n - mean() * mean()); }
    double se() const { return std::sqrt(var() / n); }
};

// Runs `count` walks in chunks; f(rng, acc[]) adds one sample to each accumulator.
template <int K, class F> std::array<Acc, K> run(long long count, uint64_t salt, F &&f) {
    const int chunks = 256;
    std::vector<std::array<Acc, K>> part(chunks);
    const double t0 = now();
    parallel(chunks, [&](int c) {
        Rng r(gSeed * 0x9E3779B97F4A7C15ull + salt * 7919 + c, salt + 17 * c + 1);
        const long long n = count / chunks + (c < count % chunks ? 1 : 0);
        for (long long i = 0; i < n; ++i)
            f(r, part[c]);
    });
    std::array<Acc, K> out{};
    for (auto &p : part)
        for (int k = 0; k < K; ++k)
            out[k].merge(p[k]);
    for (auto &a : out)
        a.time = now() - t0;
    return out;
}
static long long N(double base) { return std::max(1000LL, (long long)(base * gScale)); }

// ---------------------------------------------------------------- E1
enum class Entry { Normal, Cosine, DielDiffuse };
static const char *entryName(Entry e) {
    return e == Entry::Normal ? "normal" : e == Entry::Cosine ? "cosine" : "dielectric diffuse";
}
// Half-space reflectance for one entry model; eta applies at exit (and for DielDiffuse at entry).
static Acc halfSpaceR(const Medium &m, Entry e, long long n, uint64_t salt, const Guide &gd = Guide{}) {
    auto a = run<1>(n, salt, [&](Rng &r, std::array<Acc, 1> &acc) {
        V3 dir{0, 0, -1};
        double w0 = 1;
        if (e == Entry::Cosine) {
            dir = sampleCosine({0, 0, -1}, r);
        } else if (e == Entry::DielDiffuse) {
            const V3 in = sampleCosine({0, 0, -1}, r); // incident travel direction, cosine weighted
            const double ci = -in.z, F = fresnelDielectric(ci, m.eta);
            if (r.u() < F) {
                acc[0].add(0); // specular reflection: not subsurface
                return;
            }
            const double e2 = 1 / m.eta, s2 = e2 * e2 * (1 - ci * ci);
            dir = normalize(in * e2 + V3{0, 0, -1} * (std::sqrt(1 - s2) - e2 * ci));
        }
        Geo geo;
        WalkOut o = walkMono(geo, m, {0, 0, 0}, dir, r, gd);
        acc[0].add(o.w * w0);
        acc[0].segs += o.segs;
    });
    return a[0];
}

static void e1() {
    std::printf("\n== E1: albedo inversion, half space, total diffuse reflectance R vs target colour A ==\n");
    const double As[] = {0.05, 0.2, 0.5, 0.8, 0.95, 0.99};
    const long long n = N(4e6);
    std::printf("-- OpenPBR 2025 / Karma: sigma_t = 1/mfp, alpha = (1-s^2)/(1-g s^2), s = vdH^-1(A); g = 0\n");
    std::printf("%6s %9s | %-26s | %-26s | %-26s | %-26s\n", "A", "alpha", "normal entry, matched", "cosine entry, matched",
                "cosine entry, eta 1.5 exit", "diel. diffuse, eta 1.5");
    for (double A : As) {
        Medium m;
        m.alpha = alphaOpenPbr(A, 0);
        std::printf("%6.2f %9.6f |", A, m.alpha);
        for (int k = 0; k < 4; ++k) {
            Medium mm = m;
            Entry e = k == 0 ? Entry::Normal : k == 3 ? Entry::DielDiffuse : Entry::Cosine;
            if (k >= 2)
                mm.eta = 1.5;
            Acc a = halfSpaceR(mm, e, n, 100 + k * 13 + uint64_t(A * 1000));
            std::printf(" %.4f+-%.4f (R/A %.3f) |", a.mean(), a.se(), a.mean() / A);
        }
        std::printf("\n");
    }
    std::printf("-- anisotropy: OpenPBR/Karma eq. 50 vs Cycles' remap (sigma_t = 1/(mfp(1-g))); normal | cosine entry, matched\n");
    std::printf("%6s %6s %9s | %-36s | %9s | %-36s\n", "g", "A", "alphaOP", "OpenPBR: R normal / R cosine", "alphaCy",
                "Cycles: R normal / R cosine");
    for (double g : {-0.5, 0.0, 0.5, 0.9})
        for (double A : {0.2, 0.5, 0.8}) {
            Medium m;
            m.g = g;
            m.alpha = alphaOpenPbr(A, g);
            Acc a0 = halfSpaceR(m, Entry::Normal, n / 2, 300 + uint64_t(A * 100 + g * 10 + 50));
            Acc a1 = halfSpaceR(m, Entry::Cosine, n / 2, 400 + uint64_t(A * 100 + g * 10 + 50));
            Medium c = m;
            c.alpha = alphaCycles(A, g);
            c.sigt = 1 / (1 - g);
            Acc c0 = halfSpaceR(c, Entry::Normal, n / 2, 500 + uint64_t(A * 100 + g * 10 + 50));
            Acc c1 = halfSpaceR(c, Entry::Cosine, n / 2, 600 + uint64_t(A * 100 + g * 10 + 50));
            std::printf("%6.2f %6.2f %9.6f | %.4f (%.3f) / %.4f (%.3f)         | %9.6f | %.4f (%.3f) / %.4f (%.3f)\n", g, A,
                        m.alpha, a0.mean(), a0.mean() / A, a1.mean(), a1.mean() / A, c.alpha, c0.mean(), c0.mean() / A,
                        c1.mean(), c1.mean() / A);
        }
    std::printf("-- Chiang 2016 searchlight fit (Karma's disabled branch): alpha(A), sigma_t = 1/(d*s(A)); normal | cosine\n");
    for (double A : As) {
        Medium m;
        m.alpha = alphaChiang16(A);
        Acc a0 = halfSpaceR(m, Entry::Normal, n / 2, 700 + uint64_t(A * 1000));
        Acc a1 = halfSpaceR(m, Entry::Cosine, n / 2, 800 + uint64_t(A * 1000));
        std::printf("A %.2f alpha %.6f s(A) %.3f: R %.4f (R/A %.3f) / %.4f (%.3f)\n", A, m.alpha, mfpScaleChiang16(A),
                    a0.mean(), a0.mean() / A, a1.mean(), a1.mean() / A);
    }
    std::printf("-- the van de Hulst formula itself (A from s) vs measured R at alpha from a grid, cosine entry, g = 0\n");
    for (double al : {0.3, 0.6, 0.9, 0.99, 0.999}) {
        Medium m;
        m.alpha = al;
        const double s = std::sqrt(1 - al);
        Acc a0 = halfSpaceR(m, Entry::Normal, n / 2, 900 + uint64_t(al * 1000));
        Acc a1 = halfSpaceR(m, Entry::Cosine, n / 2, 950 + uint64_t(al * 1000));
        std::printf("alpha %.3f vdH %.4f  R normal %.4f  R cosine %.4f\n", al, vdhAlbedo(s), a0.mean(), a1.mean());
    }
}

// ---------------------------------------------------------------- E2
static void e2() {
    std::printf("\n== E2: plain vs Dwivedi-guided walks (one-sample MIS), g = 0 ==\n");
    std::printf("nu: exact root vs Meng et al.'s approximation\n");
    for (double al : {0.5, 0.9, 0.99, 0.999, 0.9999})
        std::printf("  alpha %.4f nu %.5f approx %.5f\n", al, dwivediNu(al), dwivediNuApprox(al));
    const long long n = N(2e6);
    for (int geoKind = 0; geoKind < 3; ++geoKind) {
        Geo geo;
        if (geoKind >= 1) {
            geo.shape = Shape::Sphere;
            geo.R = geoKind == 1 ? 3 : 30; // radius 3 or 30 mfp, entry at the pole
        }
        std::printf("-- %s, cosine entry --\n", geoKind == 1   ? "sphere radius 3 mfp (entry at the top, guided to the entry normal)"
                                                 : geoKind == 2 ? "sphere radius 30 mfp (entry at the top, guided to the entry normal)"
                                                                : "half space");
        std::printf("%8s %-10s %10s %10s %8s %9s %9s %8s\n", "alpha", "strategy", "R", "relStd", "segs", "ns/walk", "eff",
                    "effRel");
        for (double al : {0.5, 0.9, 0.99, 0.999}) {
            Medium m;
            m.alpha = al;
            double effPlain = 0;
            for (double pg : {0.0, 0.5, 0.75, 0.9}) {
                Guide gd;
                gd.on = pg > 0;
                gd.pg = pg;
                gd.nu = dwivediNu(al);
                gd.axis = {0, 0, 1};
                V3 p0 = geoKind ? V3{0, 0, geo.R} : V3{0, 0, 0};
                auto a = run<1>(n, 2000 + uint64_t(al * 1e4) + uint64_t(pg * 10), [&](Rng &r, std::array<Acc, 1> &acc) {
                    WalkOut o = walkMono(geo, m, p0, sampleCosine({0, 0, -1}, r), r, gd);
                    acc[0].add(o.w);
                    acc[0].segs += o.segs;
                })[0];
                const double ns = a.time * 1e9 / a.n * (gThreads > 0 ? gThreads : std::thread::hardware_concurrency());
                const double eff = 1 / (a.var() * ns);
                if (pg == 0)
                    effPlain = eff;
                char name[32];
                std::snprintf(name, sizeof name, pg == 0 ? "plain" : "dwivedi%.2f", pg);
                std::printf("%8.4f %-10s %10.5f %10.4f %8.2f %9.1f %9.3g %8.2f\n", al, name, a.mean(),
                            std::sqrt(a.var()) / a.mean(), a.segs / a.n, ns, eff, eff / effPlain);
            }
        }
    }
}

// ---------------------------------------------------------------- E3 chromatic
struct Rgb {
    double c[3];
};
// One walk carrying RGB throughput. mode 0: per-step uniform channel choice,
// 1: per-step choice by throughput, 2: per-walk hero channel with path-level
// spectral MIS (balance over the three channels' path pdfs).
static Rgb walkChroma(const Geo &geo, const double sigt[3], const double alpha[3], double g, V3 p, V3 dir, Rng &r, int mode,
                      int &segs) {
    double beta[3] = {1, 1, 1};
    double logF[3] = {0, 0, 0}, logP[3] = {0, 0, 0}; // mode 2
    const int hero = std::min(2, int(r.u() * 3));
    for (int seg = 0; seg < 100000; ++seg) {
        ++segs;
        double pk[3];
        if (mode == 0) {
            pk[0] = pk[1] = pk[2] = 1.0 / 3;
        } else if (mode == 1) {
            const double s = beta[0] + beta[1] + beta[2];
            for (int k = 0; k < 3; ++k)
                pk[k] = s > 0 ? beta[k] / s : 1.0 / 3;
        }
        int ch = hero;
        if (mode != 2) {
            const double u = r.u();
            ch = u < pk[0] ? 0 : u < pk[0] + pk[1] ? 1 : 2;
        }
        const double t = -std::log(1 - r.u()) / sigt[ch];
        V3 n;
        int face;
        const double tb = geo.exitT(p, dir, n, face);
        if (t >= tb) {
            if (mode == 2) {
                double lp[3], mx = -1e300;
                for (int k = 0; k < 3; ++k) {
                    lp[k] = logP[k] - sigt[k] * tb;
                    mx = std::max(mx, lp[k]);
                }
                double sp = 0;
                for (int k = 0; k < 3; ++k)
                    sp += std::exp(lp[k] - mx);
                const double logDen = mx + std::log(sp / 3);
                Rgb o;
                for (int k = 0; k < 3; ++k)
                    o.c[k] = std::exp(logF[k] - sigt[k] * tb - logDen);
                return o;
            }
            double q = 0;
            for (int k = 0; k < 3; ++k)
                q += pk[k] * std::exp(-sigt[k] * tb);
            Rgb o;
            for (int k = 0; k < 3; ++k)
                o.c[k] = beta[k] * std::exp(-sigt[k] * tb) / q;
            return o;
        }
        if (mode == 2) {
            for (int k = 0; k < 3; ++k) {
                logF[k] += std::log(alpha[k] * sigt[k]) - sigt[k] * t;
                logP[k] += std::log(sigt[k]) - sigt[k] * t;
            }
        } else {
            double q = 0;
            for (int k = 0; k < 3; ++k)
                q += pk[k] * sigt[k] * std::exp(-sigt[k] * t);
            for (int k = 0; k < 3; ++k)
                beta[k] *= alpha[k] * sigt[k] * std::exp(-sigt[k] * t) / q;
        }
        p = p + dir * t;
        dir = fromLocal(dir, hgSampleCos(g, r.u()), 2 * kPi * r.u());
        // Russian roulette on the largest weight.
        double mxw;
        if (mode == 2) {
            double lp[3], mx = -1e300;
            for (int k = 0; k < 3; ++k) {
                lp[k] = logP[k];
                mx = std::max(mx, lp[k]);
            }
            double sp = 0;
            for (int k = 0; k < 3; ++k)
                sp += std::exp(lp[k] - mx);
            const double logDen = mx + std::log(sp / 3);
            mxw = 0;
            for (int k = 0; k < 3; ++k)
                mxw = std::max(mxw, std::exp(logF[k] - logDen));
        } else {
            mxw = std::max({beta[0], beta[1], beta[2]});
        }
        if (mxw < 0.1) {
            const double sp = mxw / 0.1;
            if (r.u() < sp) {
                if (mode == 2)
                    for (int k = 0; k < 3; ++k)
                        logF[k] -= std::log(sp);
                else
                    for (int k = 0; k < 3; ++k)
                        beta[k] /= sp;
            } else {
                return Rgb{{0, 0, 0}};
            }
        }
    }
    return Rgb{{0, 0, 0}};
}

static void e3() {
    std::printf("\n== E3: chromatic mean free paths, one walk vs three ==\n");
    struct Case {
        const char *name;
        Geo geo;
        double mfp[3], A[3];
    };
    Geo half;
    Geo sph;
    sph.shape = Shape::Sphere;
    sph.R = 2;
    Geo slab;
    slab.shape = Shape::Slab;
    slab.d = 1;
    const Case cases[] = {
        {"half space, mfp (1,.5,.25), A .8", half, {1, 0.5, 0.25}, {0.8, 0.8, 0.8}},
        {"sphere R 2, mfp (1,.5,.25), A .8", sph, {1, 0.5, 0.25}, {0.8, 0.8, 0.8}},
        {"sphere R 2, skin mfp (1,.15,.1), A (.9,.6,.4)", sph, {1, 0.15, 0.1}, {0.9, 0.6, 0.4}},
        {"slab d 1, skin mfp (1,.15,.1), A (.9,.6,.4)", slab, {1, 0.15, 0.1}, {0.9, 0.6, 0.4}},
    };
    const long long n = N(1e6);
    const char *modes[] = {"uniform/step", "throughput/step", "hero/walk", "3 walks"};
    for (const Case &cs : cases) {
        double sigt[3], alpha[3];
        for (int k = 0; k < 3; ++k) {
            sigt[k] = 1 / cs.mfp[k];
            alpha[k] = alphaOpenPbr(cs.A[k], 0);
        }
        std::printf("-- %s (cosine entry at the top; R and T summed: total escape) --\n", cs.name);
        std::printf("%-16s %-27s %-27s %-27s %8s %8s %8s\n", "strategy", "R", "G", "B", "segs", "ns", "effRel");
        double eff0 = 0;
        for (int mode = 3; mode >= 0; --mode) {
            V3 p0 = cs.geo.shape == Shape::Sphere ? V3{0, 0, cs.geo.R} : V3{0, 0, 0};
            auto a = run<4>(n, 3000 + mode * 31 + uint64_t(cs.mfp[1] * 100), [&](Rng &r, std::array<Acc, 4> &acc) {
                int segs = 0;
                Rgb o;
                const V3 d0 = sampleCosine({0, 0, -1}, r);
                if (mode == 3) {
                    for (int k = 0; k < 3; ++k) {
                        Medium m;
                        m.sigt = sigt[k];
                        m.alpha = alpha[k];
                        WalkOut w = walkMono(cs.geo, m, p0, d0, r, Guide{});
                        o.c[k] = w.w;
                        segs += w.segs;
                    }
                } else {
                    o = walkChroma(cs.geo, sigt, alpha, 0, p0, d0, r, mode, segs);
                }
                for (int k = 0; k < 3; ++k)
                    acc[k].add(o.c[k]);
                acc[3].segs += segs;
                acc[3].n += 1;
            });
            const double ns = a[0].time * 1e9 / a[0].n * (gThreads > 0 ? gThreads : std::thread::hardware_concurrency());
            const double eff = 1 / ((a[0].var() + a[1].var() + a[2].var()) * ns);
            if (mode == 3)
                eff0 = eff;
            std::printf("%-16s", modes[mode]);
            for (int k = 0; k < 3; ++k)
                std::printf(" %.4f+-%.4f sd %-6.3f", a[k].mean(), a[k].se(), std::sqrt(a[k].var()));
            std::printf(" %8.2f %8.1f %8.2f\n", a[3].segs / a[3].n, ns, eff / eff0);
        }
    }
}

// ---------------------------------------------------------------- E4 step caps
// Half space with Karma's entry (cosine, matched boundary): segments per walk
// and the reflectance kept when walks are killed after `cap` segments.
static void e4() {
    std::printf("\n== E4: segments per walk and the energy of long walks (half space, cosine entry, matched) ==\n");
    const int caps[] = {16, 32, 64, 128, 256, 512, 1024};
    std::printf("%6s %5s %9s %-10s %8s %8s %7s %7s | R kept with a cap of", "A", "g", "alpha", "strategy", "R", "meanSeg",
                "p99", "p999");
    for (int c : caps)
        std::printf(" %7d", c);
    std::printf("\n");
    const long long n = N(4e6);
    struct Case {
        double A, g;
    };
    const Case cases[] = {{0.5, 0}, {0.8, 0}, {0.95, 0}, {0.99, 0}, {0.999, 0}, {0.8, 0.5}, {0.8, 0.9}, {0.5, 0.9}};
    for (const Case &cs : cases) {
        Medium m;
        m.g = cs.g;
        m.alpha = alphaOpenPbr(cs.A, cs.g);
        for (int guided = 0; guided < (cs.g == 0 ? 2 : 1); ++guided) {
            Guide gd;
            gd.on = guided;
            gd.pg = 0.75;
            gd.nu = dwivediNu(m.alpha);
            const int nb = 1 << 16, chunks = 256;
            std::vector<std::vector<double>> H(chunks), W(chunks);
            std::vector<Acc> acc(chunks);
            const double t0 = now();
            parallel(chunks, [&](int c) {
                Rng r(gSeed * 0x9E3779B97F4A7C15ull + 4000 + c + uint64_t(cs.A * 1e4) + guided * 77 + uint64_t(cs.g * 10),
                      c + 1);
                H[c].assign(nb + 1, 0);
                W[c].assign(nb + 1, 0);
                const long long k = n / chunks;
                for (long long i = 0; i < k; ++i) {
                    WalkOut o = walkMono(Geo{}, m, {0, 0, 0}, sampleCosine({0, 0, -1}, r), r, gd, nb);
                    acc[c].add(o.w);
                    acc[c].segs += o.segs;
                    H[c][std::min(o.segs, nb)] += 1;
                    W[c][std::min(o.segs, nb)] += o.w;
                }
            });
            (void)t0;
            std::vector<double> h(nb + 1), w(nb + 1);
            Acc a;
            for (int c = 0; c < chunks; ++c) {
                a.merge(acc[c]);
                for (int i = 0; i <= nb; ++i) {
                    h[i] += H[c][i];
                    w[i] += W[c][i];
                }
            }
            double tot = 0;
            for (double x : h)
                tot += x;
            double cum = 0;
            int p99 = -1, p999 = -1;
            for (int i = 0; i <= nb; ++i) {
                cum += h[i];
                if (p99 < 0 && cum >= 0.99 * tot)
                    p99 = i;
                if (p999 < 0 && cum >= 0.999 * tot)
                    p999 = i;
            }
            std::printf("%6.3f %5.2f %9.6f %-10s %8.4f %8.2f %7d %7d |", cs.A, cs.g, m.alpha, guided ? "dwivedi.75" : "plain",
                        a.mean(), a.segs / a.n, p99, p999);
            for (int cap : caps) {
                double s = 0;
                for (int i = 0; i <= cap; ++i)
                    s += w[i];
                std::printf(" %7.4f", s / a.n);
            }
            std::printf("\n");
        }
    }
}

// ---------------------------------------------------------------- E5 sphere furnace
static void e5() {
    std::printf("\n== E5: sphere (radius R) in a white furnace, seen by an orthographic camera (disc r < 0.95 R) ==\n");
    std::printf("Pixel value = escape probability of the walk started where the camera ray enters (matched boundary).\n");
    const long long n = N(1e6);
    std::printf("%8s %6s %9s | %-18s %-18s\n", "R/mfp", "A", "alpha", "straight entry", "cosine entry");
    for (double Rm : {1000.0, 33.33, 10.0, 3.333, 1.0, 0.3333})
        for (double A : {0.2, 0.5, 0.8}) {
            Geo geo;
            geo.shape = Shape::Sphere;
            geo.R = Rm;
            Medium m;
            m.alpha = alphaOpenPbr(A, 0);
            double res[2];
            double se[2];
            for (int e = 0; e < 2; ++e) {
                auto a = run<1>(n, 5000 + uint64_t(Rm * 10) + uint64_t(A * 100) + e, [&](Rng &r, std::array<Acc, 1> &acc) {
                    // uniform point on the disc of radius 0.95 R, seen from +z
                    const double rr = 0.95 * Rm * std::sqrt(r.u()), ph = 2 * kPi * r.u();
                    const double x = rr * std::cos(ph), y = rr * std::sin(ph), z = std::sqrt(Rm * Rm - x * x - y * y);
                    const V3 p{x, y, z}, nrm = normalize(p);
                    const V3 d = e == 0 ? V3{0, 0, -1} : sampleCosine(-nrm, r);
                    WalkOut o = walkMono(geo, m, p * (1 - 1e-12), d, r, Guide{});
                    acc[0].add(o.w);
                })[0];
                res[e] = a.mean();
                se[e] = a.se();
            }
            std::printf("%8.4g %6.2f %9.6f | %.4f+-%.4f     %.4f+-%.4f\n", Rm, A, m.alpha, res[0], se[0], res[1], se[1]);
        }
    // chromatic radius of scene f_chroma: radius 1, mfp 0.3 x (1, .5, .25)
    std::printf("f_chroma (R 1, mfp .3 x (1,.5,.25), A .8): ");
    for (double mfp : {0.3, 0.15, 0.075}) {
        Geo geo;
        geo.shape = Shape::Sphere;
        geo.R = 1 / mfp;
        Medium m;
        m.alpha = alphaOpenPbr(0.8, 0);
        for (int e = 0; e < 2; ++e) {
            auto a = run<1>(n, 5600 + uint64_t(mfp * 1000) + e, [&](Rng &r, std::array<Acc, 1> &acc) {
                const double rr = 0.95 * geo.R * std::sqrt(r.u()), ph = 2 * kPi * r.u();
                const double x = rr * std::cos(ph), y = rr * std::sin(ph), z = std::sqrt(geo.R * geo.R - x * x - y * y);
                const V3 p{x, y, z}, nrm = normalize(p);
                const V3 d = e == 0 ? V3{0, 0, -1} : sampleCosine(-nrm, r);
                acc[0].add(walkMono(geo, m, p * (1 - 1e-12), d, r, Guide{}).w);
            })[0];
            std::printf("%s %.4f ", e == 0 ? "straight" : "cosine", a.mean());
        }
        std::printf("| ");
    }
    std::printf("\n");
}

// ---------------------------------------------------------------- E6 slabs
static void e6() {
    std::printf("\n== E6: finite slabs at normal incidence (unit irradiance) and the half space under a headlight ==\n");
    std::printf("T, R: diffuse transmittance / reflectance (ballistic light excluded); Lt0: transmitted radiance along the\n");
    std::printf("normal (exits within 10 deg; Lambert would give T/pi); bal: ballistic fraction.\n");
    const long long n = N(2e6);
    const double cosC = std::cos(10 * kPi / 180), omegaCos = kPi * (1 - cosC * cosC);
    std::printf("%6s %6s %4s %-9s | %8s %8s %8s %8s %8s\n", "d/mfp", "A", "g", "entry", "T", "R", "Lt0", "T/pi", "bal");
    struct Row {
        double d, A, g;
        int entry; // 0 straight normal, 1 cosine, 2 straight at 60 deg
    };
    std::vector<Row> rows;
    for (double d : {0.5, 1.0, 2.0, 4.0, 8.0})
        for (double A : {0.5, 0.8, 0.95})
            for (int e = 0; e < 2; ++e)
                rows.push_back({d, A, 0, e});
    for (double g : {0.5, -0.5})
        for (double A : {0.5, 0.8, 0.95})
            for (int e = 0; e < 2; ++e)
                rows.push_back({2, A, g, e});
    for (double A : {0.5, 0.8, 0.95})
        rows.push_back({2, A, 0, 2});
    // slab_chroma: d 0.1 with mfp .1 x (1,.5,.25) -> d/mfp 1, 2, 4 at A .8 (covered above)
    for (const Row &rw : rows) {
        Geo geo;
        geo.shape = Shape::Slab;
        geo.d = rw.d;
        Medium m;
        m.g = rw.g;
        m.alpha = alphaOpenPbr(rw.A, rw.g);
        auto a = run<4>(n, 6000 + uint64_t(rw.d * 100 + rw.A * 10 + rw.g * 3 + rw.entry), [&](Rng &r, std::array<Acc, 4> &acc) {
            V3 d{0, 0, -1};
            if (rw.entry == 1)
                d = sampleCosine({0, 0, -1}, r);
            else if (rw.entry == 2)
                d = {std::sin(kPi / 3), 0, -0.5};
            WalkOut o = walkMono(geo, m, {0, 0, 0}, d, r, Guide{});
            const bool T = o.face == 1 && !o.ballistic, Rf = o.face == 0;
            acc[0].add(T ? o.w : 0);
            acc[1].add(Rf ? o.w : 0);
            acc[2].add(T && -o.dir.z > cosC ? o.w : 0);
            acc[3].add(o.face == 1 && o.ballistic ? o.w : 0);
        });
        const char *en = rw.entry == 0 ? "straight" : rw.entry == 1 ? "cosine" : "str. 60deg";
        std::printf("%6.2f %6.2f %4.1f %-9s | %8.5f %8.5f %8.5f %8.5f %8.5f\n", rw.d, rw.A, rw.g, en, a[0].mean(), a[1].mean(),
                    a[2].mean() / omegaCos, a[0].mean() / kPi, a[3].mean());
    }
    // Half space lit from the viewing direction at incidence cos mu: the
    // reflected radiance towards the light (= camera) for unit irradiance at
    // normal incidence (so mu at the point); Lambert exit: mu R / pi.
    std::printf("-- half space, light and camera along the same direction (scene head), A = .2 / .5 / .8 --\n");
    std::printf("%5s | %-30s | %-30s\n", "mu", "straight entry: L(retro), mu R/pi", "cosine entry: mu R/pi");
    for (double mu : {1.0, 0.8, 0.6, 0.4, 0.2}) {
        std::printf("%5.2f |", mu);
        for (int e = 0; e < 2; ++e) {
            for (double A : {0.2, 0.5, 0.8}) {
                Medium m;
                m.alpha = alphaOpenPbr(A, 0);
                const double st = std::sqrt(1 - mu * mu);
                const V3 in{st, 0, -mu};
                const double cosCone = std::cos(10 * kPi / 180), omega = 2 * kPi * (1 - cosCone);
                auto a = run<2>(n / 2, 6500 + uint64_t(mu * 100) + uint64_t(A * 10) + e, [&](Rng &r, std::array<Acc, 2> &acc) {
                    const V3 d = e == 0 ? in : sampleCosine({0, 0, -1}, r);
                    WalkOut o = walkMono(Geo{}, m, {0, 0, 0}, d, r, Guide{});
                    acc[0].add(o.w);
                    acc[1].add(o.face == 0 && dot(o.dir, -in) > cosCone ? o.w : 0);
                });
                if (e == 0)
                    std::printf(" %.4f/%.4f", a[1].mean() / omega, mu * a[0].mean() / kPi);
                else
                    std::printf(" %.4f", mu * a[0].mean() / kPi);
            }
            std::printf(" |");
        }
        std::printf("\n");
    }
}

int main(int argc, char **argv) {
    std::string which = argc > 1 ? argv[1] : "all";
    gThreads = argc > 2 ? std::atoi(argv[2]) : 0;
    gScale = argc > 3 ? std::atof(argv[3]) : 1.0;
    gSeed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1;
    std::printf("sss_lab %s threads %d scale %g seed %llu\n", which.c_str(),
                gThreads > 0 ? gThreads : int(std::thread::hardware_concurrency()), gScale, (unsigned long long)gSeed);
    const double t0 = now();
    if (which == "all" || which == "e1")
        e1();
    if (which == "all" || which == "e2")
        e2();
    if (which == "all" || which == "e3")
        e3();
    if (which == "all" || which == "e4")
        e4();
    if (which == "all" || which == "e5")
        e5();
    if (which == "all" || which == "e6")
        e6();
    std::printf("\ntotal %.1f s\n", now() - t0);
    return 0;
}
