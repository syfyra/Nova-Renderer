// M11 P2 (docs/design/M11-P2.md): per-object ray visibility (Karma's
// rendervisibility) and holdouts (holdoutmode Matte and Background) in the
// renderer, measured against Karma's behaviour: what camera rays and the
// other rays see, alpha, the holdout; light path expressions, pre-lit
// background objects and bgoccludeshadow; Nova XPU against the CPU.

#include "test_helpers.h"

#include "core/film/lpe_compile.h"
#include "core/integrator/renderer.h"
#include "core/scene/scene.h"
#include "core/scene/visibility.h"
#include "core/shading/compiler.h"
#include "core/volume/shader.h"
#include "core/volume/shell.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
namespace sf = nova::surface_field;

namespace {

constexpr int kRes = 64;          // pixels over 8 x 8 units (ortho, from +Z)
constexpr float kSun = 2.0f;      // irradiance of the sun at normal incidence

// The light travelling along `t` (a unit vector), delta.
LightDesc sun(Vec3f t, float intensity = kSun) {
    LightDesc l;
    l.type = LightType::Distant;
    l.angleDeg = 0.0f;
    l.intensity = intensity;
    // Rows: X, Y, Z axes of the light; its local -Z travels along t.
    const Vec3f z = -t;
    const Vec3f a = std::fabs(z.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 1.0f, 0.0f};
    const Vec3f x = normalize(cross(a, z));
    const Vec3f y = cross(z, x);
    Mat4 m = Mat4::identity();
    const Vec3f rows[3] = {x, y, z};
    for (int r = 0; r < 3; ++r) {
        m.m[r][0] = rows[r].x;
        m.m[r][1] = rows[r].y;
        m.m[r][2] = rows[r].z;
    }
    l.transform = m;
    return l;
}

MeshId addMesh(Scene &scene, MeshDesc desc, MaterialId material = kInvalidId) {
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, std::move(desc));
    if (material != kInvalidId)
        scene.setMeshMaterials(id, {material});
    return id;
}

// A quad from a corner and two edges, normal along e1 x e2.
MeshDesc quad(Vec3f o, Vec3f e1, Vec3f e2) {
    MeshDesc m;
    m.points = {o, o + e1, o + e1 + e2, o + e2};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

// The ground scene: a grey ground (z = 0, facing +Z), a grey ball of radius
// 1 on it at the origin and a sun at 45 degrees towards +Y (the ball's
// shadow falls on y in [1, 2.41] past the ball), seen from above.
struct Ground {
    Scene scene;
    MeshId ground = 0, ball = 0;
    MaterialId grey = 0;
    Ground() {
        grey = scene.createMaterial();
        MaterialDesc m;
        m.inputs = lambertDefaults(spectrum(0.5f));
        scene.updateMaterial(grey, m);
        ground = addMesh(scene, quad({-6.0f, -6.0f, 0.0f}, {12.0f, 0.0f, 0.0f}, {0.0f, 12.0f, 0.0f}), grey);
        MeshDesc b = makeSphere(1.0f, 64, 32);
        b.transform = translation({0.0f, 0.0f, 1.0f});
        ball = addMesh(scene, b, grey);
        scene.updateLight(scene.createLight(), sun(normalize(Vec3f{0.0f, 1.0f, -1.0f})));
    }
};

// The mirror scene: a 45 degree mirror sends the camera's rays along +Y onto
// a grey card at y = 6 (facing -Y) lit by a sun travelling along (0, 0.6,
// -0.8) (cosine 0.6 on the card); an optional caster ball shadows the card
// around (0, 6, 0).
struct Mirror {
    Scene scene;
    MeshId card = 0;
    MaterialId grey = 0;
    explicit Mirror(bool caster) {
        grey = scene.createMaterial();
        MaterialDesc g;
        g.inputs = lambertDefaults(spectrum(0.5f));
        scene.updateMaterial(grey, g);
        const MaterialId mirror = scene.createMaterial();
        MaterialDesc m;
        m.inputs = standardSurfaceDefaults();
        m.inputs.set(sf::baseColor, spectrum(1.0f));
        m.inputs.set(sf::metalness, 1.0f);
        m.inputs.set(sf::specularRoughness, 0.0f);
        scene.updateMaterial(mirror, m);
        // Normal (0, 1, 1) / sqrt(2): the plane y + z = 0.
        addMesh(scene, quad({-5.0f, -5.0f, 5.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 10.0f, -10.0f}), mirror);
        card = addMesh(scene, quad({-5.0f, 6.0f, -5.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 10.0f}), grey);
        if (caster) {
            // On the light's way to (0, 6, 0), out of the reflected rays of
            // the regions below (the camera sees it around y = 3.6).
            MeshDesc b = makeSphere(0.6f, 48, 24);
            b.transform = translation({0.0f, 3.6f, 3.2f});
            addMesh(scene, b, grey);
        }
        scene.updateLight(scene.createLight(), sun(normalize(Vec3f{0.0f, 0.6f, -0.8f})));
    }
};

struct Result {
    Film film;
    std::shared_ptr<const lpe::Compiled> lpe;
    bool gpu = false;
};

// maxDepth 1: direct light only (the ground scene's values are analytic);
// the mirror scene needs 2.
Result render(Scene &scene, std::vector<std::string> const &expressions, RenderDevice device = RenderDevice::Cpu,
              uint32_t maxDepth = 1, const ObjectVisibility &defaults = {}, bool bgOcclude = false) {
    scene.setDefaultVisibility(defaults);
    scene.setBackgroundOccludesShadows(bgOcclude);
    scene.commit();
    Result r;
    Renderer renderer;
    RenderSettings settings;
    settings.device = device;
    settings.maxSamples = 64;
    settings.maxDepth = maxDepth;
    if (!expressions.empty()) {
        r.lpe = std::make_shared<lpe::Compiled>(lpe::compile(expressions));
        REQUIRE(r.lpe->ok());
        settings.lpe = r.lpe;
    }
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 10.0f}, 4.0f, kRes, kRes));
    renderer.resize(kRes, kRes);
    r.gpu = true;
    while (!renderer.converged()) {
        renderer.renderPass({});
        r.gpu = r.gpu && renderer.lastPassOnGpu();
    }
    r.film = renderer.film();
    return r;
}

// Means over the pixels within h of (x, y) (world units, image y up).
struct Region {
    float x, y, h;
};
template <class F> double mean(const Film &, Region r, F &&value) {
    double sum = 0.0;
    int n = 0;
    for (int py = 0; py < kRes; ++py)
        for (int px = 0; px < kRes; ++px) {
            const float wx = (float(px) + 0.5f) / kRes * 8.0f - 4.0f, wy = (float(py) + 0.5f) / kRes * 8.0f - 4.0f;
            if (std::fabs(wx - r.x) > r.h || std::fabs(wy - r.y) > r.h)
                continue;
            const size_t i = size_t(py) * kRes + size_t(px);
            sum += value(i);
            ++n;
        }
    return n ? sum / n : 0.0;
}
double color(const Result &r, Region g) {
    return mean(r.film, g, [&](size_t i) { return double(r.film.colorSum[i].x) / r.film.count[i]; });
}
double alpha(const Result &r, Region g) {
    return mean(r.film, g, [&](size_t i) { return double(r.film.alphaSum[i]) / r.film.count[i]; });
}
double lpeValue(const Result &r, uint32_t o, Region g) {
    return mean(r.film, g, [&](size_t i) { return double(r.film.lpe(o)[i].x) / r.film.count[i]; });
}
// A shadow; output's value, 1 - lit / unoccluded over the region (holdout
// shadow; outputs with the background points' whole light in both, D14).
double shadowValue(const Result &r, uint32_t o, Region g) {
    const int32_t slot = r.lpe->shadowSlot(o);
    REQUIRE(slot >= 0);
    const int32_t total = r.lpe->holdoutShadow(o) ? r.lpe->holdoutTotalSlot() : -1;
    const auto whole = [&](size_t i) { return total >= 0 ? double(r.film.lpeLitSum(uint32_t(total))[i].x) : 0.0; };
    const double all = mean(r.film, g, [&](size_t i) { return double(r.film.lpe(o)[i].x) + whole(i); });
    const double lit =
        mean(r.film, g, [&](size_t i) { return double(r.film.lpeLitSum(uint32_t(slot))[i].x) + whole(i); });
    return all > 0.0 ? 1.0 - lit / all : 0.0;
}

const Region kBall{0.0f, 0.0f, 0.3f}, kShadow{0.0f, 1.8f, 0.2f}, kOpen{2.5f, -2.5f, 0.4f};
// The ground lit by the sun at 45 degrees: 0.5 / pi * 2 * cos 45.
const double kLitGround = 0.5 / kPi * kSun * std::sqrt(0.5);

// The ground scene under a constant unit dome instead of the sun.
void domeOnly(Ground &g) {
    g.scene.removeLight(0);
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.intensity = 1.0f;
    g.scene.updateLight(g.scene.createLight(), dome);
}

// A coated grey ground material: Standard Surface, base 0.5, specular 1,
// roughness 0.3 (Karma's spec probes, docs/design/M11-P2.md D14).
MaterialId coatedGrey(Scene &scene) {
    const MaterialId id = scene.createMaterial();
    MaterialDesc m;
    m.inputs = standardSurfaceDefaults();
    m.inputs.set(sf::baseColor, spectrum(0.5f));
    m.inputs.set(sf::specularRoughness, 0.3f);
    scene.updateMaterial(id, m);
    return id;
}

// A medium in a box over the ground (D15): x in [-3.5, -1.5], y in [-3.5,
// -1.5], z in [0.5, 1.5]; black (absorbing 1 per unit) or, without a
// material, white of density 1. The camera sees the ground under it through
// 1 unit (kUnder); the sun reaches kUnder through 0 to sqrt 2 of it (with
// the region's y), kBoxShadow, beside it, through about 0.6 sqrt 2.
const Region kUnder{-2.5f, -2.5f, 0.5f}, kBoxShadow{-2.5f, -0.6f, 0.1f};
VolumeId addFog(Scene &scene, bool white) {
    const Vec3f lo{-3.5f, -3.5f, 0.5f}, hi{-1.5f, -1.5f, 1.5f};
    const std::vector<Vec3f> p = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                                  {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    const uint32_t quads[6][4] = {{0, 3, 2, 1}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}, {4, 5, 6, 7}};
    std::vector<uint32_t> idx;
    for (const auto &q : quads)
        idx.insert(idx.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
    VolumeDesc desc;
    desc.name = "fog";
    desc.shell = makeTriangleShell(p.data(), p.size(), idx.data(), idx.size() / 3);
    if (!white) {
        shading::IrGraph g;
        shading::IrBuilder b(g);
        const auto field = [](uint16_t f) { return uint16_t(volume_field::kBase + f); };
        b.output(field(volume_field::scattering), 3, b.constant(0.0f, 0.0f, 0.0f));
        b.output(field(volume_field::absorption), 3, b.constant(1.0f, 1.0f, 1.0f));
        MaterialDesc m;
        m.program = shading::compile(g).program;
        desc.material = scene.createMaterial();
        scene.updateMaterial(desc.material, m);
    }
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    scene.updateVolumeMotion(id, {Mat4::identity()});
    return id;
}

ObjectVisibility holdout(HoldoutMode m) { return {-1, int8_t(m)}; }
ObjectVisibility visible(const char *expression) {
    uint8_t mask = 0;
    REQUIRE(parseRenderVisibility(expression, mask));
    return {int16_t(mask), -1};
}

} // namespace

TEST_CASE("rendervisibility expressions parse as Karma's menu means them", "[holdout][visibility]") {
    const auto mask = [](const char *text) {
        uint8_t m = 0xFF;
        std::string error;
        INFO(text);
        REQUIRE(parseRenderVisibility(text, m, &error));
        return int(m);
    };
    CHECK(mask("*") == kVisibleAll);
    CHECK(mask("primary") == kVisibleCamera);
    CHECK(mask("primary|shadow") == (kVisibleCamera | kVisibleShadow));
    CHECK(mask("-primary") == (kVisibleAll & ~kVisibleCamera));
    CHECK(mask("-diffuse") == (kVisibleAll & ~kVisibleDiffuse));
    CHECK(mask("-diffuse&-reflect&-refract&-volume&-sss") == (kVisibleCamera | kVisibleShadow));
    CHECK(mask("-shadow") == (kVisibleAll & ~kVisibleShadow));
    CHECK(mask("") == 0);
    CHECK(mask("  ") == 0);
    // Pattern lists: plain terms add, negated ones remove.
    CHECK(mask("* -primary") == (kVisibleAll & ~kVisibleCamera));
    CHECK(mask("primary shadow") == (kVisibleCamera | kVisibleShadow));
    CHECK(mask("-primary -shadow") == (kVisibleAll & ~(kVisibleCamera | kVisibleShadow)));
    CHECK(mask("(primary | reflect) & -reflect") == kVisibleCamera);
    CHECK(mask("PRIMARY|Shadow") == (kVisibleCamera | kVisibleShadow));
    uint8_t m = 0;
    std::string error;
    CHECK_FALSE(parseRenderVisibility("primary|", m, &error));
    CHECK_FALSE(parseRenderVisibility("glossy", m, &error));
    CHECK(error.find("glossy") != std::string::npos);
    CHECK_FALSE(parseRenderVisibility("(primary", m, &error));

    HoldoutMode h = HoldoutMode::None;
    CHECK((parseHoldoutMode("Matte", h) && h == HoldoutMode::Matte));
    CHECK((parseHoldoutMode("2", h) && h == HoldoutMode::Background));
    CHECK((parseHoldoutMode(" background ", h) && h == HoldoutMode::Background));
    CHECK((parseHoldoutMode("None", h) && h == HoldoutMode::None));
    CHECK_FALSE(parseHoldoutMode("3", h));
}

TEST_CASE("holdout; expressions see only the paths of holdouts", "[holdout][lpe]") {
    const lpe::Compiled c = lpe::compile({"C.*[LO]", "holdout;C.*[LO]", "holdouts;shadow;CDL", "C<RD>L"});
    REQUIRE(c.ok());
    CHECK(c.holdoutStart != 0);
    using lpe::Event;
    const Event C{lpe::TypeCamera}, D{lpe::TypeReflect, lpe::ScatterDiffuse}, L{lpe::TypeLight},
        H{lpe::TypeHoldout};
    const auto outputs = [&](std::vector<Event> const &events) {
        const lpe::State s = c.run(events);
        const lpe::Program p = c.program();
        return std::vector<uint16_t>(p.outputs(s), p.outputs(s) + p.outputCount(s));
    };
    CHECK(outputs({C, D, L}) == std::vector<uint16_t>{0, 3});
    CHECK(outputs({H, C, D, L}) == std::vector<uint16_t>{1, 2});
    CHECK(outputs({H, C, D, D, L}) == std::vector<uint16_t>{1});
    // Without holdout; outputs a holdout path reaches none.
    const lpe::Compiled plain = lpe::compile({"C.*[LO]", "C.*"});
    REQUIRE(plain.ok());
    CHECK(plain.holdoutStart == 0);
    CHECK(plain.run({H, C, D, L}) == 0);
}

TEST_CASE("Ray visibility hides objects from the ray classes it leaves out", "[holdout][visibility][render]") {
    const std::vector<std::string> lpes = {"C<RD>L"};
    Ground base;
    const Result b = render(base.scene, lpes);
    CHECK_THAT(color(b, kOpen), WithinAbs(kLitGround, 0.02 * kLitGround));
    CHECK(color(b, kShadow) < 0.02 * kLitGround);
    CHECK(alpha(b, kBall) == 1.0);

    SECTION("-primary: the camera sees the ground under the ball, its shadow stays") {
        Ground g;
        g.scene.setMeshVisibility(g.ball, visible("-primary"));
        const Result r = render(g.scene, lpes);
        CHECK(color(r, kBall) < 0.02 * kLitGround); // the ground below, in the ball's shadow
        CHECK(alpha(r, kBall) == 1.0);
        CHECK(color(r, kShadow) < 0.02 * kLitGround);
    }
    SECTION("-shadow: no shadow, the ball still seen") {
        Ground g;
        g.scene.setMeshVisibility(g.ball, visible("-shadow"));
        const Result r = render(g.scene, lpes);
        CHECK_THAT(color(r, kShadow), WithinAbs(kLitGround, 0.02 * kLitGround));
        CHECK_THAT(color(r, kBall), WithinAbs(color(b, kBall), 0.02 * color(b, kBall)));
    }
    SECTION("an empty expression hides the object from every ray") {
        Ground g;
        g.scene.setMeshVisibility(g.ball, visible(""));
        const Result r = render(g.scene, lpes);
        CHECK_THAT(color(r, kBall), WithinAbs(kLitGround, 0.02 * kLitGround));
        CHECK_THAT(color(r, kShadow), WithinAbs(kLitGround, 0.02 * kLitGround));
    }
    SECTION("the render settings' default applies to objects without their own") {
        Ground g;
        g.scene.setMeshVisibility(g.ground, visible("*"));
        const Result r = render(g.scene, lpes, RenderDevice::Cpu, 1, visible("-shadow"));
        CHECK_THAT(color(r, kShadow), WithinAbs(kLitGround, 0.02 * kLitGround));
    }
    SECTION("-reflect: a mirror sees through the card") {
        Mirror m(false);
        const Result normal = render(m.scene, {}, RenderDevice::Cpu, 2);
        // 0.5 / pi * 2 * 0.6 on the card, times the mirror's reflectance.
        CHECK(color(normal, {0.0f, 1.0f, 1.0f}) > 0.9 * 0.5 / kPi * kSun * 0.6);
        m.scene.setMeshVisibility(m.card, visible("-reflect"));
        const Result r = render(m.scene, {}, RenderDevice::Cpu, 2);
        CHECK(color(r, {0.0f, 1.0f, 1.0f}) == 0.0);
    }
}

TEST_CASE("The light scattered rays reach is tested with shadow visibility", "[holdout][visibility][render]") {
    // Karma (measured, docs/design/M11-P2.md): under a constant dome, an
    // object invisible to shadow rays takes nothing from the ground's light
    // (Nova finds a constant dome through the scattered rays alone); the
    // ground still sees the object in its indirect light.
    const Region near{1.2f, 0.0f, 0.1f};
    Ground base;
    domeOnly(base);
    const Result b = render(base.scene, {}, RenderDevice::Cpu, 1);
    const double open = color(b, kOpen);
    CHECK_THAT(open, WithinAbs(0.5, 0.02)); // albedo 0.5 under a unit dome
    CHECK(color(b, near) < 0.9 * open);
    Ground g;
    domeOnly(g);
    g.scene.setMeshVisibility(g.ball, visible("-shadow"));
    const Result r = render(g.scene, {}, RenderDevice::Cpu, 1);
    CHECK_THAT(color(r, near), WithinAbs(open, 0.03 * open));
    // The other way round: a ground visible to shadow rays only blocks the
    // dome below it for the ball's scattered rays, as an ordinary ground.
    Ground h, hb;
    domeOnly(h);
    domeOnly(hb);
    h.scene.setMeshVisibility(h.ground, visible("primary|shadow"));
    const Result rh = render(h.scene, {}, RenderDevice::Cpu, 1);
    const Result rb = render(hb.scene, {}, RenderDevice::Cpu, 1);
    const Region side{0.95f, 0.0f, 0.04f}; // the ball's side, seen from above
    CHECK_THAT(color(rh, side), WithinAbs(color(rb, side), 0.03 * color(rb, side)));
}

TEST_CASE("Holdouts: the camera's cut-outs and what other rays see", "[holdout][render]") {
    const std::vector<std::string> lpes = {"holdout;C.*[LO]", "holdouts;shadow;CDL", "C.*[LO]", "holdouts;CD.+L"};
    Ground base;
    const Result b = render(base.scene, lpes);
    const double ballLit = color(b, kBall);
    CHECK(ballLit > 0.1);

    SECTION("Matte: colour and alpha 0 for the camera, its light in holdout; outputs, its shadow kept") {
        Ground g;
        g.scene.setMeshVisibility(g.ball, holdout(HoldoutMode::Matte));
        const Result r = render(g.scene, lpes);
        CHECK(color(r, kBall) == 0.0);
        CHECK(alpha(r, kBall) == 0.0);
        CHECK(lpeValue(r, 2, kBall) == 0.0);
        CHECK_THAT(lpeValue(r, 0, kBall), WithinAbs(ballLit, 0.02 * ballLit));
        CHECK(color(r, kShadow) < 0.02 * kLitGround);
        CHECK_THAT(color(r, kOpen), WithinAbs(kLitGround, 0.02 * kLitGround));
        CHECK(alpha(r, kOpen) == 1.0);
        CHECK(lpeValue(r, 0, kOpen) == 0.0);
    }
    SECTION("Background ground: alpha 0, the shadow fraction in holdouts;shadow;") {
        Ground g;
        g.scene.setMeshVisibility(g.ground, holdout(HoldoutMode::Background));
        const Result r = render(g.scene, lpes);
        CHECK(color(r, kOpen) == 0.0);
        CHECK(alpha(r, kOpen) == 0.0);
        CHECK_THAT(shadowValue(r, 1, kShadow), WithinAbs(1.0, 1e-6));
        CHECK_THAT(shadowValue(r, 1, kOpen), WithinAbs(0.0, 1e-6));
        CHECK_THAT(lpeValue(r, 0, kOpen), WithinAbs(kLitGround, 0.02 * kLitGround));
        CHECK_THAT(color(r, kBall), WithinAbs(ballLit, 0.05 * ballLit));
        CHECK(alpha(r, kBall) == 1.0);
    }
    SECTION("The render settings' holdout mode applies to objects without their own") {
        Ground g;
        g.scene.setMeshVisibility(g.ball, holdout(HoldoutMode::None));
        const Result r = render(g.scene, lpes, RenderDevice::Cpu, 1, holdout(HoldoutMode::Matte));
        CHECK(alpha(r, kOpen) == 0.0);
        CHECK(alpha(r, kBall) == 1.0);
    }
}

TEST_CASE("Background holdouts look pre-lit to rays other than the camera's", "[holdout][render]") {
    const Region card{0.0f, 1.5f, 0.3f};   // the card at (0, 6, -1.5), unshadowed
    const Region shaded{0.0f, 0.0f, 0.15f}; // the card at (0, 6, 0), in the caster's shadow
    Mirror normal(true);
    const Result n = render(normal.scene, {}, RenderDevice::Cpu, 2);
    const double lit = color(n, card);
    // The mirror's reflectance: the lit card is 0.5 / pi * 2 * 0.6.
    const double reflectance = lit / (0.5 / kPi * kSun * 0.6);
    CHECK(reflectance > 0.9);
    CHECK(color(n, shaded) < 0.01 * lit);

    Mirror m(true);
    m.scene.setMeshVisibility(m.card, holdout(HoldoutMode::Background));
    const Result r = render(m.scene, {"C<RG>O", "C.*[LO]"}, RenderDevice::Cpu, 2);
    // Its diffuse albedo, in the fraction of the light that reaches it: an
    // O event seen through the mirror.
    CHECK_THAT(color(r, card), WithinAbs(0.5 * reflectance, 0.01));
    CHECK(color(r, shaded) < 0.01);
    CHECK_THAT(lpeValue(r, 0, card), WithinAbs(0.5 * reflectance, 0.01));

    SECTION("Matte objects look as they are") {
        Mirror mm(true);
        mm.scene.setMeshVisibility(mm.card, holdout(HoldoutMode::Matte));
        const Result rm = render(mm.scene, {}, RenderDevice::Cpu, 2);
        CHECK_THAT(color(rm, card), WithinAbs(lit, 0.02 * lit));
    }
    SECTION("illumbackground lights light them as well and leave the shadow fraction alone") {
        Mirror mi(true);
        mi.scene.setMeshVisibility(mi.card, holdout(HoldoutMode::Background));
        LightDesc l = sun(normalize(Vec3f{0.0f, 0.6f, -0.8f}));
        l.illumBackground = true;
        mi.scene.updateLight(0, l);
        const Result ri = render(mi.scene, {}, RenderDevice::Cpu, 2);
        CHECK_THAT(color(ri, card), WithinAbs(0.5 * reflectance + lit, 0.02));
        CHECK_THAT(color(ri, shaded), WithinAbs(0.5 * reflectance, 0.02)); // pre-lit unshadowed, the light blocked
    }
}

TEST_CASE("Background holdouts do not shadow background points (bgoccludeshadow)", "[holdout][render]") {
    // A background box between the sun and part of the ball's shadow.
    const auto scene = [](Ground &g) {
        g.scene.setMeshVisibility(g.ground, holdout(HoldoutMode::Background));
        MeshDesc box = quad({0.2f, 0.8f, 0.0f}, {1.8f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.5f}); // a wall at y = 0.8, facing -Y
        const MeshId wall = addMesh(g.scene, box, g.grey);
        g.scene.setMeshVisibility(wall, holdout(HoldoutMode::Background));
    };
    const std::vector<std::string> lpes = {"holdouts;shadow;CDL"};
    const Region both{0.6f, 1.6f, 0.15f}, wallOnly{1.6f, 1.6f, 0.15f}, ballOnly{-0.5f, 1.6f, 0.15f};
    Ground off;
    scene(off);
    const Result a = render(off.scene, lpes);
    CHECK_THAT(shadowValue(a, 0, both), WithinAbs(1.0, 1e-6));
    CHECK_THAT(shadowValue(a, 0, wallOnly), WithinAbs(0.0, 1e-6));
    CHECK_THAT(shadowValue(a, 0, ballOnly), WithinAbs(1.0, 1e-6));
    Ground on;
    scene(on);
    const Result b = render(on.scene, lpes, RenderDevice::Cpu, 1, {}, true);
    CHECK_THAT(shadowValue(b, 0, both), WithinAbs(0.0, 1e-6));
    CHECK_THAT(shadowValue(b, 0, wallOnly), WithinAbs(0.0, 1e-6));
    CHECK_THAT(shadowValue(b, 0, ballOnly), WithinAbs(1.0, 1e-6));
}

TEST_CASE("Background holdouts' shadow; outputs share the point's whole light (Karma)", "[holdout][render]") {
    // A coated ground under the sun alone: in the umbra every lobe's light
    // is blocked. Karma divides a background point's occluded light by its
    // whole unoccluded light (D14): the diffuse and glossy shadows are their
    // lobes' shares and add up to 1; a Matte point's are 1 each.
    const std::vector<std::string> lpes = {"holdouts;shadow;C<RD>L", "holdouts;shadow;C<RG>L", "holdouts;shadow;C.L",
                                           "holdouts;unoccluded;C.L"};
    const auto build = [](Ground &g, HoldoutMode mode) {
        g.scene.setMeshMaterials(g.ground, {coatedGrey(g.scene)});
        g.scene.setMeshVisibility(g.ground, holdout(mode));
    };
    Ground bg;
    build(bg, HoldoutMode::Background);
    const Result b = render(bg.scene, lpes);
    const double diffuse = shadowValue(b, 0, kShadow), glossy = shadowValue(b, 1, kShadow);
    CHECK(diffuse > 0.8);
    CHECK(diffuse < 1.0);
    CHECK(glossy > 0.0);
    CHECK_THAT(diffuse + glossy, WithinAbs(1.0, 1e-4));
    CHECK_THAT(shadowValue(b, 2, kShadow), WithinAbs(1.0, 1e-5));
    CHECK_THAT(shadowValue(b, 2, kOpen), WithinAbs(0.0, 1e-5));

    SECTION("Matte holdouts keep each output's own light") {
        Ground mg;
        build(mg, HoldoutMode::Matte);
        const Result m = render(mg.scene, lpes);
        CHECK_THAT(shadowValue(m, 0, kShadow), WithinAbs(1.0, 1e-6));
        CHECK_THAT(shadowValue(m, 1, kShadow), WithinAbs(1.0, 1e-6));
    }
    SECTION("illumbackground lights stay in unoccluded; outputs and out of shadow; outputs") {
        Ground ig;
        build(ig, HoldoutMode::Background);
        LightDesc l = sun(normalize(Vec3f{0.0f, 1.0f, -1.0f}));
        l.illumBackground = true;
        ig.scene.updateLight(0, l);
        const Result r = render(ig.scene, lpes);
        CHECK_THAT(shadowValue(r, 2, kShadow), WithinAbs(0.0, 1e-6));
        const double unoccluded = lpeValue(b, 3, kShadow);
        CHECK(unoccluded > 0.8 * kLitGround);
        CHECK_THAT(lpeValue(r, 3, kShadow), WithinAbs(unoccluded, 1e-6 + 1e-5 * unoccluded));
    }
}

TEST_CASE("Volumes take ray visibility and holdout modes (Karma)", "[holdout][visibility][volume][render]") {
    // The sun's transmittance over kUnder: the mean of e^-x for x uniform in
    // [0, sqrt 2].
    const double through = std::exp(-1.0), lightUnder = (1.0 - std::exp(-std::sqrt(2.0))) / std::sqrt(2.0),
                 lightBeside = std::exp(-0.6 * std::sqrt(2.0));
    const auto fogged = [](Ground &g, ObjectVisibility v, bool white = false) {
        g.scene.setVolumeVisibility(addFog(g.scene, white), v);
    };
    Ground base;
    fogged(base, {});
    const Result b = render(base.scene, {"holdout;C.*[LO]"});
    CHECK_THAT(color(b, kUnder), WithinAbs(kLitGround * lightUnder * through, 0.05 * kLitGround * through));
    CHECK_THAT(alpha(b, kUnder), WithinAbs(1.0, 1e-6));
    CHECK_THAT(color(b, kBoxShadow), WithinAbs(kLitGround * lightBeside, 0.05 * kLitGround * lightBeside));

    for (HoldoutMode mode : {HoldoutMode::Matte, HoldoutMode::Background}) {
        INFO("holdout mode " << int(mode));
        // The camera sees what lies behind as before; the volume leaves
        // alpha, and its shadow stays on the ground.
        Ground g;
        fogged(g, holdout(mode));
        const Result r = render(g.scene, {"holdout;C.*[LO]"});
        CHECK_THAT(color(r, kUnder), WithinAbs(color(b, kUnder), 0.05 * color(b, kUnder)));
        CHECK_THAT(alpha(r, kUnder), WithinAbs(through, 0.03));
        CHECK_THAT(color(r, kBoxShadow), WithinAbs(color(b, kBoxShadow), 0.03 * color(b, kBoxShadow)));
        CHECK(alpha(r, kOpen) == 1.0);
    }
    SECTION("rendervisibility") {
        Ground hidden, noShadow, none;
        fogged(hidden, visible("-primary"));
        fogged(noShadow, visible("-shadow"));
        fogged(none, visible(""));
        const Result h = render(hidden.scene, {}), s = render(noShadow.scene, {}), n = render(none.scene, {});
        CHECK_THAT(color(h, kUnder), WithinAbs(kLitGround * lightUnder, 0.05 * kLitGround * lightUnder));
        CHECK_THAT(alpha(h, kUnder), WithinAbs(1.0, 1e-6));
        CHECK_THAT(color(s, kUnder), WithinAbs(kLitGround * through, 0.05 * kLitGround * through));
        CHECK_THAT(color(s, kBoxShadow), WithinAbs(kLitGround, 0.02 * kLitGround));
        CHECK_THAT(color(n, kUnder), WithinAbs(kLitGround, 0.02 * kLitGround));
    }
    SECTION("background volumes do not shadow background points") {
        Ground bgFog, matteFog;
        bgFog.scene.setMeshVisibility(bgFog.ground, holdout(HoldoutMode::Background));
        matteFog.scene.setMeshVisibility(matteFog.ground, holdout(HoldoutMode::Background));
        fogged(bgFog, holdout(HoldoutMode::Background));
        fogged(matteFog, holdout(HoldoutMode::Matte));
        const Result r = render(bgFog.scene, {"holdouts;shadow;C.L"}), m = render(matteFog.scene, {"holdouts;shadow;C.L"});
        CHECK_THAT(shadowValue(r, 0, kBoxShadow), WithinAbs(0.0, 1e-6));
        CHECK_THAT(shadowValue(m, 0, kBoxShadow), WithinAbs(1.0 - lightBeside, 0.05));
    }
    SECTION("a scattering holdout volume's light goes to holdout; outputs") {
        // Karma: the camera ray's events in the volume are holdout paths
        // (beauty + holdout; = the plain render's beauty for Matte); a
        // Background volume's are lit without its own shadow, so brighter.
        // Single scattering of the sun, isotropic (1 / 4 pi) at density 1
        // over 1 unit: 2 / (4 pi) times the integral of e^-t over [0, 1],
        // with the volume's own shadow (through the top: e^-sqrt2 t, where
        // the sun leaves through the top, y >= -2.5) 0.0601, without it
        // 0.1006.
        const Region top{-2.5f, -2.2f, 0.25f};
        const std::vector<std::string> lpes = {"holdout;C.*[LO]", "C.*V.*[LO]"};
        Ground plain, matte, back;
        fogged(plain, {}, true);
        fogged(matte, holdout(HoldoutMode::Matte), true);
        fogged(back, holdout(HoldoutMode::Background), true);
        const Result p = render(plain.scene, lpes);
        const Result m = render(matte.scene, lpes);
        const Result k = render(back.scene, lpes);
        const double shadowed = kSun / (4.0 * kPi) * (1.0 - std::exp(-1.0 - std::sqrt(2.0))) / (1.0 + std::sqrt(2.0));
        const double free = kSun / (4.0 * kPi) * (1.0 - std::exp(-1.0));
        CHECK_THAT(lpeValue(p, 1, top), WithinAbs(shadowed, 0.05 * shadowed));
        CHECK_THAT(lpeValue(m, 0, top), WithinAbs(shadowed, 0.05 * shadowed));
        CHECK(lpeValue(m, 1, top) < 0.01 * shadowed);
        CHECK_THAT(color(m, kUnder) + lpeValue(m, 0, kUnder), WithinAbs(color(p, kUnder), 0.03 * color(p, kUnder)));
        CHECK_THAT(lpeValue(k, 0, top), WithinAbs(free, 0.05 * free));
        CHECK_THAT(alpha(m, kUnder), WithinAbs(through, 0.03));
    }
}

TEST_CASE("XPU holdouts and ray visibility", "[gpu][render][holdout]") {
    {
        Ground probe;
        if (!render(probe.scene, {}, RenderDevice::Xpu).gpu)
            SKIP("Nova XPU renders on the CPU here (no GPU)");
    }
    const std::vector<std::string> lpes = {"holdout;C.*[LO]", "holdouts;shadow;CDL", "C<RD>L", "holdouts;CD.+L",
                                           "C<RG>O"};
    // Two bounces: pre-lit background objects reach the camera through the ground.
    const auto compare = [&](const char *what, auto &&build, bool bgOcclude = false) {
        INFO(what);
        Ground cpuScene, gpuScene;
        build(cpuScene);
        build(gpuScene);
        const Result c = render(cpuScene.scene, lpes, RenderDevice::Cpu, 3, {}, bgOcclude);
        const Result g = render(gpuScene.scene, lpes, RenderDevice::Xpu, 3, {}, bgOcclude);
        REQUIRE(g.gpu);
        for (const Region region : {kBall, kShadow, kOpen}) {
            CHECK_THAT(color(g, region), WithinAbs(color(c, region), 0.01 * color(c, region) + 1e-5));
            CHECK_THAT(alpha(g, region), WithinAbs(alpha(c, region), 1e-6));
            for (uint32_t o = 0; o < uint32_t(lpes.size()); ++o)
                CHECK_THAT(lpeValue(g, o, region), WithinAbs(lpeValue(c, o, region), 0.01 * lpeValue(c, o, region) + 1e-5));
            CHECK_THAT(shadowValue(g, 1, region), WithinAbs(shadowValue(c, 1, region), 1e-4));
        }
    };
    compare("matte ball", [](Ground &g) { g.scene.setMeshVisibility(g.ball, holdout(HoldoutMode::Matte)); });
    compare("background ground", [](Ground &g) {
        g.scene.setMeshVisibility(g.ground, holdout(HoldoutMode::Background));
    });
    compare("background ball, pre-lit to the ground", [](Ground &g) {
        g.scene.setMeshVisibility(g.ball, holdout(HoldoutMode::Background));
    });
    compare("phantom ball", [](Ground &g) { g.scene.setMeshVisibility(g.ball, visible("-primary")); });
    compare("ball without shadow", [](Ground &g) { g.scene.setMeshVisibility(g.ball, visible("-shadow")); });
    compare("ground invisible to diffuse rays", [](Ground &g) {
        g.scene.setMeshVisibility(g.ground, visible("-diffuse"));
    });
    compare("dome, ball without shadow", [](Ground &g) {
        domeOnly(g);
        g.scene.setMeshVisibility(g.ball, visible("-shadow"));
    });
    compare("dome, ground for shadow rays only", [](Ground &g) {
        domeOnly(g);
        g.scene.setMeshVisibility(g.ground, visible("primary|shadow"));
    });
    compare(
        "bgoccludeshadow",
        [](Ground &g) {
            g.scene.setMeshVisibility(g.ground, holdout(HoldoutMode::Background));
            const MeshId wall = addMesh(g.scene, quad({0.2f, 0.8f, 0.0f}, {1.8f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.5f}), g.grey);
            g.scene.setMeshVisibility(wall, holdout(HoldoutMode::Background));
        },
        true);

    // Pre-lit plates and illumbackground lights through a mirror.
    for (bool illum : {false, true}) {
        INFO("mirror, illumbackground " << illum);
        Mirror mc(true), mg(true);
        for (Mirror *m : {&mc, &mg}) {
            m->scene.setMeshVisibility(m->card, holdout(HoldoutMode::Background));
            LightDesc l = sun(normalize(Vec3f{0.0f, 0.6f, -0.8f}));
            l.illumBackground = illum;
            m->scene.updateLight(0, l);
        }
        const Result c = render(mc.scene, lpes, RenderDevice::Cpu, 2);
        const Result g = render(mg.scene, lpes, RenderDevice::Xpu, 2);
        REQUIRE(g.gpu);
        for (const Region region : {Region{0.0f, 1.5f, 0.3f}, Region{0.0f, 0.0f, 0.15f}}) {
            CHECK_THAT(color(g, region), WithinAbs(color(c, region), 0.01 * color(c, region) + 1e-5));
            CHECK_THAT(lpeValue(g, 4, region), WithinAbs(lpeValue(c, 4, region), 0.01 * lpeValue(c, 4, region) + 1e-5));
        }
    }

    // Background points' shadow; outputs over their whole light, and
    // illumbackground lights in unoccluded; outputs (D14).
    const std::vector<std::string> coatLpes = {"holdouts;shadow;C<RD>L", "holdouts;shadow;C<RG>L",
                                               "holdouts;unoccluded;C.L"};
    for (bool illum : {false, true}) {
        INFO("coated background ground, illumbackground " << illum);
        Ground gc, gg;
        for (Ground *g : {&gc, &gg}) {
            g->scene.setMeshMaterials(g->ground, {coatedGrey(g->scene)});
            g->scene.setMeshVisibility(g->ground, holdout(HoldoutMode::Background));
            domeOnly(*g);
            LightDesc l = sun(normalize(Vec3f{0.0f, 1.0f, -1.0f}));
            l.illumBackground = illum;
            g->scene.updateLight(g->scene.createLight(), l);
        }
        const Result c = render(gc.scene, coatLpes, RenderDevice::Cpu, 2);
        const Result g = render(gg.scene, coatLpes, RenderDevice::Xpu, 2);
        REQUIRE(g.gpu);
        for (const Region region : {kShadow, kOpen}) {
            for (uint32_t o : {0u, 1u})
                CHECK_THAT(shadowValue(g, o, region), WithinAbs(shadowValue(c, o, region), 2e-3));
            CHECK_THAT(lpeValue(g, 2, region), WithinAbs(lpeValue(c, 2, region), 0.01 * lpeValue(c, 2, region) + 1e-5));
        }
    }

    // Volumes: ray visibility and holdouts (D15). Paths through media part
    // ways at floating-point differences: region means within the noise.
    const std::vector<std::string> fogLpes = {"holdout;C.*[LO]", "holdouts;shadow;C.L"};
    const auto compareFog = [&](const char *what, auto &&build) {
        INFO(what);
        Ground cpuScene, gpuScene;
        build(cpuScene);
        build(gpuScene);
        const Result c = render(cpuScene.scene, fogLpes, RenderDevice::Cpu, 2);
        const Result g = render(gpuScene.scene, fogLpes, RenderDevice::Xpu, 2);
        REQUIRE(g.gpu);
        for (const Region region : {kUnder, kBoxShadow, kOpen}) {
            CHECK_THAT(color(g, region), WithinAbs(color(c, region), 0.04 * color(c, region) + 1e-3));
            CHECK_THAT(alpha(g, region), WithinAbs(alpha(c, region), 0.03));
            CHECK_THAT(lpeValue(g, 0, region), WithinAbs(lpeValue(c, 0, region), 0.04 * lpeValue(c, 0, region) + 1e-3));
            CHECK_THAT(shadowValue(g, 1, region), WithinAbs(shadowValue(c, 1, region), 0.03));
        }
    };
    for (bool white : {false, true}) {
        compareFog(white ? "white matte fog" : "black matte fog", [white](Ground &g) {
            g.scene.setVolumeVisibility(addFog(g.scene, white), holdout(HoldoutMode::Matte));
        });
        compareFog(white ? "white background fog over a background ground" : "black background fog, background ground",
                   [white](Ground &g) {
                       g.scene.setMeshVisibility(g.ground, holdout(HoldoutMode::Background));
                       g.scene.setVolumeVisibility(addFog(g.scene, white), holdout(HoldoutMode::Background));
                   });
    }
    compareFog("fog without shadows", [](Ground &g) { g.scene.setVolumeVisibility(addFog(g.scene, true), visible("-shadow")); });
    compareFog("fog hidden from the camera",
               [](Ground &g) { g.scene.setVolumeVisibility(addFog(g.scene, true), visible("-primary")); });
}
