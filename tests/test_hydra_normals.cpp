// Normals, bump and tangents (docs/design/M3.md M3.10), through the
// translator and the renderer: Karma's normal map (Gram-Schmidt frame from
// st, the Ng guard), its world-space bump (heighttonormal), the tangent
// frame's orientation with swapped st, lit normal maps against the analytic
// irradiance, and energy with a random normal map in a white furnace.

#include "test_helpers.h"

#include "hydra/material/translator.h"

#include "core/shading/compiler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;

namespace {

HdMaterialNode2 mxNode(char const *id) {
    HdMaterialNode2 n;
    n.nodeTypeId = TfToken(id);
    return n;
}

void connect(HdMaterialNode2 &n, char const *input, char const *upstream, char const *output = "out") {
    n.inputConnections[TfToken(input)] = {{SdfPath(upstream), TfToken(output)}};
}

struct Network {
    HdNovaNetworkSource src;
    explicit Network(HdMaterialNode2 const &surface) {
        src.context = TfToken("mtlx");
        src.net.nodes[SdfPath("surface")] = surface;
        src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("surface"), TfToken()};
    }
    Network &add(char const *name, HdMaterialNode2 const &n) {
        src.net.nodes[SdfPath(name)] = n;
        return *this;
    }
    nova::MaterialDesc material() const {
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
        REQUIRE(tr.ok);
        for (std::string const &w : tr.warnings)
            FAIL_CHECK(w);
        nova::MaterialDesc d;
        d.inputs = tr.defaults;
        d.program = nova::shading::compile(tr.graph).program;
        REQUIRE(d.program);
        d.params = tr.params;
        return d;
    }
};

// A quad over [-1, 1]^2 facing +Z with st = ((x + 1) / 2, (y + 1) / 2), or
// swapped (st.x along y), rendered orthographically at pixel centers.
std::vector<nova::Spectrum> renderQuad(nova::MaterialDesc const &material, bool swapSt = false, int size = 8,
                                       nova::LightDesc const *light = nullptr, uint32_t spp = 1,
                                       nova::Mat4 const *objectToWorld = nullptr) {
    nova::Scene scene;
    nova::MeshDesc quad = nova::test::makeQuad(1.0f);
    std::vector<float> st = {0, 0, 1, 0, 1, 1, 0, 1};
    if (swapSt)
        st = {0, 0, 0, 1, 1, 1, 1, 0};
    quad.setAttribute({"st", nova::AttributeRate::Vertex, 2, st});
    const nova::MeshId id = scene.createMesh();
    scene.updateMesh(id, quad);
    if (objectToWorld)
        scene.updateMeshTransform(id, *objectToWorld);
    const nova::MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(id, {m});
    if (light)
        scene.updateLight(scene.createLight(), *light);
    scene.commit();
    nova::Renderer renderer;
    nova::RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = 1;
    settings.pixelFilterScale = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(nova::test::makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size));
    renderer.resize(size, size);
    nova::test::renderToConvergence(renderer);
    std::vector<nova::Spectrum> out;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            out.push_back(nova::test::pixelMean(renderer.film(), x, y));
    return out;
}

// An unlit surface showing a vector3 node's output as its emission.
Network showVector(HdMaterialNode2 const &n) {
    HdMaterialNode2 surface = mxNode("ND_surface_unlit");
    connect(surface, "emission_color", "show");
    HdMaterialNode2 convert = mxNode("ND_convert_vector3_color3");
    connect(convert, "in", "probe");
    Network net(surface);
    net.add("show", convert).add("probe", n);
    return net;
}

HdMaterialNode2 normalMap(GfVec3f in) {
    HdMaterialNode2 n = mxNode("ND_normalmap_float");
    n.parameters[TfToken("in")] = VtValue(in);
    return n;
}

void checkAll(std::vector<nova::Spectrum> const &px, nova::Vec3f expect, double tol) {
    int wrong = 0;
    for (nova::Spectrum const &p : px)
        wrong += std::fabs(p.x - expect.x) > tol || std::fabs(p.y - expect.y) > tol || std::fabs(p.z - expect.z) > tol;
    INFO("first pixel " << px[0].x << " " << px[0].y << " " << px[0].z << ", expected " << expect.x << " " << expect.y
                        << " " << expect.z);
    CHECK(wrong == 0);
}

} // namespace

TEST_CASE("Normal maps follow Karma", "[hydra][normals]") {
    // Flat: the shading normal.
    checkAll(renderQuad(showVector(normalMap(GfVec3f(0.5f, 0.5f, 1.0f))).material()), {0, 0, 1}, 1e-6);
    // Tilted by a towards +u: T = +X with st, +Y with swapped st (B follows).
    const float a = 0.5f;
    const GfVec3f tilt(0.5f + 0.5f * std::sin(a), 0.5f, 0.5f + 0.5f * std::cos(a));
    checkAll(renderQuad(showVector(normalMap(tilt)).material()), {std::sin(a), 0.0f, std::cos(a)}, 1e-5);
    checkAll(renderQuad(showVector(normalMap(tilt)).material(), true), {0.0f, std::sin(a), std::cos(a)}, 1e-5);
    const GfVec3f tiltV(0.5f, 0.5f + 0.5f * std::sin(a), 0.5f + 0.5f * std::cos(a));
    checkAll(renderQuad(showVector(normalMap(tiltV)).material()), {0.0f, std::sin(a), std::cos(a)}, 1e-5);
    checkAll(renderQuad(showVector(normalMap(tiltV)).material(), true), {std::sin(a), 0.0f, std::cos(a)}, 1e-5);
    // Below the surface: pulled back to just above it (Karma's 1.001 guard).
    const float r = std::sqrt(0.5f);
    checkAll(renderQuad(showVector(normalMap(GfVec3f(1.0f, 0.5f, 0.0f))).material()), {r, 0.0f, -r + r * 1.001f}, 1e-5);
    // The tangent node: Karma's T.
    HdMaterialNode2 tangent = mxNode("ND_tangent_vector3");
    tangent.parameters[TfToken("space")] = VtValue(std::string("world"));
    checkAll(renderQuad(showVector(tangent).material()), {1, 0, 0}, 1e-6);
    checkAll(renderQuad(showVector(tangent).material(), true), {0, 1, 0}, 1e-6);
}

TEST_CASE("Object-to-world normals keep their side on mirrored objects", "[hydra][normals]") {
    // The inverse transpose of a mirror in x leaves (0, 0.6, 0.8) as it is;
    // the bare cofactor (det -1) would negate it, unlike the hit's normal.
    HdMaterialNode2 n = mxNode("ND_transformnormal_vector3");
    n.parameters[TfToken("in")] = VtValue(GfVec3f(0.0f, 0.6f, 0.8f));
    n.parameters[TfToken("fromspace")] = VtValue(std::string("object"));
    n.parameters[TfToken("tospace")] = VtValue(std::string("world"));
    const nova::MaterialDesc material = showVector(n).material();
    checkAll(renderQuad(material), {0.0f, 0.6f, 0.8f}, 1e-5);
    nova::Mat4 mirror = nova::Mat4::identity();
    mirror.m[0][0] = -1.0;
    checkAll(renderQuad(material, false, 8, nullptr, 1, &mirror), {0.0f, 0.6f, 0.8f}, 1e-5);
}

TEST_CASE("A linear height bumps to the analytic tilt", "[hydra][normals]") {
    // h = 0.6 u = 0.3 (x + 1): Karma's world-space bump moves P by Ng h, so
    // the surface rises with slope 0.3 along x; normal = norm(-0.3, 0, 1).
    HdMaterialNode2 st = mxNode("ND_texcoord_vector2");
    HdMaterialNode2 u = mxNode("ND_extract_vector2");
    connect(u, "in", "st");
    HdMaterialNode2 h = mxNode("ND_multiply_float");
    connect(h, "in1", "u");
    h.parameters[TfToken("in2")] = VtValue(0.6f);
    HdMaterialNode2 bump = mxNode("ND_bump_vector3");
    connect(bump, "height", "h");
    Network net = showVector(bump);
    net.add("st", st).add("u", u).add("h", h);
    const float k = 0.3f, len = std::sqrt(1.0f + k * k);
    checkAll(renderQuad(net.material(), false, 8), {-k / len, 0.0f, 1.0f / len}, 2e-5);
    // The same with swapped st: the slope runs along y.
    checkAll(renderQuad(net.material(), true, 8), {0.0f, -k / len, 1.0f / len}, 2e-5);
}

TEST_CASE("A tilted normal map gives the analytic irradiance", "[hydra][normals]") {
    // Lambert (Standard Surface without specular) under a distant light
    // from +Z: the radiance scales with cos of the normal map's tilt.
    auto lit = [](float tilt) {
        HdMaterialNode2 surface = mxNode("ND_standard_surface_surfaceshader");
        surface.parameters[TfToken("specular")] = VtValue(0.0f);
        surface.parameters[TfToken("base_color")] = VtValue(GfVec3f(0.8f, 0.8f, 0.8f));
        connect(surface, "normal", "nmap");
        Network net(surface);
        net.add("nmap", normalMap(GfVec3f(0.5f + 0.5f * std::sin(tilt), 0.5f, 0.5f + 0.5f * std::cos(tilt))));
        nova::LightDesc sun;
        sun.type = nova::LightType::Distant;
        sun.angleDeg = 0.0f;
        sun.intensity = 2.0f;
        double sum = 0.0;
        for (nova::Spectrum const &p : renderQuad(net.material(), false, 8, &sun, 4))
            sum += p.y;
        return sum / 64.0;
    };
    const double flat = lit(0.0f);
    CHECK(flat > 0.1);
    for (float t : {0.4f, 0.9f})
        CHECK_THAT(lit(t) / flat, WithinAbs(std::cos(t), 1e-4));
}

TEST_CASE("A random normal map keeps the white furnace at most 1", "[hydra][normals]") {
    // White Lambert sphere, normals from 3D noise, in a uniform white dome
    // with unlimited bounces.
    HdMaterialNode2 surface = mxNode("ND_standard_surface_surfaceshader");
    surface.parameters[TfToken("specular")] = VtValue(0.0f);
    surface.parameters[TfToken("base_color")] = VtValue(GfVec3f(1.0f, 1.0f, 1.0f));
    connect(surface, "normal", "nmap");
    HdMaterialNode2 noise = mxNode("ND_noise3d_vector3");
    noise.parameters[TfToken("amplitude")] = VtValue(GfVec3f(0.45f, 0.45f, 0.25f));
    noise.parameters[TfToken("pivot")] = VtValue(0.5f);
    HdMaterialNode2 pos = mxNode("ND_position_vector3");
    HdMaterialNode2 scale = mxNode("ND_multiply_vector3FA");
    connect(scale, "in1", "pos");
    scale.parameters[TfToken("in2")] = VtValue(6.0f);
    connect(noise, "position", "scaled");
    HdMaterialNode2 lift = mxNode("ND_add_vector3");
    connect(lift, "in1", "noise");
    lift.parameters[TfToken("in2")] = VtValue(GfVec3f(0.0f, 0.0f, 0.25f)); // z in [0.5, 1]
    HdMaterialNode2 nmap = mxNode("ND_normalmap_float");
    connect(nmap, "in", "lifted");
    Network net(surface);
    net.add("nmap", nmap).add("lifted", lift).add("noise", noise).add("scaled", scale).add("pos", pos);

    nova::Scene scene;
    const nova::MeshId sphere = scene.createMesh();
    scene.updateMesh(sphere, nova::test::makeSphere(1.0f, 96, 48));
    const nova::MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, net.material());
    scene.setMeshMaterials(sphere, {m});
    nova::LightDesc dome;
    dome.type = nova::LightType::Dome;
    dome.cameraVisible = true;
    scene.updateLight(scene.createLight(), dome);
    scene.commit();
    nova::Renderer renderer;
    nova::RenderSettings settings;
    settings.maxSamples = 256;
    settings.maxDepth = 64;
    settings.clampIndirect = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    const int size = 32;
    renderer.setCamera(nova::test::makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size));
    renderer.resize(size, size);
    nova::test::renderToConvergence(renderer);
    double sum = 0.0, sq = 0.0;
    int n = 0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float px = (x + 0.5f) / size * 2.0f - 1.0f, py = (y + 0.5f) / size * 2.0f - 1.0f;
            if (px * px + py * py > 0.9f)
                continue;
            const double v = nova::test::pixelMean(renderer.film(), x, y).y;
            sum += v;
            sq += v * v;
            ++n;
        }
    const double mean = sum / n, sigma = std::sqrt(std::fmax(sq / n - mean * mean, 0.0) / n);
    INFO("furnace mean " << mean << " +- " << sigma);
    // No energy gain. Bent shading normals lose some (directions below the
    // surface are absorbed; 0.876 measured, as in Cycles' Lambert).
    CHECK(mean <= 1.0 + 3.0 * sigma);
    CHECK(mean > 0.8);
}

TEST_CASE("UsdPreviewSurface renders as Karma's graph", "[hydra][preview]") {
    // Karma CPU (docs/research/m3/hydra-materials.md 2.5): a plane under a
    // white dome of intensity 1, direct light only, seen from above.
    auto render = [](HdMaterialNode2 const &surface) {
        Network net(surface);
        nova::Scene scene;
        const nova::MeshId id = scene.createMesh();
        scene.updateMesh(id, nova::test::makeQuad(1.0f));
        const nova::MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, net.material());
        scene.setMeshMaterials(id, {m});
        nova::LightDesc dome;
        dome.type = nova::LightType::Dome;
        scene.updateLight(scene.createLight(), dome);
        scene.commit();
        nova::Renderer renderer;
        nova::RenderSettings settings;
        settings.maxSamples = 512;
        settings.maxDepth = 1;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(nova::test::makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.5f, 8, 8));
        renderer.resize(8, 8);
        nova::test::renderToConvergence(renderer);
        nova::Spectrum sum{0.0f, 0.0f, 0.0f};
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                sum += nova::test::pixelMean(renderer.film(), x, y);
        return sum * (1.0f / 64.0f);
    };
    HdMaterialNode2 preview = mxNode("UsdPreviewSurface");
    preview.parameters[TfToken("diffuseColor")] = VtValue(GfVec3f(0.5f, 0.25f, 0.125f));
    const nova::Spectrum c = render(preview);
    // c = C (1 - E) + S: the base loses E = 0.0368 (Karma 0.0376), the
    // specular reflects the dome with S = 0.0369. Karma's S is 0.0327: its
    // Schlick lobe lacks the multiple-scattering compensation MaterialX's
    // generalized_schlick_bsdf applies (and Nova follows), +0.004 absolute.
    CHECK_THAT(c.x, WithinAbs(0.5139, 0.5139 * 0.02));
    CHECK_THAT(c.y, WithinAbs(0.2733, 0.2733 * 0.02));
    CHECK_THAT(c.z, WithinAbs(0.1530, 0.1530 * 0.03));
    const nova::Spectrum d = render(mxNode("UsdPreviewSurface"));
    CHECK_THAT(d.y, WithinAbs(0.2062, 0.2062 * 0.02));
}
