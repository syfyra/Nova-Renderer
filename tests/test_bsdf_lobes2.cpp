// M3.2 lobe library (docs/design/M3.md section 7): QON and EON diffuse,
// Charlie sheen and Zeltner fuzz, thin-film Fresnel and the shading-normal
// helpers.

#include "bsdf_stats.h"

#include "core/bsdf/diffuse.h"
#include "core/bsdf/energy.h"
#include "core/bsdf/shading_frame.h"
#include "core/bsdf/sheen.h"
#include "core/bsdf/thin_film.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <functional>

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

// Lobes with sample(wo, u, s) or sample(wo, u, uc, s).
template <class L> LobeUnderTest adapt(const L &lobe) {
    LobeUnderTest l;
    l.sample = [lobe](V3 wo, V3 u, V3 &wi, double &pdf, double &w, bool &delta) {
        LobeSample s;
        bool ok;
        if constexpr (requires { lobe.sample(Vec3f{}, Vec2f{}, 0.0f, s); })
            ok = lobe.sample(toF(wo), {float(u.x), float(u.y)}, float(u.z), s);
        else
            ok = lobe.sample(toF(wo), {float(u.x), float(u.y)}, s);
        if (!ok)
            return false;
        wi = toD(s.wi);
        delta = s.delta;
        pdf = s.pdf;
        w = s.fcos.x / s.pdf;
        return true;
    };
    l.pdf = [lobe](V3 wo, V3 wi) {
        float pdf = 0.0f;
        lobe.eval(toF(wo), toF(wi), pdf);
        return double(pdf);
    };
    l.evalCos = [lobe](V3 wo, V3 wi) {
        float pdf = 0.0f;
        return double(lobe.eval(toF(wo), toF(wi), pdf).x);
    };
    return l;
}

double albedo(const LobeUnderTest &lobe, Vec3f wo, uint32_t n, uint64_t seed, double *stdError = nullptr) {
    Pcg32 rng(seed);
    double sum = 0.0, sum2 = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        V3 wi{}, u{rng.uniform(), rng.uniform(), rng.uniform()};
        double pdf = 0, w = 0;
        bool delta = false;
        const double v = lobe.sample(toD(wo), u, wi, pdf, w, delta) ? w : 0.0;
        sum += v;
        sum2 += v * v;
    }
    const double mean = sum / n;
    if (stdError)
        *stdError = std::sqrt(std::fmax(0.0, sum2 / n - mean * mean) / n);
    return mean;
}

void runChi2(const char *name, const LobeUnderTest &lobe, std::initializer_list<float> mus) {
    Pcg32 rng(4321);
    for (const float mu : mus) {
        INFO(name << ", cos(wo) " << mu);
        const Chi2Result r = nova::test::chi2Test(lobe, toD(direction(mu)), rng, 1 << 18, 48, 5.0, 0.01, int(mus.size()));
        INFO(r.message << " (p " << r.pValue << ", dof " << r.dof << ", pdf mass " << r.pdfMass << ")");
        CHECK(r.pass);
        CHECK_THAT(r.pdfMass, WithinAbs(1.0, 2e-3)); // reflection lobes over the hemisphere
    }
}

} // namespace

TEST_CASE("Rough diffuse and sheen lobes sample their pdfs (chi-square)", "[bsdf][chi2]") {
    runChi2("QON sigma 1", adapt(QonLobe{spectrum(1.0f), 1.0f}), {0.2f, 0.8f});
    runChi2("EON r 0.5", adapt(EonLobe{spectrum(1.0f), 0.5f}), {0.1f, 0.5f, 0.9f});
    runChi2("EON r 1", adapt(EonLobe{spectrum(1.0f), 1.0f}), {0.1f, 0.5f, 0.9f});
    runChi2("Charlie r 0.3", adapt(CharlieLobe{spectrum(1.0f), 0.3f}), {0.3f, 0.9f});
    runChi2("Zeltner r 0.2", adapt(ZeltnerLobe{spectrum(1.0f), 0.2f}), {0.2f, 0.8f});
    runChi2("Zeltner r 0.7", adapt(ZeltnerLobe{spectrum(1.0f), 0.7f}), {0.2f, 0.8f});
}

TEST_CASE("Diffuse and sheen albedos match their definitions", "[bsdf]") {
    // EON preserves energy exactly for a white albedo; its directional albedo
    // formula matches the integral for colors.
    for (const float r : {0.0f, 0.5f, 1.0f})
        for (const float mu : {0.05f, 0.3f, 0.7f, 1.0f}) {
            INFO("EON r " << r << " mu " << mu);
            double se = 0.0;
            const double white = albedo(adapt(EonLobe{spectrum(1.0f), r}), direction(mu), 1 << 18, 3, &se);
            CHECK_THAT(white, WithinAbs(1.0, 4 * se + 2e-3));
            const EonLobe colored{{0.8f, 0.4f, 0.1f}, r};
            const double grey = albedo(adapt(EonLobe{spectrum(0.4f), r}), direction(mu), 1 << 18, 4, &se);
            CHECK_THAT(grey, WithinAbs(EonLobe{spectrum(0.4f), r}.directionalAlbedo(mu).x, 4 * se + 2e-3));
            CHECK(colored.directionalAlbedo(mu).x <= 0.8f + 1e-6f);
        }
    // QON at normal incidence: s = 0, so the albedo is exactly A.
    double se = 0.0;
    CHECK_THAT(albedo(adapt(QonLobe{spectrum(1.0f), 1.0f}), direction(1.0f), 1 << 18, 5, &se),
               WithinAbs(0.624060, 4 * se + 1e-4));
    CHECK_THAT(QonLobe::albedoFit(1.0f, 1.0f), WithinAbs(0.624060, 0.03)); // a fit: 0.647
    // Zeltner: a sample's weight is exactly R(mu, r) (Karma measured 0.024870).
    CHECK_THAT(albedo(adapt(ZeltnerLobe{spectrum(1.0f), 0.5f}), direction(1.0f), 1 << 14, 6),
               WithinAbs(zeltnerAlbedo(1.0f, 0.5f), 1e-5));
    CHECK_THAT(zeltnerAlbedo(1.0f, 0.5f), WithinAbs(0.0249, 2e-4));
    // Charlie: MaterialX's fit approximates the albedo of this form.
    for (const float r : {0.3f, 0.8f}) {
        const double e = albedo(adapt(CharlieLobe{spectrum(1.0f), r}), direction(0.5f), 1 << 18, 7, &se);
        CHECK_THAT(e, WithinAbs(CharlieLobe::albedoFit(0.5f, r), 0.04));
    }
}

TEST_CASE("Diffuse and sheen lobes conserve energy and stay finite", "[bsdf][robust]") {
    Pcg32 rng(17);
    int bad = 0;
    for (int set = 0; set < 128; ++set) {
        const float r = float(rng.uniform()), mu = float(0.02 + 0.98 * rng.uniform());
        const Vec3f wo = direction(mu, float(rng.uniform() * 6.28));
        double se = 0.0;
        // MaterialX's Charlie form (no masking term) reflects more than it
        // receives below roughness 0.2 at grazing views (mu < 0.09; up to 5.6
        // at r = 0): the closure caps it with the albedo table, the lobe
        // keeps the definition.
        const bool charlieBounded = r >= 0.2f || mu >= 0.1f;
        // MaterialX's classic Oren-Nayar (A, B form) exceeds 1 by up to ~1.4%
        // at grazing views (A + B sin(theta_o) / 2 for small sigma); the lobe
        // keeps the definition.
        const double excess[] = {0.015, 0.0, 0.0, 0.0};
        const char *names[] = {"QON", "EON", "Charlie", "Zeltner"};
        int which = 0;
        for (const LobeUnderTest &lobe : {adapt(QonLobe{spectrum(1.0f), r}), adapt(EonLobe{spectrum(1.0f), r}),
                                          adapt(CharlieLobe{spectrum(1.0f), charlieBounded ? r : 0.5f}),
                                          adapt(ZeltnerLobe{spectrum(1.0f), r})}) {
            const double e = albedo(lobe, wo, 1 << 12, uint64_t(set), &se);
            if (!(e <= 1.0 + excess[which] + 4 * se + 1e-3) || !std::isfinite(e)) {
                UNSCOPED_INFO(names[which] << ": r " << r << " mu " << mu << " albedo " << e << " +- " << se);
                ++bad;
            }
            ++which;
        }
    }
    CHECK(bad == 0);
    bad = 0;
    for (int i = 0; i < 100000; ++i) {
        const float r = rng.uniform() < 0.1 ? 0.0f : float(rng.uniform());
        const Vec3f wo = direction(rng.uniform() < 0.1 ? 1e-7f : float(rng.uniform()), float(rng.uniform() * 6.28));
        const Vec3f wi = direction(float(rng.uniform() * 2 - 1), float(rng.uniform() * 6.28));
        float pdf;
        for (const Spectrum f : {QonLobe{spectrum(1.0f), r}.eval(wo, wi, pdf), EonLobe{spectrum(1.0f), r}.eval(wo, wi, pdf),
                                 CharlieLobe{spectrum(1.0f), r}.eval(wo, wi, pdf), ZeltnerLobe{spectrum(1.0f), r}.eval(wo, wi, pdf)})
            bad += !isFinite(f) || f.x < 0.0f;
    }
    CHECK(bad == 0);
}

TEST_CASE("Zeltner fuzz stays finite when wo.z rounds above 1", "[bsdf][robust]") {
    // dot(wo, n) of two normalized float vectors is often 1 + 1 ulp; the fit's
    // sqrt(1 - mu) went NaN there and blacked out the whole closure.
    const Vec3f wo{0.0f, 0.0f, std::nextafter(1.0f, 2.0f)};
    for (const float r : {0.3f, 0.5f, 1.0f}) { // where the fits keep the lobe active at mu = 1
        const ZeltnerLobe z{spectrum(0.8f), r};
        INFO("r " << r);
        REQUIRE(z.active(wo.z));
        float pdf = -1.0f;
        const Spectrum f = z.eval(wo, direction(0.6f), pdf);
        CHECK(isFinite(f));
        CHECK(std::isfinite(pdf));
        CHECK(pdf > 0.0f);
        LobeSample s;
        REQUIRE(z.sample(wo, {0.3f, 0.7f}, s));
        CHECK(isFinite(s.fcos));
        CHECK(s.pdf > 0.0f);
    }
}

TEST_CASE("The thin-film Fresnel reduces to the base and keeps the DC term grey", "[bsdf][thinfilm]") {
    AiryBase glass{};
    glass.schlick = false;
    glass.n = spectrum(1.5f);
    glass.k = spectrum(0.0f);
    // A film with the index of air is no film: the base Fresnel.
    for (const float mu : {0.1f, 0.5f, 1.0f}) {
        const Spectrum F = fresnelAiry(mu, ThinFilm{1.0f, 500.0f, 1.0f}, glass, kXyzToRec709Grey);
        CHECK_THAT(F.x, WithinAbs(fresnelDielectric(mu, 1.5f), 1e-4));
        CHECK_THAT(F.y, WithinAbs(F.x, 1e-4));
        CHECK_THAT(F.z, WithinAbs(F.x, 1e-4));
    }
    // A very thick film averages the interference out: the incoherent
    // reflectance, the same in every channel (Karma: R/G 1.27).
    const Spectrum thick = fresnelAiry(1.0f, ThinFilm{1.0f, 50000.0f, 1.33f}, glass, kXyzToRec709Grey);
    CHECK_THAT(thick.x, WithinAbs(thick.y, 1e-3));
    CHECK_THAT(thick.z, WithinAbs(thick.y, 1e-3));
    // Incoherent sum of both interfaces: R12 + T12^2 R23 / (1 - R12 R23)
    // (the 1.33 film between air and glass reflects less than bare glass).
    const float R12 = fresnelDielectric(1.0f, 1.33f), R23 = fresnelDielectric(1.0f, 1.5f / 1.33f);
    CHECK_THAT(thick.y, WithinAbs(R12 + sqr(1.0f - R12) * R23 / (1.0f - R12 * R23), 1e-3));
    // A thin film colors the reflection.
    const Spectrum thin = fresnelAiry(1.0f, ThinFilm{1.0f, 400.0f, 1.33f}, glass, kXyzToRec709Grey);
    CHECK(maxComponent(thin) - std::fmin(thin.x, std::fmin(thin.y, thin.z)) > 0.01f);
    CHECK(isFinite(thin));
    // A Schlick base (F82 tint 1) behind an index-1 film is the base too.
    AiryBase metal{};
    metal.schlick = true;
    metal.F0 = {0.9f, 0.6f, 0.3f};
    metal.tint = spectrum(1.0f);
    metal.exponent = 5.0f;
    const Spectrum Fm = fresnelAiry(0.6f, ThinFilm{1.0f, 300.0f, 1.0f}, metal, kXyzToRec709Grey);
    CHECK(isFinite(Fm));
}

TEST_CASE("Shading normals are bent just enough for valid reflections", "[bsdf][frame]") {
    Pcg32 rng(23);
    const Vec3f ng{0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 10000; ++i) {
        const Vec3f wo = direction(float(0.01 + 0.99 * rng.uniform()), float(rng.uniform() * 6.28));
        const Vec3f n = direction(float(0.2 + 0.8 * rng.uniform()), float(rng.uniform() * 6.28));
        const Vec3f bent = ensureValidSpecularReflection(ng, wo, n);
        CHECK_THAT(length(bent), WithinAbs(1.0, 1e-4));
        const Vec3f r = bent * (2.0f * dot(bent, wo)) - wo;
        const float threshold = std::fmin(0.9f * wo.z, 0.01f);
        CHECK(dot(r, ng) >= threshold - 5e-4f);
        if (dot(n * (2.0f * dot(n, wo)) - wo, ng) >= threshold)
            CHECK(bent.x == n.x);
    }
    CHECK(bumpShadowingTerm(ng, ng, direction(0.3f)) == 1.0f);
    CHECK(bumpShadowingTerm(ng, direction(0.9f, 0.0f), direction(0.0001f, 3.14f)) < 0.1f);
}

TEST_CASE("The sheen albedo table matches the Charlie lobe", "[bsdf][tables]") {
    const BsdfTables &t = bsdfTables();
    Pcg32 rng(808);
    for (int k = 0; k < 8; ++k) {
        const int ir = 7 + int(rng.uniform() * (kLutRough - 7)), iy = 4 + int(rng.uniform() * (kLutMu - 4));
        const float r = float(ir) / float(kLutRough - 1), mu = lutMuAt(iy);
        INFO("r " << r << " mu " << mu);
        double se = 0.0;
        const double e = albedo(adapt(CharlieLobe{spectrum(1.0f), r}), direction(mu, 0.0f), 1 << 16, 100 + k, &se);
        CHECK_THAT(double(t.charlieE[ir * kLutMu + iy]), WithinAbs(e, 4 * se + 2e-3));
        CHECK_THAT(double(sheenAlbedo(t, r, mu)), WithinAbs(e, 4 * se + 2e-3));
    }
}
