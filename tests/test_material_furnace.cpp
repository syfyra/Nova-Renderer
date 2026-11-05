// M3.4 materials through the full renderer (docs/design/M3.md section 7):
// white surface configurations in a uniform environment render as 1 with
// unlimited path length (glass, mirrors and coats included: delta events,
// refraction, medium tracking and Russian roulette with the index scale);
// exact emission; light reached through delta events; the light specular
// multiplier; render regions with several materials match the full frame.
// M3.12: per-lobe albedo probes against an independent oracle.

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
namespace sf = nova::surface_field;

namespace {

LightDesc whiteDome(float radiance = 1.0f) {
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.intensity = radiance;
    dome.cameraVisible = true;
    return dome;
}

struct Stats {
    double mean = 0.0;
    double error = 0.0; // standard error of the mean over the pixels
    int pixels = 0;
};

// Mean green value of the pixels within `radius` (fraction of the half width)
// of the image center: an orthographic view of a unit sphere at the origin.
// Against a black background the radius keeps the pixel filter off the rim.
Stats renderSphere(const MaterialDesc &material, uint32_t spp, float radius = 0.97f, const LightDesc &light = whiteDome(),
                   uint32_t maxDepth = 64, LightStrategy strategy = LightStrategy::MIS) {
    Scene scene;
    const MeshId sphere = scene.createMesh();
    scene.updateMesh(sphere, makeSphere(1.0f, 96, 48));
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(sphere, {m});
    scene.updateLight(scene.createLight(), light);
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.clampIndirect = 0.0f;
    settings.lightStrategy = strategy;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    const int size = 32;
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size));
    renderer.resize(size, size);
    renderToConvergence(renderer);
    Stats s;
    double sq = 0.0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float px = (x + 0.5f) / size * 2.0f - 1.0f, py = (y + 0.5f) / size * 2.0f - 1.0f;
            if (px * px + py * py > radius * radius)
                continue;
            const double v = pixelMean(renderer.film(), x, y).y;
            s.mean += v;
            sq += v * v;
            ++s.pixels;
        }
    s.mean /= s.pixels;
    s.error = std::sqrt(std::fmax(sq / s.pixels - s.mean * s.mean, 0.0) / s.pixels);
    return s;
}

MaterialDesc material(SurfaceInputs in) {
    MaterialDesc m;
    m.inputs = in;
    return m;
}

} // namespace

TEST_CASE("White materials pass the furnace through the renderer", "[render][furnace][material]") {
    struct Case {
        std::string name;
        SurfaceInputs in;
        double expected = 1.0;
        double tolerance = 3e-3;
    };
    std::vector<Case> cases;
    // The default material: Lambert with the display color.
    cases.push_back({"Lambert 0.5", lambertDefaults(spectrum(0.5f)), 0.5, 2e-3});
    for (const bool pbr : {false, true}) {
        const std::string m = pbr ? "OpenPBR " : "Standard Surface ";
        auto white = [&] {
            SurfaceInputs in = pbr ? openPbrDefaults() : standardSurfaceDefaults();
            in.set(sf::baseColor, spectrum(1.0f));
            in.set(sf::subsurfaceColor, spectrum(1.0f));
            return in;
        };
        cases.push_back({m + "plastic", white()});
        SurfaceInputs glass = white();
        glass.set(sf::transmissionWeight, 1.0f);
        glass.set(sf::specularRoughness, 0.0f);
        cases.push_back({m + "smooth glass", glass});
        glass.set(sf::specularRoughness, 0.3f);
        cases.push_back({m + "rough glass", glass});
        SurfaceInputs matched = glass;
        matched.set(sf::specularIor, 1.0f);
        cases.push_back({m + "IOR 1 glass", matched});
        SurfaceInputs thin = white();
        thin.set(sf::transmissionWeight, 1.0f);
        thin.set(sf::thinWalled, 1.0f);
        cases.push_back({m + "thin glass", thin});
        SurfaceInputs translucent = white();
        translucent.set(sf::subsurfaceWeight, 1.0f);
        translucent.set(sf::thinWalled, 1.0f);
        cases.push_back({m + "translucent", translucent});
        SurfaceInputs metal = white();
        metal.set(sf::metalness, 1.0f);
        metal.set(sf::specularRoughness, 0.0f);
        if (pbr)
            cases.push_back({m + "mirror", metal}); // Standard Surface metal clamps reflectivity at 0.99
        SurfaceInputs coat = white();
        coat.set(sf::coatWeight, 1.0f);
        if (pbr)
            coat.set(sf::coatDarkening, 0.0f); // darkening removes energy by design
        cases.push_back({m + "coated", coat});
        SurfaceInputs sheen = white();
        sheen.set(sf::sheenWeight, 1.0f);
        cases.push_back({m + (pbr ? "fuzz" : "sheen"), sheen});
    }
    for (const Case &c : cases) {
        MaterialDesc desc;
        desc.inputs = c.in;
        const Stats s = renderSphere(desc, 64, 0.85f); // the filter stays off the (white) background
        INFO(c.name << ": " << s.mean << " +- " << s.error);
        CHECK_THAT(s.mean, WithinAbs(c.expected, c.tolerance));
    }
}

TEST_CASE("Emission renders exactly", "[render][material]") {
    SurfaceInputs unlit = unlitDefaults();
    unlit.set(sf::emissionColor, {0.5f, 0.25f, 0.125f});
    LightDesc dark = whiteDome(0.0f);
    dark.cameraVisible = false;
    const Stats s = renderSphere(material(unlit), 4, 0.8f, dark);
    CHECK_THAT(s.mean, WithinAbs(0.25, 1e-5)); // green channel
}

TEST_CASE("Index-matched glass casts no shadow", "[render][material]") {
    // A sphere of index 1 over a Lambert plane in a uniform dome: the plane
    // shows rho everywhere (the sphere is a delta pass-through).
    Scene scene;
    MeshDesc plane = makeQuad(20.0f);
    plane.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), plane);
    const MeshId sphere = scene.createMesh();
    MeshDesc ball = makeSphere(1.0f, 64, 32);
    ball.transform = translation({0.0f, 0.0f, 1.2f});
    scene.updateMesh(sphere, ball);
    SurfaceInputs glass = standardSurfaceDefaults();
    glass.set(sf::transmissionWeight, 1.0f);
    glass.set(sf::specularIor, 1.0f);
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material(glass));
    scene.setMeshMaterials(sphere, {m});
    scene.updateLight(scene.createLight(), whiteDome());
    scene.commit();
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 256;
    settings.maxDepth = 16;
    settings.clampIndirect = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 10.0f}, 0.5f, 8, 8)); // looks through the sphere at the plane
    renderer.resize(8, 8);
    renderToConvergence(renderer);
    double mean = 0.0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            mean += pixelMean(renderer.film(), x, y).y / 64.0;
    CHECK_THAT(mean, WithinAbs(0.5, 0.01));
    CHECK(renderer.film().alphaSum[0] / renderer.film().samples == 1.0f); // transmission keeps alpha
}

TEST_CASE("Mirrors reflect lights at full weight", "[render][material][mis]") {
    // A smooth white metal (F = 1, OpenPBR) sphere reflecting a rect light of
    // radiance 3 behind the camera: rays after a delta bounce weigh the area
    // light they cross by 1, whatever the strategy.
    SurfaceInputs mirror = openPbrDefaults();
    mirror.set(sf::baseColor, spectrum(1.0f));
    mirror.set(sf::metalness, 1.0f);
    mirror.set(sf::specularRoughness, 0.0f);
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.intensity = 3.0f;
    rect.width = rect.height = 20.0f;
    rect.transform = translation({0.0f, 0.0f, 3.0f}); // emits down -Z, onto the sphere
    for (const LightStrategy strategy : {LightStrategy::MIS, LightStrategy::BsdfOnly, LightStrategy::LightOnly}) {
        INFO("strategy " << int(strategy));
        const Stats s = renderSphere(material(mirror), 8, 0.3f, rect, 4, strategy);
        CHECK_THAT(s.mean, WithinAbs(3.0, 0.015));
    }
}

TEST_CASE("The light specular multiplier only scales non-diffuse reflection", "[render][material][lights]") {
    // Direct light only. Every sample splits into a diffuse and a specular
    // part, so with the same sample sequence the two halves add up to the
    // full image up to rounding.
    SurfaceInputs plastic = standardSurfaceDefaults();
    plastic.set(sf::specularRoughness, 0.3f);
    LightDesc dome = whiteDome();
    dome.cameraVisible = false;
    const double full = renderSphere(material(plastic), 64, 0.8f, dome, 1).mean;
    dome.specular = 0.0f;
    const double diffuseOnly = renderSphere(material(plastic), 64, 0.8f, dome, 1).mean;
    dome.specular = 1.0f;
    dome.diffuse = 0.0f;
    const double specularOnly = renderSphere(material(plastic), 64, 0.8f, dome, 1).mean;
    CHECK(diffuseOnly < 0.95 * full);
    CHECK(specularOnly > 0.02 * full);
    CHECK_THAT(diffuseOnly + specularOnly, WithinAbs(full, 1e-4 * full));
}

TEST_CASE("Regions with several materials render like the full frame", "[render][material]") {
    // Material lookups per mesh, refraction state and delta events are per
    // path: a pixel renders the same whichever region or tile it is in.
    Scene scene;
    for (int i = 0; i < 4; ++i) {
        MeshDesc ball = makeSphere(0.45f, 32, 16);
        ball.transform = translation({float(i % 2) - 0.5f, float(i / 2) - 0.5f, 0.0f});
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, ball);
        SurfaceInputs in = i % 2 ? openPbrDefaults() : standardSurfaceDefaults();
        in.set(sf::metalness, i == 1 ? 1.0f : 0.0f);
        in.set(sf::transmissionWeight, i == 2 ? 1.0f : 0.0f);
        in.set(sf::coatWeight, i == 3 ? 1.0f : 0.0f);
        const MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, material(in));
        scene.setMeshMaterials(id, {m});
    }
    scene.updateLight(scene.createLight(), whiteDome());
    scene.commit();
    const int size = 40;
    auto renderRegion = [&](TileRect region) {
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 4;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size));
        renderer.resize(size, size);
        renderer.setRegion(region);
        renderToConvergence(renderer);
        return renderer.film().colorSum;
    };
    const std::vector<Spectrum> full = renderRegion({0, 0, size, size});
    const TileRect part{7, 3, 29, 33};
    const std::vector<Spectrum> region = renderRegion(part);
    REQUIRE(full.size() == region.size());
    int differ = 0;
    for (int y = part.y0; y < part.y1; ++y)
        for (int x = part.x0; x < part.x1; ++x) {
            const size_t i = size_t(y) * size + size_t(x);
            differ += full[i].x != region[i].x || full[i].y != region[i].y || full[i].z != region[i].z;
        }
    CHECK(differ == 0);
}

// ---------------------------------------------------------------------------
// Albedo probes (M3.12; docs/research/m3/test-strategy.md section 7.1): the
// directional albedo of lobe configurations, rendered on an analytic unit
// sphere seen orthographically in a white environment of radiance 1, direct
// light only (a convex surface sees nothing else), at point-sampled pixel
// centers, so a pixel at distance d from the center sees mu = sqrt(1 - d^2).
// The reference is tests/data/albedo_ref.json, written by the independent
// numpy oracle tests/reference/tools/gen_albedo_ref.py: E(mu) on a grid in
// u = (2 / pi) asin(sqrt(mu)). The radial profile is compared in ten mu bins,
// per channel, at 0.3 % absolute.
// ---------------------------------------------------------------------------

namespace {

constexpr int kAlbedoGrid = 80; // the oracle's u nodes j / 80

std::string readText(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The first `count` numbers of the JSON array after the key `key` (searched
// from `from`); fewer if the text ends.
std::vector<double> jsonNumbers(const std::string &json, const std::string &key, size_t from, size_t count) {
    std::vector<double> out;
    size_t p = json.find("\"" + key + "\"", from);
    if (p == std::string::npos || (p = json.find('[', p)) == std::string::npos)
        return out;
    const char *c = json.c_str() + p;
    while (out.size() < count && *c) {
        if ((*c >= '0' && *c <= '9') || *c == '-' || *c == '.') {
            char *end = nullptr;
            out.push_back(std::strtod(c, &end));
            c = end;
        } else {
            ++c;
        }
    }
    return out;
}

struct AlbedoProbe {
    const char *name;
    SurfaceInputs in;
};

std::vector<AlbedoProbe> albedoProbes() {
    const Spectrum copper{0.95f, 0.64f, 0.54f}, warm{0.8f, 0.5f, 0.2f}, white = spectrum(1.0f);
    auto ssDiffuse = [](Spectrum color, float roughness) {
        SurfaceInputs in = standardSurfaceDefaults();
        in.set(sf::baseColor, color);
        in.set(sf::specularWeight, 0.0f);
        in.set(sf::diffuseRoughness, roughness);
        return in;
    };
    auto pbrDiffuse = [](Spectrum color, float roughness) {
        SurfaceInputs in = openPbrDefaults();
        in.set(sf::baseColor, color);
        in.set(sf::specularWeight, 0.0f);
        in.set(sf::diffuseRoughness, roughness);
        return in;
    };
    auto metal = [](bool pbr, Spectrum color, Spectrum edge, float roughness) {
        SurfaceInputs in = pbr ? openPbrDefaults() : standardSurfaceDefaults();
        in.set(sf::metalness, 1.0f);
        in.set(sf::baseColor, color);
        in.set(sf::specularColor, edge);
        in.set(sf::specularRoughness, roughness);
        return in;
    };
    auto dielectric = [](float roughness) {
        SurfaceInputs in = standardSurfaceDefaults();
        in.set(sf::baseColor, spectrum(0.0f));
        in.set(sf::specularIor, 1.5f);
        in.set(sf::specularRoughness, roughness);
        return in;
    };
    const Spectrum tint{1.0f, 0.9f, 0.75f};
    return {
        {"ss_lambert", ssDiffuse(warm, 0.0f)},
        {"ss_qon_r05", ssDiffuse(white, 0.5f)},
        {"ss_qon_r10", ssDiffuse(white, 1.0f)},
        {"pbr_eon_r05", pbrDiffuse(warm, 0.5f)},
        {"pbr_eon_r10", pbrDiffuse(warm, 1.0f)},
        {"ss_metal_r02", metal(false, copper, white, 0.2f)},
        {"ss_metal_r05", metal(false, copper, white, 0.5f)},
        {"ss_metal_r08", metal(false, copper, white, 0.8f)},
        {"ss_metal_edge_r05", metal(false, spectrum(0.9f), {1.0f, 0.6f, 0.3f}, 0.5f)},
        {"ss_metal_white_r10", metal(false, white, white, 1.0f)},
        {"pbr_metal_r03", metal(true, copper, tint, 0.3f)},
        {"pbr_metal_r07", metal(true, copper, tint, 0.7f)},
        {"ss_dielectric_r02", dielectric(0.2f)},
        {"ss_dielectric_r05", dielectric(0.5f)},
        {"ss_dielectric_r08", dielectric(0.8f)},
    };
}

} // namespace

TEST_CASE("Lobe albedos match the independent oracle", "[render][material][albedo]") {
    const std::string json = readText(std::string(NOVA_TEST_DIR) + "/data/albedo_ref.json");
    REQUIRE(!json.empty());
    constexpr size_t kNodes = kAlbedoGrid + 1;
    const std::vector<double> u = jsonNumbers(json, "u", 0, kNodes);
    REQUIRE(u.size() == kNodes);
    CHECK(u[kAlbedoGrid / 2] == 0.5);

    const int size = 128;
    const uint32_t spp = 256;
    constexpr int kBins = 10;
    for (const AlbedoProbe &probe : albedoProbes()) {
        const size_t at = json.find("\"" + std::string(probe.name) + "\"");
        REQUIRE(at != std::string::npos);
        const std::vector<double> E = jsonNumbers(json, "E", at, 3 * kNodes);
        REQUIRE(E.size() == 3 * kNodes);

        Scene scene;
        const MeshId sphere = scene.createMesh();
        MeshDesc mesh;
        mesh.shape = MeshShape::Sphere;
        mesh.points = {{0.0f, 0.0f, 0.0f}};
        mesh.sphereRadius = {1.0f};
        scene.updateMesh(sphere, mesh);
        const MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, material(probe.in));
        scene.setMeshMaterials(sphere, {m});
        scene.updateLight(scene.createLight(), whiteDome());
        scene.commit();
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = spp;
        settings.maxDepth = 1;
        settings.clampIndirect = 0.0f;
        settings.pixelFilterScale = 0.0f; // pixel centers
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size));
        renderer.resize(size, size);
        renderToConvergence(renderer);

        double diff[kBins][3] = {}, sq[kBins][3] = {};
        int count[kBins] = {};
        for (int py = 0; py < size; ++py)
            for (int px = 0; px < size; ++px) {
                const double sx = (px + 0.5) / size * 2.0 - 1.0, sy = (py + 0.5) / size * 2.0 - 1.0;
                const double d2 = sx * sx + sy * sy;
                if (d2 >= 1.0)
                    continue;
                const double mu = std::sqrt(1.0 - d2), t = std::asin(std::sqrt(mu)) * (2.0 / kPi) * kAlbedoGrid;
                const int j = std::min(int(t), kAlbedoGrid - 1), bin = std::min(int(mu * kBins), kBins - 1);
                const double f = t - j;
                const Spectrum value = pixelMean(renderer.film(), px, py);
                for (int c = 0; c < 3; ++c) {
                    const double ref = E[3 * j + c] + f * (E[3 * (j + 1) + c] - E[3 * j + c]);
                    const double dv = double(value[c]) - ref;
                    diff[bin][c] += dv;
                    sq[bin][c] += dv * dv;
                }
                ++count[bin];
            }
        for (int b = 0; b < kBins; ++b) {
            REQUIRE(count[b] > 0);
            for (int c = 0; c < 3; ++c) {
                const double mean = diff[b][c] / count[b];
                const double error = std::sqrt(std::fmax(sq[b][c] / count[b] - mean * mean, 0.0) / count[b]);
                INFO(probe.name << ", mu in [" << b * 0.1 << ", " << (b + 1) * 0.1 << "), channel " << c
                                << ": Nova - oracle " << mean << " +- " << error << " (" << count[b] << " pixels)");
                CHECK(std::fabs(mean) <= 3e-3);
            }
        }
    }
}
