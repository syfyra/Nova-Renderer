// M9 hair (docs/design/M9.md D5, D6; research docs/research/m9/curves-hair.md
// 3.4, 3.5, 4.3): the Chiang fibre as four closure lobes. A white fibre
// scatters all light (BSDF-sampled furnace 1, its pbrt 10-term lobes within
// 0.4 % by quadrature); BSDF sampling agrees with uniform sampling of f and
// its density integrates to 1; R/T events follow the side of the tube
// normal; Karma Hair's colour controls reproduce Karma's measured fibre
// albedos (absorption, melanin defaults, colour inversion, tint).

#include "test_helpers.h"

#include "core/bsdf/surface.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace nova;
using Catch::Matchers::WithinAbs;
namespace sf = nova::surface_field;

namespace {

// A fibre along t = (0, cos a, sin a) seen from wo = (0, 0, 1) (the
// research's fur scenes), hit at offset h across: the tube normal there.
struct FibreHit {
    Vec3f t, n, wo{0.0f, 0.0f, 1.0f};
};
FibreHit fibreAt(float aDeg, float h) {
    const float a = aDeg * kPi / 180.0f;
    FibreHit f;
    f.t = {0.0f, std::cos(a), std::sin(a)};
    // The normal plane: x and z' = wo projected; n = h x + sqrt(1 - h^2) z'.
    const Vec3f z = normalize(f.wo - f.t * dot(f.wo, f.t));
    const Vec3f x = cross(z, f.t);
    f.n = normalize(x * h + z * std::sqrt(std::fmax(0.0f, 1.0f - h * h)));
    return f;
}

ClosureList hairClosure(const SurfaceInputs &in, const FibreHit &f) {
    ClosureList cl;
    const ShadingGeometry g{f.n, f.n, f.t, true, 0.5f};
    const BsdfTables *tables = &bsdfTables();
    buildClosure(in, g, f.wo, ClosureContext{tables, kXyzToRec709Grey}, cl);
    return cl;
}

SurfaceInputs whiteHair() {
    SurfaceInputs in = hairDefaults();
    in.set(sf::hairMelanin, 0.0f);
    in.set(sf::hairShift, 0.0f);
    return in;
}

// The mean over h in [-1, 1] of the closure's albedo (sum of tint A_p): a
// fibre's furnace value seen across its width.
Spectrum meanAlbedo(const SurfaceInputs &in, float aDeg, int n = 2000) {
    Spectrum sum = spectrum(0.0f);
    for (int i = 0; i < n; ++i) {
        const float h = -1.0f + 2.0f * (float(i) + 0.5f) / float(n);
        sum += hairClosure(in, fibreAt(aDeg, h)).albedo;
    }
    return sum * (1.0f / float(n));
}

} // namespace

TEST_CASE("A white fibre scatters all light", "[hair]") {
    for (const float a : {0.0f, 30.0f, 60.0f, 80.0f})
        for (const float roughness : {0.15f, 0.3f, 0.6f}) {
            SurfaceInputs in = whiteHair();
            in.set(sf::hairRoughness, roughness);
            in.set(sf::hairRoughnessAzimuthal, roughness);
            Rng rng = Rng::make(uint32_t(a * 10.0f), uint32_t(roughness * 100.0f), 3u);
            double sampled = 0.0, uniform = 0.0, uniformSq = 0.0, pdfIntegral = 0.0, pdfSq = 0.0;
            const int n = 200000;
            for (int i = 0; i < n; ++i) {
                const float h = -1.0f + 2.0f * rng.next();
                const FibreHit f = fibreAt(a, h);
                const ClosureList cl = hairClosure(in, f);
                REQUIRE(cl.count >= 1); // lobes of negligible weight are dropped
                ClosureSample s;
                if (sampleClosure(cl, f.wo, {rng.next(), rng.next(), rng.next()}, s))
                    sampled += s.weight.y;
                // Uniform sphere sampling of f and of the density.
                const Vec3f wi = sampleUniformSphere(rng.next2());
                const ClosureEval e = evalClosure(cl, f.wo, wi);
                const double fu = e.fcos.y / uniformSpherePdf(), pu = e.pdf / uniformSpherePdf();
                uniform += fu;
                uniformSq += fu * fu;
                pdfIntegral += pu;
                pdfSq += pu * pu;
            }
            INFO("a " << a << ", roughness " << roughness << ": sampled " << sampled / n << ", uniform " << uniform / n
                      << ", pdf integral " << pdfIntegral / n);
            // pbrt's 10-term series loses up to 0.4 % of the lobes (research 4.3);
            // its sampler draws the exact lobes, so sampled weights keep 1.
            CHECK_THAT(sampled / n, WithinAbs(1.0, 0.006));
            // Uniform sampling of the peaked lobes is noisy: within 5 standard errors.
            const double seF = std::sqrt((uniformSq / n - (uniform / n) * (uniform / n)) / n);
            const double seP = std::sqrt((pdfSq / n - (pdfIntegral / n) * (pdfIntegral / n)) / n);
            CHECK(std::fabs(uniform / n - 1.0) < 5.0 * seF + 0.005);
            CHECK(std::fabs(pdfIntegral / n - 1.0) < 5.0 * seP + 0.005);
        }
}

TEST_CASE("Hair events take the side of the tube normal", "[hair]") {
    // Seen head-on (a = 0) at the fibre's centre, light from behind passes
    // as TT (transmission, refract class), light from the front reflects.
    const FibreHit f = fibreAt(0.0f, 0.0f);
    const ClosureList cl = hairClosure(whiteHair(), f);
    Rng rng = Rng::make(7u, 0u, 1u);
    int reflect = 0, refract = 0;
    for (int i = 0; i < 20000; ++i) {
        ClosureSample s;
        if (!sampleClosure(cl, f.wo, {rng.next(), rng.next(), rng.next()}, s))
            continue;
        CHECK(s.transmit == (dot(s.wi, f.n) < 0.0f));
        CHECK(s.rayClass == (s.transmit ? RayClass::Refract : RayClass::Reflect));
        CHECK_FALSE(s.refract);
        (s.transmit ? refract : reflect)++;
    }
    CHECK(refract > 4 * reflect); // most of a white fibre's light goes through
    CHECK(cl.lobe[0].lpeDiffuse() == false);
}

TEST_CASE("Karma Hair's colour controls reproduce Karma's fibres", "[hair]") {
    // Furnace values of a single fibre, mean over its width (research 3.4,
    // measured in Karma; the model's predictions agree to 4 digits).
    struct Case {
        const char *name;
        SurfaceInputs in;
        float a;
        Spectrum karma;
    };
    SurfaceInputs absorbing = whiteHair();
    absorbing.set(sf::hairAbsorption, {0.2f, 0.5f, 1.0f});
    SurfaceInputs defaults = hairDefaults();
    SurfaceInputs dark = hairDefaults();
    dark.set(sf::hairMelanin, 0.8f);
    dark.set(sf::hairMelaninRedness, 0.0f);
    SurfaceInputs coloured = whiteHair();
    coloured.set(sf::hairBaseColor, {0.8f, 0.5f, 0.2f});
    const Case cases[] = {
        {"absorption (0.2, 0.5, 1)", absorbing, 0.0f, {0.6983f, 0.4247f, 0.2111f}},
        {"absorption (0.2, 0.5, 1), a 30", absorbing, 30.0f, {0.6830f, 0.4068f, 0.2028f}},
        {"absorption (0.2, 0.5, 1), a 60", absorbing, 60.0f, {0.6543f, 0.3982f, 0.2403f}},
        {"defaults (melanin 0.4, redness 0.1)", defaults, 0.0f, {0.6400f, 0.4790f, 0.2524f}},
        {"defaults, a 30", defaults, 30.0f, {0.6246f, 0.4607f, 0.2399f}},
        {"melanin 0.8, redness 0", dark, 0.0f, {0.2679f, 0.1455f, 0.0813f}},
        {"baseColor (0.8, 0.5, 0.2)", coloured, 0.0f, {0.9973f, 0.9747f, 0.8722f}},
    };
    for (const Case &c : cases) {
        const Spectrum m = meanAlbedo(c.in, c.a);
        INFO(c.name << ": (" << m.x << ", " << m.y << ", " << m.z << ") vs Karma (" << c.karma.x << ", " << c.karma.y
                    << ", " << c.karma.z << ")");
        for (int ch = 0; ch < 3; ++ch)
            CHECK(std::fabs(m[ch] - c.karma[ch]) < 0.005f * c.karma[ch] + 5e-4f);
    }
    // tint (0.5, 1, 1) halves red (research 3.5).
    SurfaceInputs tinted = hairDefaults();
    tinted.set(sf::hairTint, {0.5f, 1.0f, 1.0f});
    CHECK_THAT(meanAlbedo(tinted, 0.0f).x, WithinAbs(0.5 * meanAlbedo(defaults, 0.0f).x, 1e-5));
    // diffuse 1: a Lambert of diffuseColor on the tube normal, reflection only.
    SurfaceInputs diffuse = hairDefaults();
    diffuse.set(sf::hairDiffuse, 1.0f);
    diffuse.set(sf::hairDiffuseColor, spectrum(0.5f));
    const ClosureList cl = hairClosure(diffuse, fibreAt(0.0f, 0.0f));
    REQUIRE(cl.count == 1);
    CHECK(cl.lobe[0].type == LobeType::Lambert);
}
