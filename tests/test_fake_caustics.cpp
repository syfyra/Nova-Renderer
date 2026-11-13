// Fake caustics (docs/design/M4.md 3.3, M4.2): with nova:object:fakeCaustics
// on a refractive object, shadow rays cross it straight with its
// transmittance (1 - F at the crossing angle, times the transmission tint
// and weight, roughness ignored), and rays after a diffuse event pass through
// it the same way, so light sampling and BSDF sampling agree (MIS stays
// unbiased). Off (the default), glass blocks shadow rays.
//
// Setup (as the opacity test): ground z = 0 under a distant light travelling
// along (-1, 0, -1); a glass plane at z = 1, x in [2, 3] (outside the view)
// shades the ground at x in [1, 2]; direct light only.

#include "test_helpers.h"

#include "core/bsdf/fresnel.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sf = nova::surface_field;
using Catch::Matchers::WithinAbs;

namespace {

SurfaceInputs glass(float roughness = 0.0f, Spectrum tint = spectrum(1.0f), bool thin = false) {
    SurfaceInputs in = standardSurfaceDefaults();
    in.set(sf::baseWeight, 0.0f);
    in.set(sf::transmissionWeight, 1.0f);
    in.set(sf::transmissionColor, tint);
    in.set(sf::specularRoughness, roughness);
    in.set(sf::thinWalled, thin ? 1.0f : 0.0f);
    return in;
}

struct Setup {
    Scene scene;
    explicit Setup(bool fakeDefault) { scene.setDefaultFakeCaustics(fakeDefault); }

    // A plane facing +Z (the shadow rays towards the sun hit its back), or
    // -Z with `down` (they hit its front).
    MeshId plane(float z, float x0, float x1, const SurfaceInputs &in, const RayLimitOverrides *o = nullptr,
                 bool down = false) {
        MeshDesc m;
        m.points = {{x0, -4.0f, z}, {x1, -4.0f, z}, {x1, 4.0f, z}, {x0, 4.0f, z}};
        m.indices = down ? std::vector<uint32_t>{0, 2, 1, 0, 3, 2} : std::vector<uint32_t>{0, 1, 2, 0, 2, 3};
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, m);
        MaterialDesc d;
        d.inputs = in;
        const MaterialId mat = scene.createMaterial();
        scene.updateMaterial(mat, d);
        scene.setMeshMaterials(id, {mat});
        if (o)
            scene.setMeshRayLimits(id, *o);
        return id;
    }
    void ground() { plane(0.0f, -4.0f, 4.0f, lambertDefaults(spectrum(0.5f))); }
    void sun(const std::string &shadowLink = {}) {
        LightDesc sun;
        sun.type = LightType::Distant;
        sun.angleDeg = 0.0f;
        sun.shadowLink = shadowLink;
        const double c = std::sqrt(0.5), sn = std::sqrt(0.5);
        sun.transform.m[0][0] = c;
        sun.transform.m[0][2] = -sn;
        sun.transform.m[2][0] = sn;
        sun.transform.m[2][2] = c;
        scene.updateLight(scene.createLight(), sun);
    }

    // Mean of the shaded region over the lit one, per channel.
    Spectrum shadowRatio(uint32_t spp = 16, uint32_t maxDepth = 1, LightStrategy strategy = LightStrategy::MIS) {
        scene.commit();
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = spp;
        settings.maxDepth = maxDepth;
        settings.clampIndirect = 0.0f;
        settings.pixelFilterScale = 0.0f;
        settings.lightStrategy = strategy;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        const int size = 16; // x from -2 to 2
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, size, size));
        renderer.resize(size, size);
        renderToConvergence(renderer);
        Spectrum lit{0, 0, 0}, shaded{0, 0, 0};
        int nl = 0, ns = 0;
        for (int y = 2; y < 14; ++y)
            for (int x = 0; x < size; ++x) {
                const float wx = (float(x) + 0.5f) / 4.0f - 2.0f;
                const Spectrum v = pixelMean(renderer.film(), x, y);
                if (wx > 1.1f && wx < 1.9f) {
                    shaded += v;
                    ++ns;
                } else if (wx < 0.9f) {
                    lit += v;
                    ++nl;
                }
            }
        shaded *= 1.0f / float(ns);
        lit *= 1.0f / float(nl);
        return {shaded.x / lit.x, shaded.y / lit.y, shaded.z / lit.z};
    }
};

const float kCos45 = float(std::sqrt(0.5));

} // namespace

TEST_CASE("Glass blocks shadow rays unless it fakes caustics", "[render][causticsfake]") {
    Setup off(false);
    off.ground();
    off.sun();
    off.plane(1.0f, 2.0f, 3.0f, glass());
    CHECK_THAT(off.shadowRatio().y, WithinAbs(0.0, 1e-6));

    Setup on(true);
    on.ground();
    on.sun();
    on.plane(1.0f, 2.0f, 3.0f, glass());
    const double T = 1.0 - fresnelDielectric(kCos45, 1.5f);
    CHECK_THAT(on.shadowRatio().y, WithinAbs(T, 1e-4));
}

TEST_CASE("Fake-caustic shadows: tint, roughness, stacks, thin walls", "[render][causticsfake]") {
    const double T = 1.0 - fresnelDielectric(kCos45, 1.5f);
    {
        Setup s(true);
        s.ground();
        s.sun();
        s.plane(1.0f, 2.0f, 3.0f, glass(0.0f, {1.0f, 0.5f, 0.25f}));
        const Spectrum r = s.shadowRatio();
        CHECK_THAT(r.x, WithinAbs(T, 1e-4));
        CHECK_THAT(r.y, WithinAbs(T * 0.5, 1e-4));
        CHECK_THAT(r.z, WithinAbs(T * 0.25, 1e-4));
    }
    {
        Setup s(true); // roughness is ignored (up to the energy table's interpolation)
        s.ground();
        s.sun();
        s.plane(1.0f, 2.0f, 3.0f, glass(0.4f));
        CHECK_THAT(s.shadowRatio().y, WithinAbs(T, 5e-3));
    }
    {
        Setup s(true); // two planes along the shadow rays: two crossings
        s.ground();
        s.sun();
        s.plane(1.0f, 2.0f, 3.0f, glass());
        s.plane(1.2f, 2.2f, 3.2f, glass());
        CHECK_THAT(s.shadowRatio().y, WithinAbs(T * T, 1e-4));
    }
    {
        Setup s(true); // a thin-walled sheet: its slab transmission
        s.ground();
        s.sun();
        s.plane(1.0f, 2.0f, 3.0f, glass(0.0f, spectrum(1.0f), true));
        const double r = fresnelDielectric(kCos45, 1.5f);
        CHECK_THAT(s.shadowRatio().y, WithinAbs((1.0 - r) * (1.0 - r) / (1.0 - r * r), 1e-4));
    }
}

TEST_CASE("Fake caustics are an object property", "[render][causticsfake]") {
    const double T = 1.0 - fresnelDielectric(kCos45, 1.5f);
    RayLimitOverrides on, off;
    on.fakeCaustics = 1;
    off.fakeCaustics = 0;
    Setup a(false);
    a.ground();
    a.sun();
    a.plane(1.0f, 2.0f, 3.0f, glass(), &on);
    CHECK_THAT(a.shadowRatio().y, WithinAbs(T, 1e-4));
    Setup b(true);
    b.ground();
    b.sun();
    b.plane(1.0f, 2.0f, 3.0f, glass(), &off);
    CHECK_THAT(b.shadowRatio().y, WithinAbs(0.0, 1e-6));
}

TEST_CASE("Light and BSDF sampling agree through fake-caustic glass", "[render][causticsfake][mis]") {
    // A white dome; glass at z = 0.5 over x in [1, 5] covers part of the sky
    // of the ground at x in [-2, 2]. Light samples cross the glass as shadow
    // rays, BSDF samples as paths after a diffuse event (one more iteration
    // for the pass-through).
    auto render = [](LightStrategy strategy) {
        Setup s(true);
        s.ground();
        s.plane(0.5f, 1.0f, 5.0f, glass(0.0f, {1.0f, 0.5f, 0.25f}));
        LightDesc dome;
        dome.type = LightType::Dome;
        s.scene.updateLight(s.scene.createLight(), dome);
        s.scene.commit();
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 512;
        settings.maxDepth = 2;
        settings.clampIndirect = 0.0f;
        settings.pixelFilterScale = 0.0f;
        settings.lightStrategy = strategy;
        RayLimits limits;
        limits.limit[int(LimitClass::Diffuse)] = 0.0f; // the ground's ray only gathers lights
        settings.limits = limits;
        renderer.setScene(&s.scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({-1.0f, 0.0f, 5.0f}, 0.75f, 8, 8)); // x in [-1.75, -0.25]: open sky
        renderer.resize(8, 8);
        renderToConvergence(renderer);
        Spectrum sum{0, 0, 0};
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                sum += pixelMean(renderer.film(), x, y);
        return sum * (1.0f / 64.0f);
    };
    const Spectrum light = render(LightStrategy::LightOnly), bsdf = render(LightStrategy::BsdfOnly);
    CHECK_THAT(bsdf.x / light.x, WithinAbs(1.0, 0.02));
    CHECK_THAT(bsdf.z / light.z, WithinAbs(1.0, 0.02));
    // The glass darkens the blue channel more than red (its tint).
    CHECK(light.z < light.x);
}

namespace {

// The mean of an 8 x 8 render.
Spectrum imageMean(const Scene &scene, const RenderSettings &settings, const Camera &camera) {
    Renderer renderer;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(8, 8);
    renderToConvergence(renderer);
    Spectrum sum{0, 0, 0};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sum += pixelMean(renderer.film(), x, y);
    return sum * (1.0f / 64.0f);
}

} // namespace

TEST_CASE("MIS through fake-caustic glass weighs from the scattering point", "[render][causticsfake][mis]") {
    // A rect light just above glass at z = 1 lights the ground through it;
    // the camera sits between them. A BSDF sample of the ground passes
    // through the glass (a null event) and reaches the light from there. Its
    // MIS weight needs the light's density at the ground, where next-event
    // estimation sampled the light: measured from the glass, the light
    // looks huge and MIS counted it about twice.
    auto render = [](LightStrategy strategy) {
        Setup s(true);
        s.ground();
        s.plane(1.0f, -4.0f, 4.0f, glass());
        LightDesc rect;
        rect.type = LightType::Rect; // emits along local -Z: down
        rect.width = rect.height = 1.0f;
        rect.transform = translation({0.0f, 0.0f, 1.05f});
        s.scene.updateLight(s.scene.createLight(), rect);
        s.scene.commit();
        RenderSettings settings;
        settings.maxSamples = 256;
        settings.maxDepth = 2;
        settings.clampIndirect = 0.0f;
        settings.pixelFilterScale = 0.0f;
        settings.lightStrategy = strategy;
        RayLimits limits;
        limits.limit[int(LimitClass::Diffuse)] = 0.0f; // the ground's ray only gathers lights
        settings.limits = limits;
        return imageMean(s.scene, settings, makeOrthoCamera({0.0f, 0.0f, 0.5f}, 0.75f, 8, 8));
    };
    const Spectrum light = render(LightStrategy::LightOnly), mis = render(LightStrategy::MIS);
    REQUIRE(light.y > 0.0f);
    CHECK_THAT(mis.y / light.y, WithinAbs(1.0, 0.03));
}

TEST_CASE("Opaque fake-caustic surfaces keep their throughput", "[render][causticsfake]") {
    // Lambert only: a ground and a ceiling, lit by a rect light between them
    // that faces the ceiling, so the ground sees only bounced light. Nothing
    // transmits, so fake caustics change nothing; dividing by a pass-through
    // probability that is never taken brightened every bounce by 1 / 0.95.
    auto render = [](bool fake) {
        Setup s(fake);
        s.ground();
        s.plane(1.0f, -4.0f, 4.0f, lambertDefaults(spectrum(0.8f)));
        LightDesc rect;
        rect.type = LightType::Rect;
        rect.width = rect.height = 0.5f;
        rect.intensity = 4.0f;
        rect.transform = translation({0.0f, 0.0f, 0.5f});
        rect.transform.m[1][1] = -1.0; // local -Z (the emission) to +Z: up
        rect.transform.m[2][2] = -1.0;
        s.scene.updateLight(s.scene.createLight(), rect);
        s.scene.commit();
        RenderSettings settings;
        settings.maxSamples = 64;
        settings.maxDepth = 5;
        settings.clampIndirect = 0.0f;
        settings.pixelFilterScale = 0.0f;
        return imageMean(s.scene, settings, makeOrthoCamera({0.0f, 0.0f, 0.3f}, 0.75f, 8, 8));
    };
    const Spectrum off = render(false), on = render(true);
    REQUIRE(off.y > 0.0f);
    CHECK_THAT(on.y / off.y, WithinAbs(1.0, 2e-3));
}

TEST_CASE("Fake caustics keep shadow links", "[render][causticsfake][linking]") {
    // The sun's shadow set holds other objects ("casters"), not the opaque
    // plane that would shade the ground: no shadow. Glass with fake caustics
    // elsewhere in the scene must not bring the plane's shadow back (the
    // crossings stepped onto it and stopped).
    for (const bool fakeGlass : {false, true}) {
        INFO("fake-caustic glass " << fakeGlass);
        Setup s(false);
        s.ground();
        s.sun("casters");
        s.plane(1.0f, 2.0f, 3.0f, lambertDefaults(spectrum(0.5f)));
        RayLimitOverrides on;
        on.fakeCaustics = 1;
        s.plane(0.5f, -4.0f, -3.5f, glass(), fakeGlass ? &on : nullptr); // out of every shadow ray's way
        s.scene.commit();
        REQUIRE(s.scene.shadowLinking());
        REQUIRE(s.scene.anyFakeCaustics() == fakeGlass);
        CHECK_THAT(s.shadowRatio().y, WithinAbs(1.0, 1e-4));
    }
}

// Karma's fake caustics (docs/design/M11-P1.md, measured with husk and glass
// planes): per crossing 1 - F (Schlick, F0 from the index) for the
// interface, and another 1 - F where the surface reflects on the side the
// ray comes from: front faces, back faces only with internal reflection. The
// shadow rays towards the sun hit the default plane's back face.
TEST_CASE("Karma's fake-caustic factor", "[render][causticsfake][karma]") {
    const double mu = kCos45;
    const double t = 1.0 - (0.04 + 0.96 * std::pow(1.0 - mu, 5.0)); // Schlick, F0 0.04 at IOR 1.5
    const auto ratio = [](RayLimits const &defaults, bool down, SurfaceInputs const &in = glass()) {
        Setup s(false);
        s.scene.setObjectDefaults(defaults);
        s.ground();
        s.sun();
        s.plane(1.0f, 2.0f, 3.0f, in, nullptr, down);
        return s.shadowRatio();
    };
    RayLimits karma;
    karma.fakeCaustics = true;
    karma.fakeStyle.karma = true;
    karma.internalReflection = false;
    CHECK_THAT(ratio(karma, false).y, WithinAbs(t, 1e-4));    // back face: the interface only
    CHECK_THAT(ratio(karma, true).y, WithinAbs(t * t, 1e-4)); // front face: and the reflection
    RayLimits internal = karma;
    internal.internalReflection = true;
    CHECK_THAT(ratio(internal, false).y, WithinAbs(t * t, 1e-4));
    // Tints multiply per crossing, as in Nova's model.
    const Spectrum tinted = ratio(karma, false, glass(0.0f, {1.0f, 0.5f, 0.25f}));
    CHECK_THAT(tinted.z, WithinAbs(t * 0.25, 1e-4));
}

TEST_CASE("Karma's fake-caustic controls", "[render][causticsfake][karma]") {
    const double mu = kCos45;
    const double t = 1.0 - (0.04 + 0.96 * std::pow(1.0 - mu, 5.0));
    const auto ratio = [](FakeCausticsStyle const &style, SurfaceInputs const &in = glass()) {
        RayLimits d;
        d.fakeCaustics = true;
        d.fakeStyle = style;
        d.internalReflection = false;
        Setup s(false);
        s.scene.setObjectDefaults(d);
        s.ground();
        s.sun();
        s.plane(1.0f, 2.0f, 3.0f, in);
        return s.shadowRatio().y;
    };
    FakeCausticsStyle base;
    base.karma = true;
    FakeCausticsStyle c = base; // the colour per crossing
    c.color[0] = c.color[1] = c.color[2] = 0.5f;
    CHECK_THAT(ratio(c), WithinAbs(0.5 * t, 1e-4));
    FakeCausticsStyle o = base; // lerp(1, t, opacity)
    o.opacity[0] = o.opacity[1] = o.opacity[2] = 0.5f;
    CHECK_THAT(ratio(o), WithinAbs(0.5 + 0.5 * t, 1e-4));
    FakeCausticsStyle d = base; // Schlick's bias of mu with 1 - d: d = 0.5 gives mu
    d.fresnelDarken = 0.5f;
    CHECK_THAT(ratio(d), WithinAbs(t * mu, 1e-4));
    d.fresnelDarken = 0.25f;
    CHECK_THAT(ratio(d), WithinAbs(t * mu / ((1.0 / 0.75 - 2.0) * (1.0 - mu) + 1.0), 1e-4));
    d.fresnelDarken = 1.0f;
    CHECK_THAT(ratio(d), WithinAbs(0.0, 1e-6));
    FakeCausticsStyle n = c; // the colour alone, no Fresnel
    n.bsdf = false;
    CHECK_THAT(ratio(n), WithinAbs(0.5, 1e-4));
    // Surfaces that do not transmit stay opaque whatever the controls.
    n.opacity[0] = n.opacity[1] = n.opacity[2] = 0.5f;
    CHECK_THAT(ratio(n, lambertDefaults(spectrum(0.5f))), WithinAbs(0.0, 1e-6));
    // Nova's physical factor takes the controls too.
    FakeCausticsStyle nova = c;
    nova.karma = false;
    CHECK_THAT(ratio(nova), WithinAbs(0.5 * (1.0 - fresnelDielectric(kCos45, 1.5f)), 1e-4));
}

// The render camera's NDC position (Karma's space:ndc, Global::Pndc) is
// known to every shading of a render: also to the shadow rays' crossings of
// fake-caustic surfaces (before, the CPU evaluated them without a camera,
// the position 0; the XPU with it). A tint of 1 where the camera sees the
// point in front of it (depth > 0), else 0, shadows as a constant tint of 1.
TEST_CASE("Fake-caustic shadows see the render camera", "[render][causticsfake]") {
    const auto ratio = [](bool ndc) {
        Setup s(true);
        s.ground();
        const MeshId g = s.plane(1.0f, 2.0f, 3.0f, glass());
        if (ndc) {
            using namespace shading;
            IrGraph graph;
            graph.model = SurfaceModel::StandardSurface;
            IrBuilder b(graph);
            const Ref depth = b.component(b.global(Global::Pndc), 2);
            b.output(sf::transmissionColor, 3, b.convert(b.binary(Op::GreaterEq, depth, b.constant(1e-3f)), 3));
            MaterialDesc d;
            d.inputs = glass();
            d.program = compile(graph).program;
            REQUIRE(d.program);
            const MaterialId m = s.scene.createMaterial();
            s.scene.updateMaterial(m, d);
            s.scene.setMeshMaterials(g, {m});
        }
        s.sun();
        return s.shadowRatio();
    };
    const Spectrum constant = ratio(false), ndc = ratio(true);
    REQUIRE(constant.x > 0.5f);
    CHECK_THAT(ndc.x, WithinAbs(constant.x, 1e-5));
    CHECK_THAT(ndc.z, WithinAbs(constant.z, 1e-5));
}

// Karma's constrain-roughness keeps a path's glossy roughness for its later
// events; a fake-caustic pass-through (a path after a diffuse event crossing
// the glass) must still weigh as its shadow rays do, which have no path
// roughness: the clamp leaves the pass-through's transmittance alone. The
// camera looks down through a rough thin sheet (path roughness 1) at a
// ground under smooth fake-caustic glass and a large rect light (a uniform
// dome is not light-sampled, both strategies would take its BSDF samples):
// light and BSDF sampling agree.
TEST_CASE("Constrained roughness leaves fake-caustic pass-throughs as their shadow rays",
          "[render][causticsfake][mis]") {
    auto render = [](LightStrategy strategy) {
        Setup s(true);
        s.ground();
        // The rough sheet under the camera, without reflection: its own light
        // (a glossy event: fake caustics do not apply, so its light samples
        // and its BSDF samples see the glass differently, by design) is 0.
        SurfaceInputs sheet = glass(1.0f, spectrum(1.0f), true);
        sheet.set(sf::specularWeight, 0.0f);
        s.plane(0.3f, -4.0f, 4.0f, sheet);
        s.plane(0.5f, -4.0f, 4.0f, glass(0.0f, {1.0f, 0.5f, 0.25f})); // smooth glass over everything
        LightDesc rect; // emits along its -Z: down
        rect.type = LightType::Rect;
        rect.width = rect.height = 6.0f;
        rect.transform = translation({0.0f, 0.0f, 1.5f});
        s.scene.updateLight(s.scene.createLight(), rect);
        s.scene.commit();
        RenderSettings settings;
        settings.maxSamples = 512;
        settings.maxDepth = 8;
        settings.clampIndirect = 0.0f;
        settings.pixelFilterScale = 0.0f;
        settings.lightStrategy = strategy;
        settings.constrainRoughness = true;
        RayLimits limits;
        limits.limit[int(LimitClass::Diffuse)] = 0.0f; // the ground's ray only gathers lights
        settings.limits = limits;
        return imageMean(s.scene, settings, makeOrthoCamera({0.0f, 0.0f, 0.4f}, 0.75f, 8, 8));
    };
    const Spectrum light = render(LightStrategy::LightOnly), bsdf = render(LightStrategy::BsdfOnly);
    INFO("light " << light.x << " " << light.y << " " << light.z << ", BSDF " << bsdf.x << " " << bsdf.y << " "
                  << bsdf.z);
    REQUIRE(light.x > 0.0f);
    CHECK_THAT(bsdf.x / light.x, WithinAbs(1.0, 0.02));
    CHECK_THAT(bsdf.z / light.z, WithinAbs(1.0, 0.02));
}

// A ray past its limit gathers lights only, and after a diffuse event it
// crosses fake-caustic objects as a shadow ray would. Which rays do that
// depends on the class of their event, so the light behind such an object is
// the class's own: the share of the path's weight that a mixture's class has
// (WaveQueues::classShare) is taken where the ray crosses. Off a red plastic
// with no bounce allowed, the dome behind a pane arrives through the diffuse
// lobe alone and has its colour; with the whole mixture's weight the Phong
// lobe's white was in it.
TEST_CASE("The light behind a fake-caustic object is the light of the ray's class", "[gpu][render][causticsfake][limits]") {
    const Spectrum red{0.8f, 0.1f, 0.1f};
    auto render = [&](RenderDevice device, bool pane, bool &onGpu) {
        Setup s(false);
        SurfaceInputs plastic = plasticDefaults();
        plastic.set(sf::baseColor, red);
        s.plane(0.0f, -4.0f, 4.0f, plastic);
        RayLimitOverrides on;
        on.fakeCaustics = 1;
        if (pane)
            s.plane(0.1f, -4.0f, 4.0f, glass(), &on); // over all of the hemisphere but its last degree
        LightDesc dome;
        dome.type = LightType::Dome;
        dome.intensity = 1.0f;
        s.scene.updateLight(s.scene.createLight(), dome);
        s.scene.commit();
        RenderSettings settings;
        settings.device = device;
        settings.maxSamples = 1024;
        settings.maxDepth = 8;
        settings.lightStrategy = LightStrategy::BsdfOnly;
        settings.clampIndirect = 0.0f;
        settings.pixelFilterScale = 0.0f;
        RayLimits limits;
        limits.limit[int(LimitClass::Diffuse)] = 0.0f; // every bounce's ray gathers lights only
        limits.limit[int(LimitClass::Reflect)] = 0.0f;
        settings.limits = limits;
        Renderer renderer;
        renderer.setScene(&s.scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 0.05f}, 0.25f, 8, 8)); // between the plastic and the pane
        renderer.resize(8, 8);
        onGpu = true;
        while (!renderer.converged()) {
            renderer.renderPass({});
            onGpu = onGpu && renderer.lastPassOnGpu();
        }
        Spectrum sum{0, 0, 0};
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                sum += pixelMean(renderer.film(), x, y);
        return sum * (1.0f / 64.0f);
    };
    for (const RenderDevice device : {RenderDevice::Cpu, RenderDevice::Xpu}) {
        bool onGpu = false;
        const Spectrum open = render(device, false, onGpu), behind = render(device, true, onGpu);
        if (device == RenderDevice::Xpu && !onGpu) {
            WARN("no GPU: the XPU render ran on the CPU");
            continue;
        }
        INFO((device == RenderDevice::Xpu ? "XPU" : "CPU")
             << ": under the open dome (" << open.x << ", " << open.y << ", " << open.z << "), behind the pane ("
             << behind.x << ", " << behind.y << ", " << behind.z << ")");
        // The open dome: every class's ray gathers it, both lobes' light.
        REQUIRE(open.x > 0.0f);
        CHECK(open.y / open.x > 1.3f * red.y / red.x);
        // Behind the pane: the diffuse lobe's.
        REQUIRE(behind.x > 0.1f);
        CHECK_THAT(behind.y / behind.x, WithinAbs(red.y / red.x, 0.03 * red.y / red.x));
        CHECK_THAT(behind.z / behind.x, WithinAbs(red.z / red.x, 0.03 * red.z / red.x));
    }
}
