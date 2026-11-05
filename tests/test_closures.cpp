// M3.3 closures and surface models (docs/design/M3.md section 7):
// one-sample MIS over the lobes, white furnaces of Standard Surface and
// OpenPBR configurations (the OpenPBR spec's "white furnace testing"),
// exact anchors and the agreement of both models on their shared subset.

#include "bsdf_stats.h"

#include "core/bsdf/surface.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace nova;
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
    // Rough sheets: never more than the clear sheet's energy (the boost would
    // otherwise scale microfacets whose T' is already near 1 at grazing
    // angles: up to 2x), and more than without it.
    for (const float roughness : {0.3f, 1.0f})
        for (const float specular : {0.0f, 0.5f})
            for (const float mu : {0.1f, 0.4f, 1.0f}) {
                SurfaceInputs in = whiteStandardSurface();
                in.set(sf::thinWalled, 1.0f);
                in.set(sf::transmissionWeight, 1.0f);
                in.set(sf::specularWeight, specular);
                in.set(sf::specularRoughness, roughness);
                INFO("roughness " << roughness << ", specular " << specular << ", cos(wo) " << mu);
                const float a = albedo(in, direction(mu), 1 << 14, 13).y;
                CHECK(a <= 1.0f + 0.02f);
                if (mu == 1.0f) // little reflection to give up at normal incidence
                    CHECK(a >= 0.9f);
            }
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
