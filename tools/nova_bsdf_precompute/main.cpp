// nova_bsdf_precompute: integrates the energy tables of core/bsdf/energy.h
// with the lobes' own sampling code and writes them as C++ source
// (core/bsdf/tables/bsdf_tables.cpp). Monte Carlo with Nova's Owen-scrambled
// Sobol sampler, accumulated in double, one TBB task per table cell.
//
//   nova_bsdf_precompute -o core/bsdf/tables/bsdf_tables.cpp [--scale S] [--hash-inputs file...]
//
// --scale S divides every sample count by 2^S (quick test runs; never commit
// such tables). --hash-inputs lists the files the tables depend on; their
// FNV-1a hash goes into the output so a stale table can be recognized. Run
// through the CMake target nova_bsdf_tables, which passes both.

#include "core/bsdf/conductor.h"
#include "core/bsdf/dielectric.h"
#include "core/bsdf/energy.h"
#include "core/bsdf/sheen.h"
#include "core/math/sobol.h"

#include <tbb/parallel_for.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace nova;

namespace {

int gScale = 0;

uint64_t samples(int log2) { return uint64_t(1) << (log2 > gScale ? log2 - gScale : 0); }

Vec3f viewDirection(float mu) { return {std::sqrt(std::fmax(0.0f, 1.0f - mu * mu)), 0.0f, mu}; }

// One lobe sample's weight f |cos| / pdf (the event weight for delta samples).
float weight(const LobeSample &s) {
    if (s.delta)
        return s.fcos.x;
    return s.pdf > 0.0f ? s.fcos.x / s.pdf : 0.0f;
}

ConductorLobe whiteGgx(float r) {
    ConductorLobe lobe{}; // value-initialized: film off, xyzToRgb null, c2 0
    lobe.ggx = GgxDistribution::make(r * r, r * r);
    lobe.fresnel = FresnelModel::One;
    lobe.c0 = lobe.c1 = spectrum(1.0f);
    lobe.exponent = 5.0f;
    return lobe;
}

DielectricLobe dielectric(float r, float eta, uint8_t mode) {
    DielectricLobe lobe{}; // value-initialized: film off, xyzToRgb null
    lobe.ggx = GgxDistribution::make(r * r, r * r);
    lobe.eta = eta;
    lobe.tintR = lobe.tintT = spectrum(1.0f);
    lobe.mode = mode;
    return lobe;
}

// E(mu) of GGX with F = 1.
double ggxE(float r, float mu, uint32_t seed, uint64_t n) {
    const ConductorLobe lobe = whiteGgx(r);
    const Vec3f wo = viewDirection(mu);
    double sum = 0.0;
    for (uint64_t i = 0; i < n; ++i) {
        const Vec3f u = Sampler{seed, uint32_t(i)}.get3D(0);
        LobeSample s;
        if (lobe.sample(wo, {u.x, u.y}, s))
            sum += weight(s);
    }
    return sum / double(n);
}

// 2 int E mu dmu: mu drawn with density 2 mu.
double ggxEavg(float r, uint32_t seed, uint64_t n) {
    const ConductorLobe lobe = whiteGgx(r);
    double sum = 0.0;
    for (uint64_t i = 0; i < n; ++i) {
        const Vec3f u = Sampler{seed, uint32_t(i)}.get3D(0);
        const float mu = std::fmax(std::sqrt(u.z), kLutMinMu);
        LobeSample s;
        if (lobe.sample(viewDirection(mu), {u.x, u.y}, s))
            sum += weight(s);
    }
    return sum / double(n);
}

// A, B, C: with s = (1 - mu_h)^5 the albedo of F0 (1 - s) + F90 s - b mu_h (1 - mu_h)^6
// is F0 A + F90 B - b C (mu_h = wo . m, the microfacet the sample reflects on).
void ggxABC(float r, float mu, uint32_t seed, uint64_t n, double abc[3]) {
    const ConductorLobe lobe = whiteGgx(r);
    const Vec3f wo = viewDirection(mu);
    abc[0] = abc[1] = abc[2] = 0.0;
    for (uint64_t i = 0; i < n; ++i) {
        const Vec3f u = Sampler{seed, uint32_t(i)}.get3D(0);
        LobeSample s;
        if (!lobe.sample(wo, {u.x, u.y}, s))
            continue;
        const float w = weight(s);
        const float muH = s.delta ? wo.z : dot(wo, normalize(wo + s.wi));
        const double sh = pow5(1.0f - muH);
        abc[0] += w * (1.0 - sh);
        abc[1] += w * sh;
        abc[2] += w * double(muH) * pow6(1.0f - muH);
    }
    for (int c = 0; c < 3; ++c)
        abc[c] /= double(n);
}

// E_R + E_T in energy units (transmission without the 1 / eta^2 of radiance)
// or E_R alone.
double dielectricE(float r, float eta, float mu, uint8_t mode, uint32_t seed, uint64_t n) {
    const DielectricLobe lobe = dielectric(r, eta, mode);
    const Vec3f wo = viewDirection(mu);
    double sum = 0.0;
    for (uint64_t i = 0; i < n; ++i) {
        const Vec3f u = Sampler{seed, uint32_t(i)}.get3D(0);
        LobeSample s;
        if (lobe.sample(wo, {u.x, u.y}, u.z, s))
            sum += double(weight(s)) * (s.transmit ? double(eta) * eta : 1.0);
    }
    return sum / double(n);
}

// Albedo of a clear thin-walled sheet.
double thinE(float r, float eta, float mu, uint32_t seed, uint64_t n) {
    const float alpha = r * r, alphaT = ThinDielectricLobe::transmissionAlpha(alpha, eta);
    const ThinDielectricLobe lobe{GgxDistribution::make(alpha, alpha), GgxDistribution::make(alphaT, alphaT), eta,
                                  spectrum(1.0f), spectrum(1.0f)};
    const Vec3f wo = viewDirection(mu);
    double sum = 0.0;
    for (uint64_t i = 0; i < n; ++i) {
        const Vec3f u = Sampler{seed, uint32_t(i)}.get3D(0);
        LobeSample s;
        if (lobe.sample(wo, {u.x, u.y}, u.z, s))
            sum += weight(s);
    }
    return sum / double(n);
}

// Albedo of the white Charlie sheen lobe.
double charlieE(float r, float mu, uint32_t seed, uint64_t n) {
    const CharlieLobe lobe{spectrum(1.0f), r};
    const Vec3f wo = viewDirection(mu);
    double sum = 0.0;
    for (uint64_t i = 0; i < n; ++i) {
        const Vec3f u = Sampler{seed, uint32_t(i)}.get3D(0);
        LobeSample s;
        if (lobe.sample(wo, {u.x, u.y}, s))
            sum += weight(s);
    }
    return sum / double(n);
}

// 2 int F mu dmu by the midpoint rule.
double fresnelAvg(float eta) {
    const int n = 200000;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const float mu = (float(i) + 0.5f) / float(n);
        sum += 2.0 * double(fresnelDielectric(mu, eta)) * mu;
    }
    return sum / n;
}

uint32_t cellSeed(uint32_t table, uint32_t index) { return hashCombine(mix32(table * 0x9e3779b9u + 17u), index); }

uint64_t fnv1a(const std::vector<std::string> &files) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const std::string &f : files) {
        std::ifstream in(f, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const char c : data) {
            if (c == '\r')
                continue; // line endings must not change the hash
            h = (h ^ uint8_t(c)) * 0x100000001b3ull;
        }
    }
    return h;
}

void writeArray(std::FILE *f, const char *name, const std::vector<float> &v) {
    std::fprintf(f, "const float %s[%zu] = {\n", name, v.size());
    for (size_t i = 0; i < v.size(); ++i)
        std::fprintf(f, "%s%.9g,%s", i % 8 == 0 ? "    " : "", double(v[i]), i % 8 == 7 || i + 1 == v.size() ? "\n" : " ");
    std::fprintf(f, "};\n\n");
}

template <class F> void forCells(size_t count, F &&cell) {
    tbb::parallel_for(size_t(0), count, [&](size_t i) { cell(i); });
}

} // namespace

int main(int argc, char **argv) {
    std::string out;
    std::vector<std::string> hashInputs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc)
            out = argv[++i];
        else if (a == "--scale" && i + 1 < argc)
            gScale = std::stoi(argv[++i]);
        else if (a == "--hash-inputs")
            while (i + 1 < argc && argv[i + 1][0] != '-')
                hashInputs.push_back(argv[++i]);
        else {
            std::fprintf(stderr, "usage: nova_bsdf_precompute -o out.cpp [--scale S] [--hash-inputs file...]\n");
            return 2;
        }
    }
    if (out.empty()) {
        std::fprintf(stderr, "nova_bsdf_precompute: -o is required\n");
        return 2;
    }
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };

    const int logE = 22, logEavg = 24, logABC = 22, logDiel = 18;
    std::vector<float> E(kLutRough * kLutMu), Eavg(kLutRough), ABC(kLutRough * kLutMu * 3), sheen(kLutRough * kLutMu);
    forCells(E.size(), [&](size_t c) {
        const int ir = int(c) / kLutMu, iy = int(c) % kLutMu;
        E[c] = float(ggxE(lutRoughAt(ir), lutMuAt(iy), cellSeed(1, uint32_t(c)), samples(logE)));
        double abc[3];
        ggxABC(lutRoughAt(ir), lutMuAt(iy), cellSeed(3, uint32_t(c)), samples(logABC), abc);
        for (int k = 0; k < 3; ++k)
            ABC[c * 3 + k] = float(abc[k]);
        sheen[c] = float(charlieE(float(ir) / float(kLutRough - 1), lutMuAt(iy), cellSeed(7, uint32_t(c)), samples(logE)));
    });
    forCells(Eavg.size(), [&](size_t c) {
        Eavg[c] = float(ggxEavg(lutRoughAt(int(c)), cellSeed(2, uint32_t(c)), samples(logEavg)));
    });
    std::fprintf(stderr, "GGX tables: %.1f s\n", elapsed());

    const size_t n3 = size_t(kLutEta) * kLutRough * kLutMu;
    std::vector<float> RT(n3), RTinv(n3), R(n3), thin(n3);
    forCells(n3, [&](size_t c) {
        const int iz = int(c) / (kLutRough * kLutMu), ir = int(c) / kLutMu % kLutRough, iy = int(c) % kLutMu;
        const float eta = lutEtaAt(iz, kLutEta), r = lutRoughAt(ir), mu = lutMuAt(iy);
        const uint64_t n = samples(logDiel);
        RT[c] = float(dielectricE(r, eta, mu, DielectricLobe::kBoth, cellSeed(4, uint32_t(c)), n));
        RTinv[c] = float(dielectricE(r, 1.0f / eta, mu, DielectricLobe::kBoth, cellSeed(5, uint32_t(c)), n));
        R[c] = float(dielectricE(r, eta, mu, DielectricLobe::kReflect, cellSeed(6, uint32_t(c)), n));
        thin[c] = float(thinE(r, eta, mu, cellSeed(8, uint32_t(c)), n));
    });
    std::fprintf(stderr, "dielectric tables: %.1f s\n", elapsed());

    std::vector<float> Favg(kLutFresnelAvg);
    for (int k = 0; k < kLutFresnelAvg; ++k)
        Favg[k] = float(fresnelAvg(lutEtaAt(k, kLutFresnelAvg)));

    std::FILE *f = std::fopen(out.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "nova_bsdf_precompute: cannot write %s\n", out.c_str());
        return 1;
    }
    std::fprintf(f,
                 "// Generated by tools/nova_bsdf_precompute; do not edit. Regenerate with the CMake\n"
                 "// target nova_bsdf_tables after changing the lobes (core/bsdf).\n"
                 "// Input hash (FNV-1a of the lobe headers and the generator): %016llx\n"
                 "// Samples per cell: ggxE and charlieE 2^%d, ggxEavg 2^%d, ggxABC 2^%d, dielectric 2^%d (scale %d)\n\n"
                 "#include \"core/bsdf/energy.h\"\n\nnamespace nova {\n\nnamespace {\n\n",
                 (unsigned long long)fnv1a(hashInputs), logE - gScale, logEavg - gScale, logABC - gScale,
                 logDiel - gScale, gScale);
    writeArray(f, "kGgxE", E);
    writeArray(f, "kGgxEavg", Eavg);
    writeArray(f, "kGgxABC", ABC);
    writeArray(f, "kDielRT", RT);
    writeArray(f, "kDielRTinv", RTinv);
    writeArray(f, "kDielR", R);
    writeArray(f, "kFresnelAvg", Favg);
    writeArray(f, "kCharlieE", sheen);
    writeArray(f, "kThinRT", thin);
    std::fprintf(f, "} // namespace\n\n"
                    "const BsdfTables &bsdfTables() {\n"
                    "    static const BsdfTables tables{kGgxE,       kGgxEavg,  kGgxABC, kDielRT,\n"
                    "                                   kDielRTinv,  kDielR,    kFresnelAvg,\n"
                    "                                   kCharlieE,   kThinRT};\n"
                    "    return tables;\n"
                    "}\n\n} // namespace nova\n");
    std::fclose(f);
    std::fprintf(stderr, "wrote %s in %.1f s\n", out.c_str(), elapsed());
    return 0;
}
