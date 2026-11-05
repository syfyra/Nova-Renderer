// M3.3 closures and surface models (docs/design/M3.md section 7):
// one-sample MIS over the lobes, white furnaces of Standard Surface and
// OpenPBR configurations (the OpenPBR spec's "white furnace testing"),
// exact anchors and the agreement of both models on their shared subset.

#include "bsdf_stats.h"
#include "test_helpers.h"

#include "core/bsdf/surface.h"
#include "core/math/sobol.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace nova;
using nova::test::AlbedoPair;
using nova::test::Chi2Result;
using nova::test::LobeUnderTest;
using nova::test::Pcg32;
using nova::test::V3;
using Catch::Matchers::WithinAbs;
namespace sf = nova::surface_field;

namespace {

Vec3f toF(V3 v) { return {float(v.x), float(v.y), float(v.z)}; }
V3 toD(Vec3f v) { return {v.x, v.y, v.z}; }
Vec3f direction(float mu, float phi = 0.3f) {
    const float s = std::sqrt(std::fmax(0.0f, 1.0f - mu * mu));
    return {s * std::cos(phi), s * std::sin(phi), mu};
}

ClosureContext context() { return {&bsdfTables(), kXyzToRec709Grey}; }
const ShadingGeometry kFlat{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, true};

ClosureList build(const SurfaceInputs &in, Vec3f wo) {
    ClosureList cl;
    buildClosure(in, kFlat, wo, context(), cl);
    return cl;
}

// The closure as a lobe under test (one channel), built for each wo.
LobeUnderTest adapt(const SurfaceInputs &in, int channel = 0) {
    LobeUnderTest l;
    l.sample = [in, channel](V3 wo, V3 u, V3 &wi, double &pdf, double &w, bool &delta) {
        const ClosureList cl = build(in, toF(wo));
        ClosureSample s;
        if (!sampleClosure(cl, toF(wo), toF(u), s))
            return false;
        wi = toD(s.wi);
        pdf = s.pdf;
        delta = s.delta;
        w = s.weight[channel];
        return true;
    };
    l.pdf = [in](V3 wo, V3 wi) { return double(evalClosure(build(in, toF(wo)), toF(wo), toF(wi)).pdf); };
    l.evalCos = [in, channel](V3 wo, V3 wi) {
        return double(evalClosure(build(in, toF(wo)), toF(wo), toF(wi)).fcos[channel]);
    };
    return l;
}

// Total albedo (all events, delta included) at wo, per channel.
// Directional albedo in energy units: refracted samples carry radiance
// scaled by 1 / eta^2, so they count eta^2 times.
Spectrum albedo(const SurfaceInputs &in, Vec3f wo, uint32_t n, uint64_t seed) {
    const ClosureList cl = build(in, wo);
    Pcg32 rng(seed);
    Spectrum sum = spectrum(0.0f);
    for (uint32_t i = 0; i < n; ++i) {
        ClosureSample s;
        if (sampleClosure(cl, wo, {float(rng.uniform()), float(rng.uniform()), float(rng.uniform())}, s))
            sum += s.weight * (s.refract ? s.eta * s.eta : 1.0f);
    }
    return sum * (1.0f / float(n));
}

SurfaceInputs whiteStandardSurface() {
    SurfaceInputs in = standardSurfaceDefaults();
    in.set(sf::baseColor, spectrum(1.0f));
    return in;
}
SurfaceInputs whiteOpenPbr() {
    SurfaceInputs in = openPbrDefaults();
    in.set(sf::baseColor, spectrum(1.0f));
    in.set(sf::subsurfaceColor, spectrum(1.0f));
    return in;
}

// White thin-walled Standard Surface glass.
SurfaceInputs thinGlass(float specular, float roughness, float ior) {
    SurfaceInputs in = whiteStandardSurface();
    in.set(sf::thinWalled, 1.0f);
    in.set(sf::transmissionWeight, 1.0f);
    in.set(sf::specularWeight, specular);
    in.set(sf::specularRoughness, roughness);
    in.set(sf::specularIor, ior);
    return in;
}

// The closure built once for wo as a lobe under test (green channel, energy
// units); only valid at that wo.
LobeUnderTest adaptAt(const SurfaceInputs &in, Vec3f wo) {
    const ClosureList cl = build(in, wo);
    LobeUnderTest l;
    l.sample = [cl](V3 w, V3 u, V3 &wi, double &pdf, double &weight, bool &delta) {
        ClosureSample s;
        if (!sampleClosure(cl, toF(w), toF(u), s))
            return false;
        wi = toD(s.wi);
        pdf = s.pdf;
        delta = s.delta;
        weight = s.weight.y * (s.refract ? s.eta * s.eta : 1.0f);
        return true;
    };
    l.pdf = [cl](V3 w, V3 wi) { return double(evalClosure(cl, toF(w), toF(wi)).pdf); };
    l.evalCos = [cl](V3 w, V3 wi) { return double(evalClosure(cl, toF(w), toF(wi)).fcos.y); };
    return l;
}

// Directional albedo (green, energy units) by importance sampling on
// Owen-scrambled Sobol points: far less noise than albedo() at equal count.
double sobolAlbedo(const ClosureList &cl, Vec3f wo, uint32_t n, uint32_t seed) {
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        ClosureSample s;
        if (sampleClosure(cl, wo, Sampler{seed, i}.get3D(0), s))
            sum += double(s.weight.y) * (s.refract ? s.eta * s.eta : 1.0f);
    }
    return sum / n;
}

const float kCos80 = 0.17364818f;

} // namespace

TEST_CASE("Closures sample their mixture pdf (chi-square)", "[closure][chi2]") {
    Pcg32 params(31);
    for (int set = 0; set < 6; ++set) {
        SurfaceInputs in = set % 2 == 0 ? standardSurfaceDefaults() : openPbrDefaults();
        in.set(sf::baseColor, {float(params.uniform()), float(params.uniform()), float(params.uniform())});
        in.set(sf::metalness, float(params.uniform() < 0.3 ? params.uniform() : 0.0));
        in.set(sf::specularRoughness, float(0.15 + 0.8 * params.uniform()));
        in.set(sf::specularAnisotropy, float(params.uniform() < 0.5 ? 0.6 * params.uniform() : 0.0));
        in.set(sf::coatWeight, float(params.uniform() < 0.5 ? params.uniform() : 0.0));
        in.set(sf::coatRoughness, float(0.1 + 0.5 * params.uniform()));
        in.set(sf::sheenWeight, float(params.uniform() < 0.5 ? params.uniform() : 0.0));
        in.set(sf::diffuseRoughness, float(params.uniform()));
        in.set(sf::transmissionWeight, float(params.uniform() < 0.3 ? params.uniform() : 0.0));
        const LobeUnderTest lobe = adapt(in);
        Pcg32 rng(1000 + set);
        for (const float mu : {0.3f, 0.8f}) {
            INFO("parameter set " << set << " (" << (set % 2 == 0 ? "Standard Surface" : "OpenPBR") << "), cos(wo) " << mu);
            const Chi2Result r = nova::test::chi2Test(lobe, toD(direction(mu)), rng, 1 << 17, 32, 5.0, 0.01, 12);
            INFO(r.message << " (p " << r.pValue << ", dof " << r.dof << ")");
            CHECK(r.pass);
        }
    }
}

TEST_CASE("Sampled closure weights equal the evaluated mixture", "[closure]") {
    SurfaceInputs in = openPbrDefaults();
    in.set(sf::coatWeight, 0.7f);
    in.set(sf::sheenWeight, 0.5f);
    in.set(sf::metalness, 0.3f);
    in.set(sf::transmissionWeight, 0.4f);
    in.set(sf::specularRoughness, 0.4f);
    Pcg32 rng(3);
    int checked = 0;
    for (int i = 0; i < 4000; ++i) {
        const Vec3f wo = direction(float(0.05 + 0.95 * rng.uniform()), float(rng.uniform() * 6.28));
        const ClosureList cl = build(in, wo);
        ClosureSample s;
        if (!sampleClosure(cl, wo, {float(rng.uniform()), float(rng.uniform()), float(rng.uniform())}, s) || s.delta)
            continue;
        const ClosureEval e = evalClosure(cl, wo, s.wi);
        CHECK_THAT(e.pdf, Catch::Matchers::WithinRel(s.pdf, 1e-5f));
        CHECK_THAT(e.fcos.y / e.pdf, Catch::Matchers::WithinRel(s.weight.y, 1e-4f));
        ++checked;
    }
    CHECK(checked > 3000);
}

TEST_CASE("White surface configurations pass the furnace", "[closure][furnace]") {
    struct Config {
        std::string name;
        SurfaceInputs in;
    };
    std::vector<Config> configs;
    for (const bool pbr : {false, true}) {
        const std::string m = pbr ? "OpenPBR " : "Standard Surface ";
        auto white = [&] { return pbr ? whiteOpenPbr() : whiteStandardSurface(); };
        SurfaceInputs diffuse = white();
        configs.push_back({m + "glossy diffuse", diffuse});
        SurfaceInputs metal = white();
        metal.set(sf::metalness, 1.0f);
        configs.push_back({m + "metal", metal});
        SurfaceInputs glass = white();
        glass.set(sf::transmissionWeight, 1.0f);
        configs.push_back({m + "glass", glass});
        SurfaceInputs mixed = white();
        mixed.set(sf::metalness, 0.4f);
        mixed.set(sf::transmissionWeight, 0.3f);
        configs.push_back({m + "metal + glass + diffuse", mixed});
        SurfaceInputs coated = white();
        coated.set(sf::coatWeight, 1.0f);
        coated.set(sf::coatRoughness, 0.25f);
        if (pbr)
            coated.set(sf::coatDarkening, 0.0f); // darkening removes energy by design
        configs.push_back({m + "coated diffuse", coated});
        SurfaceInputs sheen = white();
        sheen.set(sf::sheenWeight, 1.0f);
        configs.push_back({m + (pbr ? "fuzz" : "sheen") + " over diffuse", sheen});
        SurfaceInputs thin = white();
        thin.set(sf::thinWalled, 1.0f);
        thin.set(sf::transmissionWeight, 1.0f);
        configs.push_back({m + "thin-walled glass", thin});
        SurfaceInputs translucent = white();
        translucent.set(sf::thinWalled, 1.0f);
        translucent.set(sf::subsurfaceWeight, 1.0f);
        configs.push_back({m + "thin-walled subsurface", translucent});
        SurfaceInputs aniso = white();
        aniso.set(sf::metalness, 1.0f);
        aniso.set(sf::specularAnisotropy, 0.5f);
        configs.push_back({m + "anisotropic metal", aniso});
    }
    for (const Config &c : configs)
        for (const float mu : {0.1f, 0.4f, 1.0f})
            for (const float rough : {0.0f, 0.3f, 0.7f}) {
                SurfaceInputs in = c.in;
                in.set(sf::specularRoughness, rough);
                INFO(c.name << ", cos(wo) " << mu << ", roughness " << rough);
                const Spectrum e = albedo(in, direction(mu), 1 << 16, 77);
                const bool aniso = c.name.find("anisotropic") != std::string::npos;
                CHECK_THAT(e.y, WithinAbs(1.0, aniso && mu < 1.0f ? 0.06 : 0.012));
            }
}

TEST_CASE("Surface models hit exact anchors", "[closure]") {
    const BsdfTables &t = bsdfTables();
    // Glossy diffuse: E_Rc + (1 - E_Rc) rho (OpenPBR, rho 0.5, r 0.3, eta 1.5).
    for (const float mu : {0.2f, 0.6f, 1.0f}) {
        SurfaceInputs in = openPbrDefaults();
        in.set(sf::baseColor, spectrum(0.5f));
        in.set(sf::specularRoughness, 0.3f);
        const Vec2f a = alphaOpenPbr(0.3f, 0.0f);
        const float r = lutRough(a.x, a.y);
        const float Erc = dielectricAlbedoR(t, 1.5f, r, mu) / dielectricAlbedoRT(t, 1.5f, r, mu);
        const EonLobe eon{spectrum(0.5f), 0.0f};
        const float expected = Erc + (1.0f - Erc) * eon.directionalAlbedo(mu).x;
        CHECK_THAT(albedo(in, direction(mu), 1 << 17, 5).y, WithinAbs(expected, 4e-3));
    }
    // OpenPBR smooth coat (eta 1.6) over rho 0.5 with specular_weight 0 and
    // MaterialX's darkening (docs/research/m3/surface-models.md 8.1).
    const float mus[] = {0.98f, 0.875f, 0.7f, 0.5f, 0.3f, 0.125f};
    const float expected[] = {0.3089f, 0.3099f, 0.3175f, 0.3468f, 0.4340f, 0.6434f};
    for (int i = 0; i < 6; ++i) {
        SurfaceInputs in = openPbrDefaults();
        in.set(sf::baseColor, spectrum(0.5f));
        in.set(sf::specularWeight, 0.0f);
        in.set(sf::coatWeight, 1.0f);
        in.set(sf::coatRoughness, 0.0f);
        INFO("cos(wo) " << mus[i]);
        CHECK_THAT(albedo(in, direction(mus[i]), 1 << 17, 6).y, WithinAbs(expected[i], 4e-3));
    }
    // Standard Surface: specular_color (1, 0, 0) over white keeps 1 - E_R in green.
    for (const float mu : {0.3f, 1.0f}) {
        SurfaceInputs in = whiteStandardSurface();
        in.set(sf::specularColor, {1.0f, 0.0f, 0.0f});
        const Vec2f a = alphaStandardSurface(0.2f, 0.0f);
        const float r = lutRough(a.x, a.y);
        const float Erc = dielectricAlbedoR(t, 1.5f, r, mu) / dielectricAlbedoRT(t, 1.5f, r, mu);
        CHECK_THAT(albedo(in, direction(mu), 1 << 17, 7).y, WithinAbs(1.0f - Erc, 4e-3));
    }
    // Standard Surface QON at normal incidence: A(sigma 1) = 0.624060 under a
    // perfect-white, specular-free base.
    SurfaceInputs qon = whiteStandardSurface();
    qon.set(sf::specularWeight, 0.0f);
    qon.set(sf::diffuseRoughness, 1.0f);
    CHECK_THAT(albedo(qon, direction(1.0f), 1 << 17, 8).y, WithinAbs(0.624060, 3e-3));
}

TEST_CASE("Standard Surface and OpenPBR agree on their shared subset", "[closure]") {
    // No coat, sheen or anisotropy; specular weight 1 and white; the same
    // roughness: Lambert vs EON at roughness 0 and the same interface.
    for (const float rough : {0.1f, 0.5f})
        for (const float mu : {0.2f, 0.7f}) {
            SurfaceInputs ss = standardSurfaceDefaults(), pbr = openPbrDefaults();
            for (SurfaceInputs *in : {&ss, &pbr}) {
                in->set(sf::baseColor, {0.6f, 0.3f, 0.1f});
                in->set(sf::specularRoughness, rough);
            }
            INFO("roughness " << rough << ", cos(wo) " << mu);
            const Spectrum a = albedo(ss, direction(mu), 1 << 17, 9), b = albedo(pbr, direction(mu), 1 << 17, 9);
            CHECK_THAT(a.x, WithinAbs(b.x, 5e-3));
            CHECK_THAT(a.z, WithinAbs(b.z, 5e-3));
            const ClosureList ca = build(ss, direction(mu)), cb = build(pbr, direction(mu));
            const Vec3f wi = direction(0.6f, 2.0f);
            CHECK_THAT(evalClosure(ca, direction(mu), wi).fcos.x, WithinAbs(evalClosure(cb, direction(mu), wi).fcos.x, 2e-3));
        }
}

TEST_CASE("Diffuse-only Standard Surface QON stays lit with wo below the shading normal", "[closure]") {
    // Shading normal tilted 80 degrees toward +x, wo from the -x side: above
    // Ng, below N (a smooth-shaded silhouette). The fast path (specular 0)
    // must bend the QON frame like the general path (specular 1e-4).
    const float tilt = 80.0f * 3.14159265f / 180.0f;
    const ShadingGeometry geo{{0.0f, 0.0f, 1.0f}, {std::sin(tilt), 0.0f, std::cos(tilt)}, {0.0f, 1.0f, 0.0f}, true};
    const Vec3f wo{-0.8660254f, 0.0f, 0.5f}, wi = normalize(Vec3f{0.2f, 0.1f, 1.0f});
    REQUIRE(dot(wo, geo.n) < 0.0f);
    auto eval = [&](float specular) {
        SurfaceInputs in = standardSurfaceDefaults();
        in.set(sf::specularWeight, specular);
        in.set(sf::specularRoughness, 1.0f); // the 1e-4 specular lobe stays negligible at wi
        in.set(sf::diffuseRoughness, 1.0f);
        ClosureList cl;
        buildClosure(in, geo, wo, context(), cl);
        return evalClosure(cl, wo, wi).fcos.y;
    };
    const float fast = eval(0.0f), general = eval(1e-4f);
    CHECK(general > 0.0f);
    CHECK_THAT(fast, Catch::Matchers::WithinRel(general, 2e-3f));
}

TEST_CASE("Standard Surface clamps negative base and subsurface colors", "[closure]") {
    // MaterialX's base_color_nonnegative / subsurface_color_nonnegative, with
    // and without the diffuse-only fast path and the thin-walled translucency.
    for (const float specular : {0.0f, 1.0f}) {
        SurfaceInputs in = standardSurfaceDefaults();
        in.set(sf::specularWeight, specular);
        in.set(sf::baseColor, {-0.5f, 0.5f, 0.5f});
        INFO("specular " << specular);
        const ClosureList cl = build(in, direction(0.7f));
        CHECK(cl.albedo.x >= 0.0f);
        CHECK(albedo(in, direction(0.7f), 1 << 12, 3).x >= 0.0f);
    }
    SurfaceInputs thin = standardSurfaceDefaults();
    thin.set(sf::thinWalled, 1.0f);
    thin.set(sf::subsurfaceWeight, 1.0f);
    thin.set(sf::subsurfaceColor, {-0.5f, 0.5f, 0.5f});
    CHECK(build(thin, direction(0.7f)).albedo.x >= 0.0f);
}

TEST_CASE("Standard Surface seen from inside uses the transmission roughness", "[closure]") {
    // Inside a closed glass the interface has transmission_extra_roughness
    // and the coat-affected roughness, as from outside (and as the expanded
    // MaterialX graph's transmission leaf).
    struct Case {
        float roughness, extra, coat, coatRoughness, affect;
    };
    for (const Case c : {Case{0.0f, 0.5f, 0.0f, 0.1f, 0.0f}, Case{0.2f, 0.0f, 1.0f, 0.5f, 1.0f},
                         Case{0.1f, 0.3f, 0.6f, 0.4f, 0.5f}}) {
        SurfaceInputs in = standardSurfaceDefaults();
        in.set(sf::transmissionWeight, 1.0f);
        in.set(sf::specularRoughness, c.roughness);
        in.set(sf::transmissionExtraRoughness, c.extra);
        in.set(sf::coatWeight, c.coat);
        in.set(sf::coatRoughness, c.coatRoughness);
        in.set(sf::coatAffectRoughness, c.affect);
        const float k = c.affect * c.coat * c.coatRoughness;
        const float rT = c.roughness + c.extra + (1.0f - (c.roughness + c.extra)) * k;
        const float alpha = alphaStandardSurface(rT, 0.0f).x;
        INFO("roughness " << c.roughness << ", extra " << c.extra << ", coat-affected k " << k);
        const ClosureList cl = build(in, direction(-0.7f)); // below Ng: inside
        REQUIRE(cl.count == 1);
        REQUIRE(cl.lobe[0].type == LobeType::Dielectric);
        CHECK_FALSE(cl.lobe[0].delta());
        CHECK_THAT(cl.lobe[0].dielectric.ggx.ax, WithinAbs(alpha, 1e-5));
    }
}

TEST_CASE("Thin-sheet albedo tables match the lobe between their nodes", "[closure][tables]") {
    // E_R' and E_T' of the clear sheet exactly as pushTransmission builds it
    // (both interfaces with their inter-reflections, the widened transmission
    // roughness), by importance sampling, against the tables' interpolation
    // off their nodes: thinR, and thinRT - thinR for the transmission.
    // Scrambled Sobol points (other scrambles than the generator's).
    const BsdfTables &t = bsdfTables();
    uint32_t seed = 1;
    for (const float roughness : {0.1f, 0.3f, 0.6f, 1.0f})
        for (const float ior : {1.33f, 1.5f, 2.0f})
            for (const float mu : {0.1f, kCos80, 0.3f, 0.5f, 0.7f, 1.0f}) {
                const float alpha = alphaStandardSurface(roughness, 0.0f).x;
                const float alphaT = ThinDielectricLobe::transmissionAlpha(alpha, ior);
                ThinDielectricLobe lobe{GgxDistribution::make(alpha, alpha), GgxDistribution::make(alphaT, alphaT), ior,
                                        spectrum(1.0f), spectrum(1.0f)};
                const Vec3f wo = direction(mu);
                const uint32_t n = 1u << 18;
                double er = 0.0, et = 0.0;
                ++seed;
                for (uint32_t i = 0; i < n; ++i) {
                    LobeSample s;
                    const Vec3f u = Sampler{0x7e57u + seed, i}.get3D(0);
                    if (!lobe.sample(wo, {u.x, u.y}, u.z, s))
                        continue;
                    const double w = s.delta ? s.fcos.y : s.fcos.y / s.pdf;
                    (s.transmit ? et : er) += w / n;
                }
                const float r = lutRough(alpha, alpha);
                const double tableR = thinDielectricAlbedoR(t, ior, r, mu);
                const double tableT = thinDielectricAlbedo(t, ior, r, mu) - tableR;
                INFO("roughness " << roughness << ", IOR " << ior << ", cos(wo) " << mu << ": E_R' " << er << " (table "
                                  << tableR << "), E_T' " << et << " (table " << tableT << ")");
                CHECK(std::fabs(er - tableR) < 2.5e-3); // measured: at most 1.5e-3
                CHECK(std::fabs(et - tableT) < 2.5e-3);
            }
}

TEST_CASE("Thin-walled Standard Surface glass with specular < 1 keeps its energy", "[closure][furnace]") {
    // The sheet's transmission takes the energy the weakened reflection gives
    // up (smooth sheet: exact, one event of weight R' + T' = 1 per sample).
    for (const float specular : {0.0f, 0.5f})
        for (const float mu : {0.1f, 0.4f, 1.0f}) {
            SurfaceInputs in = whiteStandardSurface();
            in.set(sf::thinWalled, 1.0f);
            in.set(sf::transmissionWeight, 1.0f);
            in.set(sf::specularWeight, specular);
            in.set(sf::specularRoughness, 0.0f);
            INFO("specular " << specular << ", cos(wo) " << mu);
            CHECK_THAT(albedo(in, direction(mu), 1 << 12, 11).y, WithinAbs(1.0, 1e-4));
            CHECK_THAT(build(in, direction(mu)).albedo.y, WithinAbs(1.0, 1e-4));
        }
    // Every roughness, angle and IOR: the energy of the specular-1 sheet. The
    // boost scales every microfacet's T', so it comes from the rough sheet's
    // own E_R' and E_T' (before, the single interface's E_R bounded it:
    // specular 0, roughness 0.3, 80 degrees kept 0.78). Importance sampling
    // on scrambled Sobol points, the same for both sheets; the estimator
    // pair of bsdf_stats.h (importance vs uniform) checks sampling against
    // evaluation where the uniform one converges.
    uint32_t seed = 1;
    for (const float roughness : {0.0f, 0.1f, 0.3f, 0.6f, 1.0f})
        for (const float ior : {1.33f, 1.5f, 2.0f})
            for (const float mu : {0.1f, kCos80, 0.3f, 0.5f, 0.7f, 1.0f}) {
                const Vec3f wo = direction(mu);
                ++seed;
                const double clear = sobolAlbedo(build(thinGlass(1.0f, roughness, ior), wo), wo, 1u << 16, seed);
                for (const float specular : {0.0f, 0.5f}) {
                    const SurfaceInputs in = thinGlass(specular, roughness, ior);
                    const double a = sobolAlbedo(build(in, wo), wo, 1u << 16, seed);
                    INFO("roughness " << roughness << ", IOR " << ior << ", cos(wo) " << mu << ", specular " << specular
                                      << ": " << a << ", specular 1: " << clear);
                    CHECK(std::fabs(a - clear) < 4e-3); // measured: at most 3.4e-3 (roughness 0.1, mu 0.1)
                    CHECK(a < 1.0 + 2e-3);
                    if (roughness >= 0.3f && (mu == kCos80 || mu == 0.7f)) {
                        Pcg32 rng(seed);
                        const AlbedoPair p = nova::test::albedoEstimates(adaptAt(in, wo), toD(wo), rng, 1 << 15);
                        INFO("importance " << p.importance << " +- " << p.importanceErr << ", uniform " << p.uniform
                                           << " +- " << p.uniformErr);
                        CHECK(std::fabs(p.uniform - p.importance) < 5.0 * std::hypot(p.uniformErr, p.importanceErr));
                        CHECK(std::fabs(p.importance - a) < 5.0 * p.importanceErr + 1e-3);
                    }
                }
            }
    // OpenPBR's specular_weight modulates the sheet's index instead (no
    // boost): the compensated sheet at that index is white.
    for (const float roughness : {0.3f, 1.0f})
        for (const float specular : {0.0f, 0.5f})
            for (const float mu : {0.1f, 0.5f, 1.0f}) {
                SurfaceInputs in = whiteOpenPbr();
                in.set(sf::thinWalled, 1.0f);
                in.set(sf::transmissionWeight, 1.0f);
                in.set(sf::specularWeight, specular);
                in.set(sf::specularRoughness, roughness);
                INFO("OpenPBR, roughness " << roughness << ", specular " << specular << ", cos(wo) " << mu);
                CHECK_THAT(albedo(in, direction(mu), 1 << 16, 17).y, WithinAbs(1.0, 0.012));
            }
}

TEST_CASE("Rough thin-walled glass with specular < 1 samples its pdf", "[closure][chi2]") {
    // The boost changes the sheet's reflection probability (from the boosted
    // R'/T' at wo): chi-square of the sampled directions and sampled weights
    // equal to the evaluated f cos / pdf.
    Pcg32 rng(91);
    for (const float roughness : {0.3f, 0.7f})
        for (const float specular : {0.0f, 0.5f})
            for (const float mu : {0.25f, 0.8f}) {
                const SurfaceInputs in = thinGlass(specular, roughness, 1.5f);
                const Vec3f wo = direction(mu);
                INFO("roughness " << roughness << ", specular " << specular << ", cos(wo) " << mu);
                const Chi2Result r = nova::test::chi2Test(adaptAt(in, wo), toD(wo), rng, 1 << 17, 32, 5.0, 0.01, 8);
                INFO(r.message << " (p " << r.pValue << ", dof " << r.dof << ")");
                CHECK(r.pass);
                const ClosureList cl = build(in, wo);
                int checked = 0;
                for (int i = 0; i < 1000; ++i) {
                    ClosureSample s;
                    if (!sampleClosure(cl, wo, {float(rng.uniform()), float(rng.uniform()), float(rng.uniform())}, s) ||
                        s.delta)
                        continue;
                    const ClosureEval e = evalClosure(cl, wo, s.wi);
                    CHECK_THAT(e.pdf, Catch::Matchers::WithinRel(s.pdf, 1e-5f));
                    CHECK_THAT(e.fcos.y / e.pdf, Catch::Matchers::WithinRel(s.weight.y, 1e-4f));
                    ++checked;
                }
                CHECK(checked > 900);
            }
}

TEST_CASE("Thin-walled glass with specular < 1: fake caustics and the albedo AOV ignore the roughness",
          "[closure][caustics]") {
    // What a straight line through the white sheet keeps, and its albedo AOV,
    // are the smooth sheet's at wo: transmission 1 - specular R' (the boost
    // of the smooth sheet, never above 1), albedo 1, at any roughness.
    const BsdfTables &t = bsdfTables();
    for (const float roughness : {0.0f, 0.3f, 1.0f})
        for (const float specular : {0.0f, 0.5f, 1.0f})
            for (const float mu : {0.2f, 0.7f, 1.0f}) {
                const Vec3f wo = direction(mu);
                const ClosureList cl = build(thinGlass(specular, roughness, 1.5f), wo);
                const float r = fresnelDielectric(mu, 1.5f), Rs = 2.0f * r / (1.0f + r);
                INFO("roughness " << roughness << ", specular " << specular << ", cos(wo) " << mu);
                CHECK_THAT(closureTransmittance(cl, wo, t).y, WithinAbs(1.0f - specular * Rs, 1e-5));
                CHECK_THAT(cl.albedo.y, WithinAbs(1.0, 1e-5));
            }
}

TEST_CASE("XPU renders rough thin-walled glass with specular < 1 like the CPU", "[gpu][render][closure]") {
    // A thin-walled pane (roughness 0.3, specular 0) seen at 80 degrees from
    // its normal under a white dome, one bounce: every pixel shows the sheet's
    // directional albedo, which must be the clear pane's (0.78 before the
    // sheet tables). Skipped without a GPU (the XPU render falls back).
    auto renderPane = [](float specular, RenderDevice device, bool &onGpu) {
        Scene scene;
        const float h = 10.0f, c = kCos80, s = std::sqrt(1.0f - kCos80 * kCos80);
        MeshDesc pane; // spans x and (0, c, s): normal (0, -s, c), 80 degrees from the view
        pane.points = {{-h, -h * c, -h * s}, {h, -h * c, -h * s}, {h, h * c, h * s}, {-h, h * c, h * s}};
        pane.indices = {0, 1, 2, 0, 2, 3};
        const MeshId mesh = scene.createMesh();
        scene.updateMesh(mesh, pane);
        MaterialDesc glass;
        glass.inputs = thinGlass(specular, 0.3f, 1.5f);
        const MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, glass);
        scene.setMeshMaterials(mesh, {m});
        LightDesc dome;
        dome.type = LightType::Dome;
        dome.intensity = 1.0f;
        scene.updateLight(scene.createLight(), dome);
        scene.commit();
        Renderer renderer;
        RenderSettings settings;
        settings.device = device;
        settings.maxSamples = 64;
        settings.maxDepth = 1;
        settings.clampIndirect = 0.0f;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        const int size = 16;
        renderer.setCamera(nova::test::makeOrthoCamera({0.0f, 0.0f, 10.0f}, 0.5f, size, size));
        renderer.resize(size, size);
        onGpu = true;
        while (!renderer.converged()) {
            renderer.renderPass({});
            onGpu = onGpu && renderer.lastPassOnGpu();
        }
        double mean = 0.0;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                mean += nova::test::pixelMean(renderer.film(), x, y).y / double(size * size);
        return mean;
    };
    bool onGpu = false, cpuOnGpu = true;
    const double xpu = renderPane(0.0f, RenderDevice::Xpu, onGpu);
    if (!onGpu)
        SKIP("no GPU: the XPU render ran on the CPU");
    const double cpu = renderPane(0.0f, RenderDevice::Cpu, cpuOnGpu), clear = renderPane(1.0f, RenderDevice::Cpu, cpuOnGpu);
    INFO("specular 0: CPU " << cpu << ", XPU " << xpu << "; specular 1 (CPU): " << clear);
    CHECK(!cpuOnGpu);
    CHECK_THAT(cpu, WithinAbs(clear, 0.01));
    CHECK_THAT(cpu, WithinAbs(1.0, 0.015));
    CHECK_THAT(xpu, WithinAbs(cpu, 0.005 * cpu));
}

TEST_CASE("Emission and unlit surfaces", "[closure]") {
    SurfaceInputs unlit = unlitDefaults();
    unlit.set(sf::emissionColor, {0.5f, 0.25f, 0.125f});
    const ClosureList cl = build(unlit, direction(0.7f));
    CHECK(cl.emission.x == 0.5f);
    CHECK(cl.emission.z == 0.125f);
    CHECK(cl.count == 0); // no transmission: absorbs everything else
    unlit.set(sf::transmissionWeight, 0.5f);
    const ClosureList through = build(unlit, direction(0.7f));
    ClosureSample s;
    REQUIRE(sampleClosure(through, direction(0.7f), {0.3f, 0.6f, 0.5f}, s));
    CHECK(s.delta);
    CHECK(s.transmit);
    CHECK_THAT(s.weight.x, WithinAbs(0.5, 1e-6));
    CHECK_THAT(s.wi.z, WithinAbs(-0.7, 1e-6)); // straight through
    // Standard Surface emission under a coat: dimmed by the coat's Fresnel.
    SurfaceInputs ss = standardSurfaceDefaults();
    ss.set(sf::emissionWeight, 2.0f);
    ss.set(sf::coatWeight, 1.0f);
    const float F0 = iorToF0(1.5f);
    CHECK_THAT(build(ss, direction(1.0f)).emission.x, WithinAbs(2.0f * (1.0f - F0), 1e-5));
}
