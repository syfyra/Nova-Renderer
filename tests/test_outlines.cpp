// M11 P9a (docs/design/M11-P9a.md): Karma's outlines and look-dev overrides
// in the renderer, against Karma's measured behaviour: the stencil's
// geometry and line profile, the line types and their thresholds, the
// composite and the outline planes; the simplified shading (display colour,
// facing ratio, depth cue fog, occlusion) and the light overrides
// (Emissive Objects, Headlight, Dome Light, autoheadlight); Nova XPU against
// the CPU.

#include "test_helpers.h"

#include "core/integrator/lookdev.h"
#include "core/integrator/outline.h"
#include "core/integrator/renderer.h"
#include "core/scene/scene.h"
#include "core/scene/visibility.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
namespace sf = nova::surface_field;

namespace {

MeshDesc quad(Vec3f o, Vec3f e1, Vec3f e2) {
    MeshDesc m;
    m.points = {o, o + e1, o + e1 + e2, o + e2};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

MeshId addMesh(Scene &scene, MeshDesc desc, MaterialId material = kInvalidId) {
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, std::move(desc));
    if (material != kInvalidId)
        scene.setMeshMaterials(id, {material});
    return id;
}

MaterialId emissive(Scene &scene, Spectrum c) {
    MaterialDesc m;
    m.inputs = standardSurfaceDefaults();
    m.inputs.set(sf::baseWeight, 0.0f);
    m.inputs.set(sf::specularWeight, 0.0f);
    m.inputs.set(sf::emissionWeight, 1.0f);
    m.inputs.set(sf::emissionColor, c);
    const MaterialId id = scene.createMaterial();
    scene.updateMaterial(id, m);
    return id;
}

MaterialId lambert(Scene &scene, float albedo) {
    MaterialDesc m;
    m.inputs = lambertDefaults(spectrum(albedo));
    const MaterialId id = scene.createMaterial();
    scene.updateMaterial(id, m);
    return id;
}

// The edge probes of docs/design/M11-P9a.md 4: a perspective camera at the
// origin looking down -Z (focal 50, aperture 36: 0.0045 tan per pixel at
// 160 x 120) sees an emissive card on the left half at z = -5 in front of
// an emissive backdrop at z = -10 (or a second card of the same object
// `gap` behind the first: a step).
constexpr int kW = 160, kH = 120;
constexpr float kTanPerPixel = 36.0f / 50.0f / float(kW);
const Spectrum kFront{0.2f, 0.4f, 0.6f}, kBack{0.1f, 0.1f, 0.1f};

struct Edge {
    Scene scene;
    MeshId front = 0;
    explicit Edge(float stepGap = -1.0f) {
        const MaterialId f = emissive(scene, kFront), b = emissive(scene, kBack);
        if (stepGap < 0.0f) {
            front = addMesh(scene, quad({-10.0f, -10.0f, -5.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 20.0f, 0.0f}), f);
            addMesh(scene, quad({-40.0f, -40.0f, -10.0f}, {80.0f, 0.0f, 0.0f}, {0.0f, 80.0f, 0.0f}), b);
        } else {
            // One object: the left card and, gap behind it, the right one
            // (reaching a little under the left one: no crack at the edge).
            MeshDesc m = quad({-10.0f, -10.0f, -5.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 20.0f, 0.0f});
            const float z = -5.0f - stepGap;
            m.points.insert(m.points.end(), {Vec3f{-0.2f, -20.0f, z}, Vec3f{20.0f, -20.0f, z}, Vec3f{20.0f, 20.0f, z},
                                             Vec3f{-0.2f, 20.0f, z}});
            m.indices.insert(m.indices.end(), {4, 5, 6, 4, 6, 7});
            front = addMesh(scene, m, f);
        }
    }
};

struct Rendered {
    Film film;
    bool gpu = true;
};

Rendered render(Scene &scene, RenderSettings settings, const Camera &camera, int w, int h,
                std::vector<UtilityRequest> planes = {}) {
    scene.setDefaultOutline(settings.objectOutline);
    scene.setLightOverride(settings.look.lights, settings.look.autoHeadlight, camera.viewToWorld);
    scene.commit();
    if (!planes.empty())
        settings.utility = std::make_shared<const std::vector<UtilityRequest>>(std::move(planes));
    Renderer renderer;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(w, h);
    Rendered r;
    while (!renderer.converged()) {
        renderer.renderPass({});
        r.gpu = r.gpu && renderer.lastPassOnGpu();
    }
    r.film = renderer.film();
    return r;
}

RenderSettings outlineSettings(RenderDevice device, float radius = 0.04f) {
    RenderSettings s;
    s.device = device;
    s.maxSamples = 64;
    s.maxDepth = 1;
    s.outline.enable = true;
    s.objectOutline.radius = radius;
    return s;
}

const std::vector<UtilityRequest> kPlanes = {{UtilityKind::Outline, {}},
                                             {UtilityKind::OutlineContour, {}},
                                             {UtilityKind::OutlineCrease, {}},
                                             {UtilityKind::OutlineDepth, {}},
                                             {UtilityKind::OutlineId, {}},
                                             {UtilityKind::OutlineThickness, {}},
                                             {UtilityKind::OutlineColor, {}}};

// A plane's channel c averaged over the rows [y0, y1) at column x.
float planeAt(const Film &film, UtilityKind kind, int c, int x, int y0 = 30, int y1 = 90) {
    const UtilityPlane *p = const_cast<Film &>(film).findUtility(kind);
    REQUIRE(p);
    double sum = 0.0;
    for (int y = y0; y < y1; ++y) {
        const size_t i = size_t(y) * size_t(film.width) + size_t(x);
        sum += p->data[i * p->width + size_t(c)] / float(film.count[i]);
    }
    return float(sum / (y1 - y0));
}
// The line's mass across the edge: a plane's channel summed over [x0, x1).
float mass(const Film &film, UtilityKind kind, int c, int x0, int x1) {
    float m = 0.0f;
    for (int x = x0; x < x1; ++x)
        m += planeAt(film, kind, c, x);
    return m;
}
Spectrum beautyAt(const Film &film, int x, int y0 = 30, int y1 = 90) {
    Spectrum sum = spectrum(0.0f);
    for (int y = y0; y < y1; ++y)
        sum += pixelMean(film, x, y);
    return sum / float(y1 - y0);
}

} // namespace

TEST_CASE("Outline stencil geometry", "[outline]") {
    OutlineSettings s;
    CHECK(outlinePointCount(s) == 4);
    s.stencils = 3;
    CHECK(outlinePointCount(s) == 28); // 4 + 8 + 16
    s.randomPattern = true;
    CHECK(outlinePointCount(s) == 4);
    s = OutlineSettings{};

    // R = object x render radius, x 1 / (1 + t) with full depth blend (measured).
    ObjectOutline o;
    o.radius = 0.02f;
    s.radius = 0.5f;
    CHECK_THAT(outlineRadius(s, o, 4.0f), WithinAbs(0.01, 1e-7));
    s.depthBlend = 1.0f;
    CHECK_THAT(outlineRadius(s, o, 4.0f), WithinAbs(0.002, 1e-7));
    s.depthBlend = 0.5f;
    CHECK_THAT(outlineRadius(s, o, 4.0f), WithinAbs(0.006, 1e-7));
    s = OutlineSettings{};

    // Rings at R k / stencils with samples x 2^(k-1) points.
    s.stencils = 3;
    for (uint32_t k = 0; k < 28; ++k) {
        const Vec2f v = outlineOffset(s, k, 17u, 5u);
        const float r = std::sqrt(v.x * v.x + v.y * v.y);
        CHECK_THAT(r, WithinAbs(k < 4 ? 1.0 / 3.0 : k < 12 ? 2.0 / 3.0 : 1.0, 1e-5));
    }
    // The default ring's profile across a straight edge at distance d:
    // acos(d / R) / pi in expectation (Karma's measured profile).
    s.stencils = 1;
    for (float d : {0.1f, 0.5f, 0.9f}) {
        double flagged = 0.0;
        const int n = 20000;
        for (int i = 0; i < n; ++i)
            for (uint32_t k = 0; k < 4; ++k)
                flagged += outlineOffset(s, k, uint32_t(i) * 2654435761u, uint32_t(i)).x > d ? 0.25 : 0.0;
        CHECK_THAT(flagged / n, WithinAbs(std::acos(d) / kPi, 0.01));
    }
    // The random pattern: radii uniform in [0, pi/2).
    s.randomPattern = true;
    double sumR = 0.0;
    for (uint32_t i = 0; i < 4000; ++i) {
        const Vec2f v = outlineOffset(s, i % 4, i, i / 4);
        const float r = std::sqrt(v.x * v.x + v.y * v.y);
        CHECK(r < 0.5f * kPi);
        sumR += r;
    }
    CHECK_THAT(sumR / 4000.0, WithinAbs(0.25 * kPi, 0.03));
}

TEST_CASE("Outline line types and weight", "[outline]") {
    OutlineSettings s;
    OutlineMain m;
    m.P = {0.0f, 0.0f, -5.0f};
    m.N = {0.0f, 0.0f, 1.0f};
    m.rho = 0.2f;
    m.mesh = 3;
    m.instance = 1;
    const Vec3f n = m.N;
    // Contour: a miss, another object or instance.
    CHECK(outlinePointFlags(m, s, false, 0, 0, {}, {}) == kOutlineContour);
    CHECK(outlinePointFlags(m, s, true, 4, 1, {0.2f, 0.0f, -5.0f}, n) == kOutlineContour);
    CHECK(outlinePointFlags(m, s, true, 3, 0, {0.2f, 0.0f, -5.0f}, n) == kOutlineContour);
    CHECK(outlinePointFlags(m, s, true, 3, 1, {0.2f, 0.0f, -5.0f}, n) == 0);
    // Depth: off the plane by more than rho (1 + 0.5 x scale).
    CHECK(outlinePointFlags(m, s, true, 3, 1, {0.2f, 0.0f, -5.29f}, n) == 0);
    CHECK(outlinePointFlags(m, s, true, 3, 1, {0.2f, 0.0f, -5.31f}, n) == kOutlineDepth);
    s.depthScale = 2.0f;
    CHECK(outlinePointFlags(m, s, true, 3, 1, {0.2f, 0.0f, -5.39f}, n) == 0);
    s.depthScale = 1.0f;
    // Crease: the normal turns by more than the threshold (1 - cos), or the
    // point rises in front of the plane (a step towards the camera).
    const float c30 = std::cos(30.0f * kPi / 180.0f), c20 = std::cos(20.0f * kPi / 180.0f);
    CHECK(outlinePointFlags(m, s, true, 3, 1, {0.2f, 0.0f, -5.0f}, {0.5f, 0.0f, c30}) == kOutlineCrease);
    CHECK(outlinePointFlags(m, s, true, 3, 1, {0.2f, 0.0f, -5.0f}, {std::sin(20.0f * kPi / 180.0f), 0.0f, c20}) == 0);
    CHECK(outlinePointFlags(m, s, true, 3, 1, {0.2f, 0.0f, -4.9f}, n) == kOutlineCrease);
    CHECK(outlinePointFlags(m, s, true, 3, 1, {0.2f, 0.0f, -5.1f}, n) == 0);
    m.object.crease = false;
    m.object.depth = false;
    CHECK(outlinePointFlags(m, s, true, 3, 1, {0.2f, 0.0f, -4.0f}, n) == 0);

    // Weight: the share remapped from [fitMin, fitMax], times the intensity.
    CHECK(outlineWeight(s, 2, 4) == 0.5f);
    s.fitMin = 0.2f;
    s.fitMax = 0.6f;
    CHECK_THAT(outlineWeight(s, 2, 4), WithinAbs(0.75, 1e-6));
    CHECK(outlineWeight(s, 3, 4) == 1.0f);
    CHECK(outlineWeight(s, 0, 4) == 0.0f);
    s = OutlineSettings{};
    s.intensity = 0.5f;
    CHECK(outlineWeight(s, 2, 4) == 0.25f);
}

TEST_CASE("Outlines in a render", "[outline][render]") {
    const Camera camera = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, 50.0, 36.0, kW, kH);
    const float R = 0.04f / kTanPerPixel; // the stencil's radius in pixels: 8.9
    SECTION("contour: the profile's mass, the composite, the debug planes") {
        Edge edge;
        const Rendered r = render(edge.scene, outlineSettings(RenderDevice::Cpu), camera, kW, kH, kPlanes);
        // Both sides of the edge (column 80): acos(d / R) / pi each, mass R / pi.
        CHECK_THAT(mass(r.film, UtilityKind::Outline, 3, 60, 80), WithinAbs(R / kPi, 0.03 * R / kPi));
        CHECK_THAT(mass(r.film, UtilityKind::Outline, 3, 80, 100), WithinAbs(R / kPi, 0.03 * R / kPi));
        CHECK_THAT(mass(r.film, UtilityKind::OutlineContour, 3, 60, 100), WithinAbs(2.0f * R / kPi, 0.06 * R / kPi));
        // The line in the outline plane is the colour (magenta) times its weight.
        const float w = planeAt(r.film, UtilityKind::Outline, 3, 76);
        CHECK(w > 0.2f);
        CHECK_THAT(planeAt(r.film, UtilityKind::Outline, 0, 76), WithinAbs(w, 1e-5));
        CHECK_THAT(planeAt(r.film, UtilityKind::Outline, 1, 76), WithinAbs(0.0, 1e-6));
        // The beauty: (1 - w) beauty + w colour per sample.
        const Spectrum b = beautyAt(r.film, 76);
        CHECK_THAT(b.y, WithinAbs(kFront.y * (1.0f - w), 2e-3));
        CHECK_THAT(b.x, WithinAbs(kFront.x * (1.0f - w) + w, 2e-3));
        // Far from the edge: no line; the debug planes on every hit.
        CHECK(planeAt(r.film, UtilityKind::Outline, 3, 40) == 0.0f);
        CHECK_THAT(beautyAt(r.film, 40).y, WithinAbs(kFront.y, 1e-5));
        CHECK_THAT(planeAt(r.film, UtilityKind::OutlineThickness, 0, 40), WithinAbs(0.04, 1e-6));
        CHECK_THAT(planeAt(r.film, UtilityKind::OutlineColor, 2, 40), WithinAbs(1.0, 1e-6));
        const float id = planeAt(r.film, UtilityKind::OutlineId, 0, 40);
        CHECK(id > 0.0f);
        CHECK(id < 1.0f);
        CHECK(id != planeAt(r.film, UtilityKind::OutlineId, 0, 120));
    }
    SECTION("no outline AOV, no lines (Karma); apply off keeps the beauty") {
        Edge edge;
        const Rendered none = render(edge.scene, outlineSettings(RenderDevice::Cpu), camera, kW, kH,
                                     {{UtilityKind::P, {}}});
        CHECK_THAT(beautyAt(none.film, 76).y, WithinAbs(kFront.y, 1e-5));
        RenderSettings s = outlineSettings(RenderDevice::Cpu);
        s.outline.apply = false;
        Edge edge2;
        const Rendered kept = render(edge2.scene, s, camera, kW, kH, kPlanes);
        CHECK_THAT(beautyAt(kept.film, 76).y, WithinAbs(kFront.y, 1e-5));
        CHECK(planeAt(kept.film, UtilityKind::Outline, 3, 76) > 0.2f);
    }
    SECTION("depth lines from rho (1 + threshold) on, the plane test's crease on the far side") {
        // rho = R t = 0.04 x 5 = 0.2: depth lines from a gap of 0.3.
        Edge shallow(0.25f), deep(0.35f);
        const Rendered a = render(shallow.scene, outlineSettings(RenderDevice::Cpu), camera, kW, kH, kPlanes);
        const Rendered b = render(deep.scene, outlineSettings(RenderDevice::Cpu), camera, kW, kH, kPlanes);
        CHECK(mass(a.film, UtilityKind::OutlineDepth, 3, 60, 100) < 0.05f);
        CHECK_THAT(mass(b.film, UtilityKind::OutlineDepth, 3, 60, 100), WithinAbs(2.0f * R / kPi, 0.06 * R / kPi));
        // The far card's points see the near card rise in front of their
        // plane (crease); the near card's points see the far card behind.
        CHECK(mass(a.film, UtilityKind::OutlineCrease, 3, 60, 78) < 0.1f);
        CHECK_THAT(mass(a.film, UtilityKind::OutlineCrease, 3, 80, 100), WithinAbs(R / kPi, 0.05 * R / kPi));
        CHECK(mass(a.film, UtilityKind::OutlineContour, 3, 60, 100) == 0.0f);
    }
    SECTION("depth blend narrows lines by 1 / (1 + t)") {
        RenderSettings s = outlineSettings(RenderDevice::Cpu, 0.2f);
        s.outline.depthBlend = 1.0f;
        Edge edge;
        const Rendered r = render(edge.scene, s, camera, kW, kH, kPlanes);
        const float r5 = 0.2f / 6.0f / kTanPerPixel; // the front card at t = 5
        CHECK_THAT(mass(r.film, UtilityKind::Outline, 3, 60, 80), WithinAbs(r5 / kPi, 0.05 * r5 / kPi));
    }
}

namespace {

// The look-dev scene: seen from +Z by an orthographic camera (64 x 64 over
// 8 x 8 units), a card at the origin tilted by `tilt` about Y with a display
// colour and a grey Lambert material, an optional sun at 45 degrees and an
// optional camera-invisible roof `roof` above the card.
constexpr int kRes = 64;
const Spectrum kDisplay{0.8f, 0.5f, 0.2f};

struct Look {
    Scene scene;
    explicit Look(float tilt = 0.0f, bool sun = true, float roof = -1.0f, bool emit = false) {
        const float t = tilt * kPi / 180.0f;
        MeshDesc card = quad({-3.0f * std::cos(t), -3.0f, 3.0f * std::sin(t)},
                             {6.0f * std::cos(t), 0.0f, -6.0f * std::sin(t)}, {0.0f, 6.0f, 0.0f});
        card.setDisplayColor(kDisplay);
        addMesh(scene, card, emit ? emissive(scene, Spectrum{2.0f, 2.0f, 2.0f}) : lambert(scene, 0.5f));
        if (roof > 0.0f) {
            const MeshId r = addMesh(scene, quad({-50.0f, -50.0f, roof}, {100.0f, 0.0f, 0.0f}, {0.0f, 100.0f, 0.0f}),
                                     lambert(scene, 0.5f));
            uint8_t mask = 0;
            REQUIRE(parseRenderVisibility("-primary", mask));
            scene.setMeshVisibility(r, {int16_t(mask), -1});
        }
        if (sun) {
            LightDesc l;
            l.type = LightType::Distant;
            l.angleDeg = 0.0f;
            l.intensity = 2.0f;
            // Travelling along (0, -1, -1) / sqrt 2: cos 45 on the card.
            Mat4 m = Mat4::identity();
            const float h = std::sqrt(0.5f);
            m.m[1][1] = h;
            m.m[1][2] = -h;
            m.m[2][1] = h;
            m.m[2][2] = h;
            l.transform = m;
            scene.updateLight(scene.createLight(), l);
        }
    }
};

RenderSettings lookSettings(RenderDevice device) {
    RenderSettings s;
    s.device = device;
    s.maxSamples = 64;
    s.maxDepth = 1;
    return s;
}

Spectrum centre(const Film &film) {
    Spectrum sum = spectrum(0.0f);
    for (int y = 28; y < 36; ++y)
        for (int x = 28; x < 36; ++x)
            sum += pixelMean(film, x, y);
    return sum / 64.0f;
}

} // namespace

TEST_CASE("Simplified shading (Karma's disable_lighting)", "[lookdev][render]") {
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 10.0f}, 4.0f, kRes, kRes);
    RenderSettings s = lookSettings(RenderDevice::Cpu);
    s.look.simplified = true;
    s.look.aoSamples = 0;
    SECTION("display colour times the facing ratio; no lights") {
        Look look(60.0f);
        const Spectrum c = centre(render(look.scene, s, camera, kRes, kRes).film);
        CHECK_THAT(c.x, WithinAbs(kDisplay.x * 0.5f, 1e-4));
        CHECK_THAT(c.z, WithinAbs(kDisplay.z * 0.5f, 1e-4));
    }
    SECTION("depth cue fog over the hit distance, with its alpha") {
        s.look.fogNear = 5.0f;
        s.look.fogFar = 15.0f;
        s.look.fogColor[0] = 1.0f;
        s.look.fogAlpha = 0.5f; // t = 10: f = 0.5 x 0.5
        Look look;
        const Spectrum c = centre(render(look.scene, s, camera, kRes, kRes).film);
        CHECK_THAT(c.x, WithinAbs(kDisplay.x * 0.75f + 0.25f, 1e-4));
        CHECK_THAT(c.y, WithinAbs(kDisplay.y * 0.75f, 1e-4));
    }
    SECTION("occlusion within head_ao_distance; emission stays") {
        s.look.aoSamples = 1;
        Look roofed(0.0f, true, 1.0f);
        CHECK(centre(render(roofed.scene, s, camera, kRes, kRes).film).x < 0.01f);
        s.look.aoDistance = 0.9f; // the roof is 1 away
        Look roofed2(0.0f, true, 1.0f);
        CHECK_THAT(centre(render(roofed2.scene, s, camera, kRes, kRes).film).x, WithinAbs(kDisplay.x, 1e-4));
        s.look.aoSamples = 0;
        Look glowing(0.0f, true, -1.0f, true);
        CHECK_THAT(centre(render(glowing.scene, s, camera, kRes, kRes).film).x, WithinAbs(2.0f + kDisplay.x, 1e-3));
    }
}

TEST_CASE("Look-dev light overrides", "[lookdev][render]") {
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 10.0f}, 4.0f, kRes, kRes);
    RenderSettings s = lookSettings(RenderDevice::Cpu);
    const double sun = 0.5 / kPi * 2.0 * std::sqrt(0.5), head = 0.5 / kPi * kHeadlightIrradiance;
    {
        Look look;
        CHECK_THAT(centre(render(look.scene, s, camera, kRes, kRes).film).x, WithinAbs(sun, 2e-3));
    }
    s.look.lights = LightOverride::EmissiveOnly;
    {
        Look look;
        CHECK(centre(render(look.scene, s, camera, kRes, kRes).film).x == 0.0f);
    }
    s.look.lights = LightOverride::Headlight;
    {
        Look look;
        CHECK_THAT(centre(render(look.scene, s, camera, kRes, kRes).film).x, WithinAbs(head, 2e-3));
        // Karma's headlight casts no shadows (measured): a camera-invisible
        // roof between it and the card changes nothing.
        Look roofed(0.0f, true, 1.0f);
        CHECK_THAT(centre(render(roofed.scene, s, camera, kRes, kRes).film).x, WithinAbs(head, 2e-3));
    }
    s.look.lights = LightOverride::Dome;
    {
        Look look; // a unit dome on a 0.5 card: 0.5
        CHECK_THAT(centre(render(look.scene, s, camera, kRes, kRes).film).x, WithinAbs(0.5, 0.01));
    }
    s.look.lights = LightOverride::Off;
    s.look.autoHeadlight = true;
    {
        Look lit; // the stage has a light: as without
        CHECK_THAT(centre(render(lit.scene, s, camera, kRes, kRes).film).x, WithinAbs(sun, 2e-3));
        Look dark(0.0f, false); // none: the headlight
        CHECK_THAT(centre(render(dark.scene, s, camera, kRes, kRes).film).x, WithinAbs(head, 2e-3));
    }
}

TEST_CASE("XPU outlines and look-dev overrides", "[gpu][render][outline][lookdev]") {
    const Camera persp = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, 50.0, 36.0, kW, kH);
    const Camera ortho = makeOrthoCamera({0.0f, 0.0f, 10.0f}, 4.0f, kRes, kRes);
    {
        Edge probe;
        if (!render(probe.scene, outlineSettings(RenderDevice::Xpu), persp, kW, kH, kPlanes).gpu)
            SKIP("Nova XPU renders on the CPU here (no GPU)");
    }
    SECTION("outlines: the planes and the beauty") {
        for (const float gap : {-1.0f, 0.35f}) {
            INFO("gap " << gap);
            Edge c(gap), g(gap);
            RenderSettings s = outlineSettings(RenderDevice::Cpu);
            s.outline.stencils = 2;
            const Rendered rc = render(c.scene, s, persp, kW, kH, kPlanes);
            s.device = RenderDevice::Xpu;
            const Rendered rg = render(g.scene, s, persp, kW, kH, kPlanes);
            REQUIRE(rg.gpu);
            for (const UtilityKind k : {UtilityKind::Outline, UtilityKind::OutlineContour, UtilityKind::OutlineCrease,
                                        UtilityKind::OutlineDepth}) {
                const float mc = mass(rc.film, k, 3, 60, 100), mg = mass(rg.film, k, 3, 60, 100);
                CHECK_THAT(mg, WithinAbs(mc, 0.01 * mc + 0.01));
            }
            for (const int x : {70, 76, 80, 84, 90, 120}) {
                const Spectrum bc = beautyAt(rc.film, x), bg = beautyAt(rg.film, x);
                CHECK_THAT(bg.x, WithinAbs(bc.x, 0.005 * bc.x + 1e-5));
                CHECK_THAT(bg.y, WithinAbs(bc.y, 0.005 * bc.y + 1e-5));
            }
            CHECK_THAT(planeAt(rg.film, UtilityKind::OutlineId, 0, 40),
                       WithinAbs(planeAt(rc.film, UtilityKind::OutlineId, 0, 40), 1e-6));
        }
    }
    SECTION("simplified shading and the light overrides") {
        const auto both = [&](float tilt, float roof, bool emit, RenderSettings s) {
            Look c(tilt, true, roof, emit), g(tilt, true, roof, emit);
            s.device = RenderDevice::Cpu;
            const Spectrum vc = centre(render(c.scene, s, ortho, kRes, kRes).film);
            s.device = RenderDevice::Xpu;
            const Rendered rg = render(g.scene, s, ortho, kRes, kRes);
            REQUIRE(rg.gpu);
            const Spectrum vg = centre(rg.film);
            CHECK_THAT(vg.x, WithinAbs(vc.x, 0.01 * vc.x + 1e-4));
            CHECK_THAT(vg.y, WithinAbs(vc.y, 0.01 * vc.y + 1e-4));
        };
        RenderSettings s = lookSettings(RenderDevice::Cpu);
        s.look.simplified = true;
        s.look.fogNear = 5.0f;
        s.look.fogFar = 15.0f;
        s.look.fogColor[0] = 1.0f;
        s.look.aoDistance = 5.0f;
        both(30.0f, 1.5f, false, s); // occluded in part by the roof
        both(0.0f, -1.0f, true, s);
        s = lookSettings(RenderDevice::Cpu);
        s.look.lights = LightOverride::Headlight;
        both(30.0f, -1.0f, false, s);
        both(0.0f, 1.0f, false, s); // no shadows from the roof
        s.look.lights = LightOverride::Dome;
        both(30.0f, -1.0f, false, s);
        s.look.lights = LightOverride::EmissiveOnly;
        both(0.0f, -1.0f, true, s);
    }
}
