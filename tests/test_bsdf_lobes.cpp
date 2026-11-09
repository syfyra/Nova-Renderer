// M3.1 lobe library (docs/design/M3.md section 7): GGX, Fresnel, dielectric,
// thin-walled, conductor and Lambert lobes, checked statistically with
// tests/bsdf_stats.h and against exact identities, plus the energy tables
// (core/bsdf/energy.h): anchors from docs/research/m3/microfacet.md, white
// furnaces with compensation, and agreement with the current lobe code.

#include "bsdf_stats.h"

#include "core/bsdf/conductor.h"
#include "core/bsdf/dielectric.h"
#include "core/bsdf/diffuse.h"
#include "core/bsdf/energy.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace nova;
using nova::test::Chi2Result;
using nova::test::LobeUnderTest;
using nova::test::Pcg32;
using nova::test::V3;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

Vec3f toF(V3 v) { return {float(v.x), float(v.y), float(v.z)}; }
V3 toD(Vec3f v) { return {v.x, v.y, v.z}; }

Vec3f direction(float mu, float phi = 0.3f) {
    const float s = std::sqrt(std::fmax(0.0f, 1.0f - mu * mu));
    return {s * std::cos(phi), s * std::sin(phi), mu};
}

// Any lobe as eval(wo, wi, pdf) -> fcos and sample(wo, u, s); one channel.
using EvalFn = std::function<Spectrum(Vec3f, Vec3f, float &)>;
using SampleFn = std::function<bool(Vec3f, Vec3f, LobeSample &)>;

LobeUnderTest adapt(EvalFn eval, SampleFn sample) {
    LobeUnderTest l;
    l.sample = [eval, sample](V3 wo, V3 u, V3 &wi, double &pdf, double &w, bool &delta) {
        LobeSample s;
        if (!sample(toF(wo), toF(u), s))
            return false;
        wi = toD(s.wi);
        delta = s.delta;
        pdf = s.pdf;
        w = s.delta ? s.fcos.x : s.fcos.x / s.pdf;
        return true;
    };
    l.pdf = [eval](V3 wo, V3 wi) {
        float pdf = 0.0f;
        eval(toF(wo), toF(wi), pdf);
        return double(pdf);
    };
    l.evalCos = [eval](V3 wo, V3 wi) {
        float pdf = 0.0f;
        return double(eval(toF(wo), toF(wi), pdf).x);
    };
    return l;
}

ConductorLobe conductor(float ax, float ay, FresnelModel model = FresnelModel::One, Spectrum c0 = spectrum(1.0f),
                        Spectrum c1 = spectrum(1.0f)) {
    return {GgxDistribution::make(ax, ay), model, c0, c1, 5.0f};
}
DielectricLobe dielectric(float alpha, float eta, uint8_t mode) {
    return {GgxDistribution::make(alpha, alpha), eta, spectrum(1.0f), spectrum(1.0f), mode};
}
ThinDielectricLobe thinDielectric(float alpha, float eta, Spectrum tintT = spectrum(1.0f)) {
    return {GgxDistribution::make(alpha, alpha),
            GgxDistribution::make(ThinDielectricLobe::transmissionAlpha(alpha, eta),
                                  ThinDielectricLobe::transmissionAlpha(alpha, eta)),
            eta, spectrum(1.0f), tintT};
}

LobeUnderTest adapt(const ConductorLobe &l) {
    return adapt([l](Vec3f wo, Vec3f wi, float &pdf) { return l.eval(wo, wi, pdf); },
                 [l](Vec3f wo, Vec3f u, LobeSample &s) { return l.sample(wo, {u.x, u.y}, s); });
}
LobeUnderTest adapt(const DielectricLobe &l) {
    return adapt([l](Vec3f wo, Vec3f wi, float &pdf) { return l.eval(wo, wi, pdf); },
                 [l](Vec3f wo, Vec3f u, LobeSample &s) { return l.sample(wo, {u.x, u.y}, u.z, s); });
}
LobeUnderTest adapt(const ThinDielectricLobe &l) {
    return adapt([l](Vec3f wo, Vec3f wi, float &pdf) { return l.eval(wo, wi, pdf); },
                 [l](Vec3f wo, Vec3f u, LobeSample &s) { return l.sample(wo, {u.x, u.y}, u.z, s); });
}
LobeUnderTest adapt(const LambertLobe &l) {
    return adapt([l](Vec3f wo, Vec3f wi, float &pdf) { return l.eval(wo, wi, pdf); },
                 [l](Vec3f wo, Vec3f u, LobeSample &s) { return l.sample(wo, {u.x, u.y}, s); });
}

// E(wo): mean sample weight, in energy units (transmission times etap^2 when
// `etaForEnergy` is set: the lobes return radiance).
double albedo(const LobeUnderTest &lobe, Vec3f wo, uint32_t n, uint64_t seed, double etapSquared = 1.0) {
    Pcg32 rng(seed);
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        V3 wi{}, u{rng.uniform(), rng.uniform(), rng.uniform()};
        double pdf = 0, w = 0;
        bool delta = false;
        if (lobe.sample(toD(wo), u, wi, pdf, w, delta))
            sum += w * ((wi.z < 0) != (wo.z < 0) ? etapSquared : 1.0);
    }
    return sum / n;
}

void runChi2(const char *name, const LobeUnderTest &lobe, std::initializer_list<float> mus, int runs) {
    Pcg32 rng(1234);
    for (const float mu : mus) {
        INFO(name << ", cos(wo) " << mu);
        const Chi2Result r = nova::test::chi2Test(lobe, toD(direction(mu)), rng, 1 << 18, 48, 5.0, 0.01, runs);
        INFO(r.message << " (p " << r.pValue << ", dof " << r.dof << ", pdf mass " << r.pdfMass << ")");
        CHECK(r.pass);
        CHECK(r.pdfMass <= 1.0 + 2e-3);
    }
}

} // namespace

TEST_CASE("GGX normals and visible normals are normalized", "[bsdf][ggx]") {
    const int nTheta = 2048, nPhi = 512;
    for (const auto &[ax, ay] : {std::pair{0.05f, 0.05f}, std::pair{0.3f, 0.3f}, std::pair{0.6f, 0.15f}, std::pair{1.0f, 1.0f}}) {
        const GgxDistribution g = GgxDistribution::make(ax, ay);
        INFO("alpha " << ax << " / " << ay);
        double projected = 0.0, visible = 0.0;
        const Vec3f wo = direction(0.4f, 0.8f);
        for (int i = 0; i < nTheta; ++i) {
            const double theta = (i + 0.5) / nTheta * (kPi / 2), dTheta = (kPi / 2) / nTheta;
            for (int j = 0; j < nPhi; ++j) {
                const double phi = (j + 0.5) / nPhi * 2 * kPi, dPhi = 2 * kPi / nPhi;
                const Vec3f m{float(std::sin(theta) * std::cos(phi)), float(std::sin(theta) * std::sin(phi)),
                              float(std::cos(theta))};
                const double dw = std::sin(theta) * dTheta * dPhi;
                projected += g.D(m) * m.z * dw;
                visible += g.pdfVisible(wo, m) * dw;
            }
        }
        CHECK_THAT(projected, WithinAbs(1.0, 2e-3));
        CHECK_THAT(visible, WithinAbs(1.0, 2e-3));
    }
}

TEST_CASE("Lobes sample their pdfs (chi-square)", "[bsdf][chi2]") {
    runChi2("conductor 0.05", adapt(conductor(0.05f, 0.05f)), {0.2f, 0.9f}, 2);
    runChi2("conductor 0.3", adapt(conductor(0.3f, 0.3f)), {0.1f, 0.5f, 0.9f}, 3);
    runChi2("conductor anisotropic 0.6 / 0.15", adapt(conductor(0.6f, 0.15f)), {0.3f, 0.8f}, 2);
    runChi2("conductor 0.8 (bounded cap)", adapt(conductor(0.8f, 0.8f)), {0.95f}, 1);
    for (const float eta : {1.5f, 1.0f / 1.5f})
        for (const float alpha : {0.05f, 0.3f, 0.6f}) {
            const std::string name = "glass eta " + std::to_string(eta) + " alpha " + std::to_string(alpha);
            runChi2(name.c_str(), adapt(dielectric(alpha, eta, DielectricLobe::kBoth)), {0.1f, 0.5f, 0.9f}, 3);
        }
    // Between nested media (docs/design/M11-P9b.md): the media's ratio bends
    // the transmission, the surface's own index reflects.
    for (const float eta : {1.5f, 1.0f / 1.5f}) {
        DielectricLobe nested = dielectric(0.3f, eta, DielectricLobe::kBoth);
        nested.etaMedia = eta >= 1.0f ? 1.5f / 1.33f : 2.0f / 1.5f;
        runChi2(eta >= 1.0f ? "glass from water 0.3" : "glass into a denser medium 0.3", adapt(nested),
                {0.1f, 0.5f, 0.9f}, 3);
    }
    runChi2("dielectric reflection 0.3", adapt(dielectric(0.3f, 1.5f, DielectricLobe::kReflect)), {0.2f, 0.8f}, 2);
    runChi2("dielectric transmission 0.3", adapt(dielectric(0.3f, 1.5f, DielectricLobe::kTransmit)), {0.2f, 0.8f}, 2);
    runChi2("thin-walled 0.3", adapt(thinDielectric(0.3f, 1.5f)), {0.2f, 0.8f}, 2);
    runChi2("Lambert", adapt(LambertLobe{spectrum(1.0f), false}), {0.5f}, 1);
    runChi2("translucent", adapt(LambertLobe{spectrum(1.0f), true}), {0.5f}, 1);
}

TEST_CASE("Sampled values equal evaluated values", "[bsdf]") {
    const std::vector<std::pair<std::string, LobeUnderTest>> lobes = {
        {"conductor", adapt(conductor(0.4f, 0.1f, FresnelModel::F82Tint, {0.9f, 0.6f, 0.3f}, {0.7f, 0.8f, 0.9f}))},
        {"glass", adapt(dielectric(0.3f, 1.5f, DielectricLobe::kBoth))},
        {"glass inside", adapt(dielectric(0.3f, 1.0f / 1.33f, DielectricLobe::kBoth))},
        {"thin", adapt(thinDielectric(0.25f, 1.45f))},
    };
    for (const auto &[name, lobe] : lobes) {
        Pcg32 rng(99);
        int checked = 0;
        for (int i = 0; i < 20000; ++i) {
            const float mu = float(rng.uniform()) * 2.0f - 1.0f;
            const Vec3f wo = direction(std::fabs(mu) < 0.02f ? 0.5f : mu, float(rng.uniform() * 6.28));
            V3 wi{}, u{rng.uniform(), rng.uniform(), rng.uniform()};
            double pdf = 0, w = 0;
            bool delta = false;
            if (!lobe.sample(toD(wo), u, wi, pdf, w, delta) || delta)
                continue;
            const double pdfEval = lobe.pdf(toD(wo), wi), fEval = lobe.evalCos(toD(wo), wi);
            INFO(name << ": wo " << wo.x << " " << wo.y << " " << wo.z);
            CHECK_THAT(pdfEval, WithinRel(pdf, 1e-4));
            CHECK_THAT(fEval / pdfEval, WithinRel(w, 1e-4));
            ++checked;
        }
        CHECK(checked > 5000); // the metal rejects wo below the surface
    }
}

TEST_CASE("Microfacet reflection is reciprocal, transmission scales by the index", "[bsdf]") {
    Pcg32 rng(5);
    const ConductorLobe metal = conductor(0.35f, 0.2f);
    const DielectricLobe glass = dielectric(0.3f, 1.5f, DielectricLobe::kBoth);
    for (int i = 0; i < 200; ++i) {
        const Vec3f a = direction(0.05f + 0.9f * float(rng.uniform()), float(rng.uniform() * 6.28));
        const Vec3f b = direction(0.05f + 0.9f * float(rng.uniform()), float(rng.uniform() * 6.28));
        float p;
        // f = fcos / |cos wi|
        const float fab = metal.eval(a, b, p).x / b.z, fba = metal.eval(b, a, p).x / a.z;
        CHECK_THAT(fab, WithinRel(fba, 1e-4f));
        const Vec3f below = mirrorZ(b);
        const float tab = glass.eval(a, below, p).x / std::fabs(below.z); // outside -> inside
        const float tba = glass.eval(below, a, p).x / a.z;                // inside -> outside
        if (tab > 1e-6f && tba > 1e-6f)
            CHECK_THAT(tab / tba, WithinRel(1.0f / (1.5f * 1.5f), 1e-3f)); // (n_o / n_i)^2
    }
}

TEST_CASE("Fresnel identities", "[bsdf][fresnel]") {
    for (const float eta : {1.33f, 1.5f, 2.4f})
        CHECK_THAT(fresnelDielectric(1.0f, eta), WithinAbs(sqr((eta - 1.0f) / (eta + 1.0f)), 1e-6));
    const float critical = std::sqrt(1.0f - sqr(1.0f / 1.5f)); // cos of the critical angle inside
    CHECK(fresnelDielectric(critical * 0.99f, 1.0f / 1.5f) == 1.0f);
    CHECK(fresnelDielectric(critical * 1.01f, 1.0f / 1.5f) < 1.0f);
    for (const float mu : {0.05f, 0.3f, 0.7f, 1.0f})
        CHECK_THAT(fresnelConductor(mu, 1.5f, 0.0f), WithinAbs(fresnelDielectric(mu, 1.5f), 1e-5));
    // F82 tint: F(1/7) = tint * Schlick(1/7); the average against quadrature.
    for (const float tint : {0.0f, 0.4f, 0.9f}) {
        CHECK_THAT(fresnelF82Tint(kF82Mu, 0.6f, tint, 1.0f, 5.0f),
                   WithinAbs(tint * fresnelSchlick(kF82Mu, 0.6f, 1.0f, 5.0f), 1e-5));
        double avg = 0.0;
        for (int i = 0; i < 100000; ++i) {
            const float mu = (i + 0.5f) / 100000.0f;
            avg += 2.0 * fresnelF82Tint(mu, 0.6f, tint, 1.0f, 5.0f) * mu / 100000.0;
        }
        CHECK_THAT(fresnelF82TintAvg(0.6f, tint, 1.0f, 5.0f), WithinAbs(avg, 2e-4));
    }
    for (int i = 0; i <= 100; ++i) // clamped at 0 even for a zero tint
        CHECK(fresnelF82Tint(float(i) / 100.0f, 0.0f, 0.0f, 1.0f, 5.0f) >= 0.0f);
    // The artistic IOR reproduces the reflectivity at normal incidence.
    for (const float r : {0.04f, 0.5f, 0.95f}) {
        float n, k;
        artisticIor(r, 0.6f, n, k);
        CHECK_THAT(fresnelConductor(1.0f, n, k), WithinAbs(r, 1e-4));
    }
    // OpenPBR's specular_weight scales the normal-incidence reflectance.
    CHECK_THAT(iorToF0(openPbrModulatedEta(1.5f, 0.5f)), WithinAbs(0.5f * iorToF0(1.5f), 1e-6));
    CHECK(openPbrModulatedEta(0.8f, 1.0f) < 1.0f);
    // The conductor average from its F82 fit against quadrature (the fit
    // itself is off by 4e-3 here; it only feeds the multiple-scattering term).
    float n, k;
    artisticIor(0.9f, 0.5f, n, k);
    double avg = 0.0;
    for (int i = 0; i < 100000; ++i) {
        const float mu = (i + 0.5f) / 100000.0f;
        avg += 2.0 * fresnelConductor(mu, n, k) * mu / 100000.0;
    }
    CHECK_THAT(conductorFresnelAvg(n, k), WithinAbs(avg, 5e-3));
}

TEST_CASE("Energy tables hit the reference values", "[bsdf][tables]") {
    const BsdfTables &t = bsdfTables();
    auto r = [](float alpha) { return lutRough(alpha, alpha); };
    CHECK_THAT(ggxAlbedo(t, r(1.0f), 1.0f), WithinAbs(0.307, 2e-3));
    CHECK_THAT(ggxAlbedo(t, r(0.5f), 0.5f), WithinAbs(0.698, 2e-3));
    CHECK_THAT(ggxAlbedo(t, r(0.5f), 0.05f), WithinAbs(0.913, 2e-3));
    CHECK_THAT(dielectricAlbedoRT(t, 1.5f, r(0.3f), 1.0f), WithinAbs(0.989, 2e-3));
    CHECK_THAT(dielectricAlbedoRT(t, 1.0f / 1.5f, r(1.0f), 1.0f), WithinAbs(0.418, 2e-3));
    CHECK_THAT(dielectricAlbedoR(t, 1.5f, r(0.05f), 1.0f), WithinAbs(0.0403, 1e-3));
    // Smooth limits and monotonicity.
    for (const float mu : {0.05f, 0.5f, 1.0f}) {
        CHECK_THAT(ggxAlbedo(t, 0.0f, mu), WithinAbs(1.0, 1e-3));
        float last = 2.0f;
        for (int i = 0; i <= 10; ++i) {
            const float e = ggxAlbedo(t, float(i) / 10.0f, std::fmax(mu, 0.2f));
            CHECK(e <= last + 1e-4f);
            last = e;
        }
    }
    // Average dielectric Fresnel against quadrature, from outside and inside
    // (the inside follows by reciprocity).
    for (const float eta : {1.1f, 1.5f, 2.4f, 1.0f / 1.33f, 1.0f / 1.5f}) {
        double avg = 0.0;
        for (int i = 0; i < 100000; ++i) {
            const float mu = (i + 0.5f) / 100000.0f;
            avg += 2.0 * fresnelDielectric(mu, eta) * mu / 100000.0;
        }
        INFO("eta " << eta);
        CHECK_THAT(dielectricFresnelAvg(t, eta), WithinAbs(avg, 2e-3));
    }
}

TEST_CASE("Energy tables match the current lobe code", "[bsdf][tables]") {
    // 16 random cells recomputed with independent random numbers: a table
    // generated from older lobes fails here (regenerate: nova_bsdf_tables).
    const BsdfTables &t = bsdfTables();
    Pcg32 rng(2024);
    for (int k = 0; k < 16; ++k) {
        const int ir = int(rng.uniform() * kLutRough), iy = 3 + int(rng.uniform() * (kLutMu - 3));
        const int iz = 1 + int(rng.uniform() * (kLutEta - 1));
        const float r = lutRoughAt(ir), mu = lutMuAt(iy), eta = lutEtaAt(iz, kLutEta);
        const Vec3f wo = direction(mu, 0.0f);
        INFO("r " << r << " mu " << mu << " eta " << eta);
        const uint32_t n = 1u << 16;
        const double e = albedo(adapt(conductor(r * r, r * r)), wo, n, 11 + k);
        CHECK(std::fabs(e - t.ggxE[ir * kLutMu + iy]) < 5e-3);
        const double rt = albedo(adapt(dielectric(r * r, eta, DielectricLobe::kBoth)), wo, n, 31 + k, double(eta) * eta);
        CHECK(std::fabs(rt - t.dielRT[(iz * kLutRough + ir) * kLutMu + iy]) < 5e-3);
        const double rtInv =
            albedo(adapt(dielectric(r * r, 1.0f / eta, DielectricLobe::kBoth)), wo, n, 51 + k, 1.0 / (double(eta) * eta));
        CHECK(std::fabs(rtInv - t.dielRTinv[(iz * kLutRough + ir) * kLutMu + iy]) < 5e-3);
        const double thin = albedo(adapt(thinDielectric(r * r, eta)), wo, n, 71 + k);
        CHECK(std::fabs(thin - t.thinRT[(iz * kLutRough + ir) * kLutMu + iy]) < 5e-3);
    }
}

TEST_CASE("Compensated lobes pass the white furnace", "[bsdf][furnace]") {
    const BsdfTables &t = bsdfTables();
    const uint32_t n = 1u << 18;
    for (const float mu : {0.05f, 0.2f, 0.5f, 1.0f})
        for (const float rough : {0.0f, 0.25f, 0.5f, 1.0f}) {
            const Vec3f wo = direction(mu);
            const float alpha = std::fmax(rough * rough, kMinAlpha);
            INFO("mu " << mu << ", roughness " << rough);
            // White metal: E (1 + F_ms (1 - E) / E) with F_avg = 1 is 1.
            const float rIndex = lutRough(alpha, alpha);
            const float E = ggxAlbedo(t, rIndex, mu), Eavg = ggxAlbedoAvg(t, rIndex);
            const double metal = albedo(adapt(conductor(alpha, alpha)), wo, n, 7) * msCompensation(E, Eavg, spectrum(1.0f)).x;
            CHECK_THAT(metal, WithinAbs(1.0, 5e-3));
            // Anisotropic metal (alphas 2.56 apart) through the rms-alpha
            // index: close at normal incidence (3.6% over at roughness 1),
            // approximate elsewhere (the albedo depends on the azimuth). The
            // pair keeps the rms alpha, as the material models' anisotropy
            // mappings do; like them, the index uses the lobe's alphas
            // (clamped at 1).
            const float spread = std::sqrt(2.0f / (1.0f + 1.0f / sqr(2.56f)));
            const GgxDistribution ga = GgxDistribution::make(alpha * spread, alpha * spread / 2.56f);
            const float ra = lutRough(ga.ax, ga.ay);
            const double aniso = albedo(adapt(conductor(ga.ax, ga.ay)), wo, n, 8) *
                                 msCompensation(ggxAlbedo(t, ra, mu), ggxAlbedoAvg(t, ra), spectrum(1.0f)).x;
            CHECK_THAT(aniso, WithinAbs(1.0, mu < 1.0f ? 0.1 : rough < 1.0f ? 1.5e-2 : 4e-2));
            // Glass from outside and inside: E_RT / table = 1.
            for (const float eta : {1.33f, 1.5f, 2.0f, 1.0f / 1.5f}) {
                INFO("eta " << eta);
                const double rt = albedo(adapt(dielectric(alpha, eta, DielectricLobe::kBoth)), wo, n, 9, double(eta) * eta);
                CHECK_THAT(rt / dielectricAlbedoRT(t, eta, rIndex, mu), WithinAbs(1.0, 5e-3));
            }
            // A clear thin-walled sheet, compensated by its own table.
            for (const float eta : {1.33f, 1.5f, 2.0f}) {
                INFO("thin-walled eta " << eta);
                const double sheet = albedo(adapt(thinDielectric(alpha, eta)), wo, n, 10);
                CHECK_THAT(sheet / thinDielectricAlbedo(t, eta, rIndex, mu), WithinAbs(1.0, 5e-3));
            }
        }
}

TEST_CASE("Lobes stay finite for degenerate inputs", "[bsdf][robust]") {
    Pcg32 rng(77);
    int bad = 0;
    for (int i = 0; i < 300000; ++i) {
        const float alpha = rng.uniform() < 0.2 ? 0.0f : float(rng.uniform());
        const float eta = rng.uniform() < 0.1 ? 1.0f : float(0.2 + 3.0 * rng.uniform());
        float muO = float(rng.uniform() * 2.0 - 1.0);
        if (rng.uniform() < 0.1)
            muO = (rng.uniform() < 0.5 ? 1e-7f : -1e-7f);
        if (rng.uniform() < 0.05)
            muO = 1.0f;
        const Vec3f wo = direction(muO, float(rng.uniform() * 6.28));
        const Vec3f wiEval = direction(float(rng.uniform() * 2.0 - 1.0), float(rng.uniform() * 6.28));
        const Vec2f u{float(rng.uniform()), float(rng.uniform())};
        const float uc = float(rng.uniform());
        const ConductorLobe metal = conductor(alpha, alpha * 0.5f, FresnelModel::Conductor, {0.2f, 0.9f, 1.5f}, {3.0f, 2.5f, 2.0f});
        const DielectricLobe glass = dielectric(alpha, eta, DielectricLobe::kBoth);
        const ThinDielectricLobe sheet = thinDielectric(alpha, std::fmax(eta, 1.0f), {0.5f, 0.0f, 1.0f});
        auto finite = [](Spectrum c, float pdf) { return isFinite(c) && std::isfinite(pdf) && pdf >= 0.0f; };
        // eval writes pdf: evaluate before reading it (argument order is unspecified).
        float pdf = 0.0f;
        Spectrum f = metal.eval(wo, wiEval, pdf);
        bad += !finite(f, pdf);
        f = glass.eval(wo, wiEval, pdf);
        bad += !finite(f, pdf);
        f = sheet.eval(wo, wiEval, pdf);
        bad += !finite(f, pdf);
        LobeSample s;
        for (int which = 0; which < 3; ++which) {
            const bool ok = which == 0 ? metal.sample(wo, u, s) : which == 1 ? glass.sample(wo, u, uc, s) : sheet.sample(wo, u, uc, s);
            if (ok)
                bad += !finite(s.fcos, s.pdf) || std::fabs(length(s.wi) - 1.0f) > 1e-3f;
        }
    }
    CHECK(bad == 0);
}
