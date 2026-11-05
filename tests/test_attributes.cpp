// Shading attributes (docs/design/M3.md K12, M3.5): interpolation per rate
// and width, width conversion, the lookup order mesh -> instance -> program
// fallback, and a program reading st through the renderer (a checker that
// must match its analytic value at every pixel center).

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
namespace sh = nova::shading;

TEST_CASE("Attributes interpolate per rate and width", "[attributes]") {
    // Two triangles over 4 points; triangle 1 = (0, 2, 3).
    const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    const float b1 = 0.25f, b2 = 0.5f, b0 = 1.0f - b1 - b2;
    for (uint8_t w = 1; w <= 4; ++w) {
        INFO("width " << int(w));
        std::vector<float> perPoint, perCorner, perTri;
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < w; ++k)
                perPoint.push_back(float(10 * i + k));
        for (int c = 0; c < 6; ++c)
            for (int k = 0; k < w; ++k)
                perCorner.push_back(float(100 + 10 * c + k));
        for (int t = 0; t < 2; ++t)
            for (int k = 0; k < w; ++k)
                perTri.push_back(float(1000 * (t + 1) + k));
        float v[4];
        AttributeRef vertex{perPoint.data(), AttributeSource::Vertex, w};
        REQUIRE(fetchAttribute(vertex, &indices[3], 1, 0, b1, b2, w, v));
        for (int k = 0; k < w; ++k)
            CHECK_THAT(v[k], WithinAbs(b0 * float(k) + b1 * float(20 + k) + b2 * float(30 + k), 1e-4));
        AttributeRef corner{perCorner.data(), AttributeSource::Corner, w};
        REQUIRE(fetchAttribute(corner, &indices[3], 1, 0, b1, b2, w, v));
        for (int k = 0; k < w; ++k)
            CHECK_THAT(v[k], WithinAbs(b0 * float(130 + k) + b1 * float(140 + k) + b2 * float(150 + k), 1e-3));
        AttributeRef uniform{perTri.data(), AttributeSource::Uniform, w};
        REQUIRE(fetchAttribute(uniform, &indices[3], 1, 0, b1, b2, w, v));
        for (int k = 0; k < w; ++k)
            CHECK(v[k] == float(2000 + k));
        AttributeRef instance{perTri.data(), AttributeSource::Instance, w};
        REQUIRE(fetchAttribute(instance, &indices[0], 0, 1, b1, b2, w, v));
        for (int k = 0; k < w; ++k)
            CHECK(v[k] == float(2000 + k));
        AttributeRef constant{perTri.data(), AttributeSource::Constant, w};
        REQUIRE(fetchAttribute(constant, &indices[0], 1, 1, b1, b2, w, v));
        for (int k = 0; k < w; ++k)
            CHECK(v[k] == float(1000 + k));
    }
    // Width conversion: splat, pad with zeros, truncate; None reports false.
    const float one[1] = {0.5f}, two[2] = {1.0f, 2.0f}, four[4] = {1, 2, 3, 4};
    float v[4] = {-1, -1, -1, -1};
    REQUIRE(fetchAttribute({one, AttributeSource::Constant, 1}, nullptr, 0, 0, 0, 0, 3, v));
    CHECK((v[0] == 0.5f && v[1] == 0.5f && v[2] == 0.5f));
    REQUIRE(fetchAttribute({two, AttributeSource::Constant, 2}, nullptr, 0, 0, 0, 0, 3, v));
    CHECK((v[0] == 1.0f && v[1] == 2.0f && v[2] == 0.0f));
    REQUIRE(fetchAttribute({four, AttributeSource::Constant, 4}, nullptr, 0, 0, 0, 0, 3, v));
    CHECK((v[0] == 1.0f && v[1] == 2.0f && v[2] == 3.0f));
    CHECK_FALSE(fetchAttribute({}, nullptr, 0, 0, 0, 0, 3, v));
}

TEST_CASE("Primvar lookup: mesh, then instance, then the fallback", "[attributes][instancing]") {
    // Three instances of a quad, side by side, seen from above with the
    // default material (base color = displayColor, else 0.18) under a white
    // dome: Lambert in a white environment shows the albedo.
    auto render = [](bool meshColor, bool instanceColors) {
        Scene scene;
        MeshDesc quad = makeQuad(0.45f);
        if (meshColor)
            quad.setDisplayColor(spectrum(0.25f));
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, quad);
        std::vector<Xform34> instances;
        for (int i = 0; i < 3; ++i) { // stretched to y in [-1.35, 1.35]: the filter stays on the quad
            Mat4 xf = translation({float(i) - 1.0f, 0.0f, 0.0f});
            xf.m[1][1] = 3.0;
            instances.push_back(toXform34(xf));
        }
        scene.setMeshInstances(id, instances);
        if (instanceColors)
            scene.setMeshInstanceAttributes(id, {{"displayColor", 3, {0.1f, 0.1f, 0.1f, 0.5f, 0.5f, 0.5f, 0.9f, 0.9f, 0.9f}}});
        LightDesc dome;
        dome.type = LightType::Dome;
        scene.updateLight(scene.createLight(), dome);
        scene.commit();
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 64;
        settings.maxDepth = 1;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.5f, 30, 10));
        renderer.resize(30, 10);
        renderToConvergence(renderer);
        std::vector<float> means(3, 0.0f); // the inner pixels of each quad
        for (int q = 0; q < 3; ++q) {
            int n = 0;
            for (int y = 4; y < 6; ++y) // the image spans y in [-1.5, 1.5]: rows 4 and 5 lie on the quads
                for (int x = 10 * q + 2; x < 10 * q + 8; ++x, ++n)
                    means[q] += pixelMean(renderer.film(), x, y).y;
            means[q] /= float(n);
        }
        return means;
    };
    const std::vector<float> instance = render(false, true), mesh = render(true, true), none = render(false, false);
    CHECK_THAT(instance[0], WithinAbs(0.1, 0.005));
    CHECK_THAT(instance[1], WithinAbs(0.5, 0.01));
    CHECK_THAT(instance[2], WithinAbs(0.9, 0.02));
    for (const float v : mesh)
        CHECK_THAT(v, WithinAbs(0.25, 0.01));
    for (const float v : none)
        CHECK_THAT(v, WithinAbs(0.18, 0.01));
}

TEST_CASE("A checker from st matches its analytic value at every pixel", "[attributes][render]") {
    // Unlit quad over [-1, 1]^2 with st = (x + 1) / 2 per point; emission =
    // ((floor(4 s) + floor(4 t)) mod 2) seen through pixel centers.
    Scene scene;
    MeshDesc quad = makeQuad(1.0f);
    quad.setAttribute({"st", AttributeRate::Vertex, 2, {0, 0, 1, 0, 1, 1, 0, 1}});
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, quad);

    sh::IrGraph g;
    g.model = SurfaceModel::Unlit;
    sh::IrBuilder b(g);
    const sh::Ref st = b.primvar("st", 2, b.constant(0.0f));
    const sh::Ref cells = b.unary(sh::Op::Floor, b.mul(st, b.constant(4.0f)));
    const sh::Ref checker = b.binary(sh::Op::Mod, b.add(b.component(cells, 0), b.component(cells, 1)), b.constant(2.0f));
    b.output(surface_field::emissionColor, 3, checker);
    const sh::CompileResult compiled = sh::compile(g);
    REQUIRE(compiled.program);
    MaterialDesc material;
    material.inputs = unlitDefaults();
    material.program = compiled.program;
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(id, {m});
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 1;
    settings.pixelFilterScale = 0.0f; // point sampling: pixel centers
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    const int size = 40;
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size));
    renderer.resize(size, size);
    renderToConvergence(renderer);
    int wrong = 0;
    double mean = 0.0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float s = (float(x) + 0.5f) / float(size), t = (float(y) + 0.5f) / float(size);
            const float expected = std::fmod(std::floor(4.0f * s) + std::floor(4.0f * t), 2.0f);
            const float v = pixelMean(renderer.film(), x, y).y;
            wrong += std::fabs(v - expected) > 1e-5f;
            mean += v / double(size * size);
        }
    CHECK(wrong == 0);
    CHECK_THAT(mean, WithinAbs(0.5, 1e-9));
}
