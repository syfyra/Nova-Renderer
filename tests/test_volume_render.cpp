// M8.2 volumes in the renderer (docs/design/M8.md D8, D9, D12, D18): ratio
// tracking through a slab gives its exact transmittance (camera rays and
// shadow rays), emission per unit length adds up along the ray (also where
// nothing absorbs), alpha is the volumes' opacity, and renders do not depend
// on the thread count or on adaptive sampling at threshold 0.

#if defined(_MSC_VER)
#pragma warning(disable : 4702) // unreachable code in NanoVDB's grid builder templates
#endif

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/integrator/volume_media.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"

#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/GridBuilder.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace nova;
using namespace nova::test;
using namespace nova::shading;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A slab of constant value: voxels [0, 32) x [0, 32) x [0, 8), 1/32 units
// each, voxel (0,0,0) at the origin. Along z the trilinear field ramps over
// one voxel at both ends, so its integral through the slab is value * 8/32.
VolumeGridPtr slabGrid(float value) {
    nanovdb::build::Grid<float> g(0.0f, "field", nanovdb::GridClass::FogVolume);
    g.setTransform(1.0 / 32.0, nanovdb::Vec3d(0.0));
    g([&](const nanovdb::Coord &) { return value; }, nanovdb::CoordBBox(nanovdb::Coord(0), nanovdb::Coord(31, 31, 7)));
    const nanovdb::GridHandle<nanovdb::HostBuffer> h = nanovdb::createNanoGrid(g);
    return VolumeGrid::copyOf(h.data(), h.size());
}
constexpr float kSlabDepth = 8.0f / 32.0f;

// A volume shader of fields: absorption = a * density, emission = e *
// density + t * temperature (no scattering: M8.2 renders no volume events).
MaterialDesc absorbingEmitter(float a, float e, float t) {
    IrGraph g;
    IrBuilder b(g);
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    const Ref temp = b.primvar("temperature", 1, b.constant(0.0f));
    b.output(uint16_t(volume_field::kBase + volume_field::absorption), 3, b.convert(b.mul(d, b.constant(a)), 3));
    b.output(uint16_t(volume_field::kBase + volume_field::emission), 3,
             b.convert(b.add(b.mul(d, b.constant(e)), b.mul(temp, b.constant(t))), 3));
    MaterialDesc m;
    m.program = compile(g).program;
    return m;
}

struct Stats {
    Spectrum color{0.0f, 0.0f, 0.0f};
    double alpha = 0.0;
};

// The mean over every pixel of an orthographic view down -Z of the slab's
// middle (far from its sides).
Stats render(const Scene &scene, uint32_t spp, int threads = 0, Film *out = nullptr) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = 4;
    settings.clampIndirect = 0.0f;
    renderer.setThreadLimit(threads);
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.5f, 0.5f, 5.0f}, 0.2f, 16, 16));
    renderer.resize(16, 16);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    Stats s;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const size_t i = size_t(y) * 16 + size_t(x);
            s.color += pixelMean(film, x, y) * (1.0f / 256.0f);
            s.alpha += film.alphaSum[i] / film.count[i] / 256.0;
        }
    if (out)
        *out = film;
    return s;
}

VolumeId addSlab(Scene &scene, MaterialId material, float density, float temperature = 0.0f) {
    VolumeDesc desc;
    if (density > 0.0f)
        desc.fields.push_back({"density", "density", slabGrid(density), Affine3f{}});
    if (temperature > 0.0f)
        desc.fields.push_back({"temperature", "temperature", slabGrid(temperature), Affine3f{}});
    desc.material = material;
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    return id;
}

void addDome(Scene &scene, bool visible) {
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.color = spectrum(1.0f);
    dome.cameraVisible = visible;
    scene.updateLight(scene.createLight(), dome);
}

} // namespace

TEST_CASE("Volumes absorb with their exact transmittance", "[volume][render]") {
    Scene scene;
    const MaterialId mat = scene.createMaterial();
    scene.updateMaterial(mat, absorbingEmitter(1.0f, 0.0f, 0.0f));
    addSlab(scene, mat, 4.0f); // optical depth 4 * 8/32 = 1
    addDome(scene, true);
    scene.commit();
    const Stats s = render(scene, 256);
    CHECK_THAT(s.color.x, WithinRel(std::exp(-1.0), 0.01));
    CHECK_THAT(s.color.z, WithinRel(std::exp(-1.0), 0.01));
    CHECK_THAT(s.alpha, WithinAbs(1.0, 1e-6)); // the dome is visible

    SECTION("alpha is the volumes' opacity") {
        Scene dark;
        const MaterialId m = dark.createMaterial();
        dark.updateMaterial(m, absorbingEmitter(1.0f, 0.0f, 0.0f));
        addSlab(dark, m, 4.0f);
        addDome(dark, false);
        dark.commit();
        const Stats a = render(dark, 256);
        CHECK_THAT(a.alpha, WithinRel(1.0 - std::exp(-1.0), 0.01));
        CHECK(a.color.y == 0.0f);
    }
    SECTION("the default volume extinguishes by density") {
        Scene plain;
        addSlab(plain, kDefaultMaterialId, 2.0f); // optical depth 0.5
        plain.commit();
        CHECK_THAT(render(plain, 256).alpha, WithinRel(1.0 - std::exp(-0.5), 0.01));
    }
}

TEST_CASE("Volume emission adds up along the ray", "[volume][render]") {
    SECTION("an absorbing emitter: (e / a)(1 - exp(-a tau))") {
        Scene scene;
        const MaterialId mat = scene.createMaterial();
        scene.updateMaterial(mat, absorbingEmitter(1.0f, 2.0f, 0.0f));
        addSlab(scene, mat, 4.0f);
        scene.commit();
        const Stats s = render(scene, 256);
        CHECK_THAT(s.color.y, WithinRel(2.0 * (1.0 - std::exp(-1.0)), 0.01));
    }
    SECTION("emission where nothing absorbs") {
        // A temperature field only: emission 3 * 1.5 per unit length over
        // the slab's integral 8/32 (the majorant's floor collects it).
        Scene scene;
        const MaterialId mat = scene.createMaterial();
        scene.updateMaterial(mat, absorbingEmitter(1.0f, 0.0f, 3.0f));
        addSlab(scene, mat, 0.0f, 1.5f);
        scene.commit();
        REQUIRE(scene.activeVolumes().size() == 1);
        const Stats s = render(scene, 256);
        CHECK_THAT(s.color.x, WithinRel(3.0 * 1.5 * kSlabDepth, 0.01));
        CHECK(s.alpha == 0.0); // no density, no opacity
    }
}

TEST_CASE("Shadow rays take the volumes' transmittance", "[volume][render]") {
    // A white plane below the slab, lit straight from above by a distant
    // light, seen straight from above: through the slab twice.
    const auto plane = [](bool slab) {
        auto scene = std::make_unique<Scene>();
        MeshDesc quad = makeQuad(2.0f);
        quad.transform = translation({0.5f, 0.5f, -0.5f});
        quad.setDisplayColor(spectrum(1.0f));
        scene->updateMesh(scene->createMesh(), quad);
        LightDesc sun; // shines down its -Z axis
        sun.type = LightType::Distant;
        sun.color = spectrum(1.0f);
        sun.normalize = true;
        scene->updateLight(scene->createLight(), sun);
        if (slab) {
            const MaterialId mat = scene->createMaterial();
            scene->updateMaterial(mat, absorbingEmitter(1.0f, 0.0f, 0.0f));
            addSlab(*scene, mat, 2.0f); // optical depth 0.5 each way
        }
        scene->commit();
        return scene;
    };
    const Stats open = render(*plane(false), 64), shaded = render(*plane(true), 256);
    REQUIRE(open.color.y > 0.1f);
    CHECK_THAT(shaded.color.y / open.color.y, WithinRel(std::exp(-1.0), 0.015));
}

TEST_CASE("Volume renders do not depend on the thread count", "[volume][render]") {
    Scene scene;
    const MaterialId mat = scene.createMaterial();
    scene.updateMaterial(mat, absorbingEmitter(1.0f, 2.0f, 0.0f));
    addSlab(scene, mat, 4.0f);
    addDome(scene, true);
    scene.commit();
    Film one, many;
    render(scene, 16, 1, &one);
    render(scene, 16, 16, &many);
    for (size_t i = 0; i < one.colorSum.size(); ++i) {
        CHECK(one.colorSum[i].x == many.colorSum[i].x);
        CHECK(one.alphaSum[i] == many.alphaSum[i]);
    }

    SECTION("adaptive sampling with threshold 0 is the uniform render") {
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 16;
        settings.maxDepth = 4;
        settings.clampIndirect = 0.0f;
        settings.adaptive = true;
        settings.oracleMinSamples = 4;
        settings.oracleThreshold = 0.0f;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.5f, 0.5f, 5.0f}, 0.2f, 16, 16));
        renderer.resize(16, 16);
        renderToConvergence(renderer);
        const Film &a = renderer.film();
        bool same = true;
        for (size_t i = 0; i < one.colorSum.size(); ++i)
            same = same && a.count[i] == 16u && a.colorSum[i].x == one.colorSum[i].x && a.alphaSum[i] == one.alphaSum[i];
        CHECK(same);
    }
}

TEST_CASE("Ratio tracking is unbiased on a heterogeneous field", "[volume][tracking]") {
    // Smoke-like density (three soft blobs, hashed variation), the default
    // volume (extinction = density); per ray the mean of many estimates
    // against the exact transmittance, from a fine quadrature of the
    // trilinear field along the ray (research A E1).
    nanovdb::build::Grid<float> g(0.0f, "density", nanovdb::GridClass::FogVolume);
    g.setTransform(1.0 / 48.0, nanovdb::Vec3d(0.0));
    g([](const nanovdb::Coord &c) {
        const float blobs[3][4] = {{16, 20, 24, 8}, {30, 28, 20, 6}, {24, 34, 30, 9}};
        float d = 0.0f;
        for (const auto &b : blobs) {
            const float dx = float(c[0]) - b[0], dy = float(c[1]) - b[1], dz = float(c[2]) - b[2];
            d += 3.0f * std::exp(-(dx * dx + dy * dy + dz * dz) / (2.0f * b[3] * b[3]));
        }
        const uint32_t h = (uint32_t(c[0]) * 73856093u) ^ (uint32_t(c[1]) * 19349663u) ^ (uint32_t(c[2]) * 83492791u);
        const float v = d * (0.7f + 0.6f * float(h % 1000u) / 1000.0f);
        return v > 0.05f ? v : 0.0f;
    }, nanovdb::CoordBBox(nanovdb::Coord(0), nanovdb::Coord(47)));
    const nanovdb::GridHandle<nanovdb::HostBuffer> handle = nanovdb::createNanoGrid(g);
    Scene scene;
    VolumeDesc desc;
    desc.fields.push_back({"density", "density", VolumeGrid::copyOf(handle.data(), handle.size()), Affine3f{}});
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    scene.commit();
    VolumeSampler exact(*scene.volumeRecord(id).desc.fields[0].grid->grid());

    VolumeEvalCache cache;
    Rng pick = Rng::make(7, 0, 1);
    double zSum = 0.0, varSum = 0.0;
    int rays = 0;
    for (int r = 0; r < 120; ++r) {
        // Random rays through the box.
        const Vec3f o{pick.next() * 1.4f - 0.2f, pick.next() * 1.4f - 0.2f, -0.5f};
        const Vec3f target{pick.next(), pick.next(), 1.5f};
        const Vec3f d = normalize(target - o);
        const float tEnd = length(target - o);
        double tau = 0.0;
        const int steps = 20000;
        for (int i = 0; i < steps; ++i) {
            const Vec3f p = o + d * ((float(i) + 0.5f) * tEnd / float(steps));
            tau += std::max(exact(p * 48.0f), 0.0f) * (tEnd / steps);
        }
        const double T = std::exp(-tau);
        if (T < 0.01 || T > 0.99)
            continue;
        MediumRay ray{o, d, tEnd, 0.5f, 0, 0};
        double mean = 0.0, m2 = 0.0;
        const int n = 400;
        for (int s = 0; s < n; ++s) {
            Rng rng = Rng::make(uint32_t(r), uint32_t(s), 3);
            const double v = trackMedia(scene, cache, ShadeGlobals{}, ray, rng.next(), rng, nullptr).y;
            mean += v / n;
            m2 += v * v / n;
        }
        const double var = std::max(m2 - mean * mean, 1e-12) / n;
        zSum += mean - T;
        varSum += var;
        ++rays;
    }
    REQUIRE(rays > 40);
    const double z = zSum / std::sqrt(varSum);
    INFO("rays " << rays << ", z " << z);
    CHECK(std::fabs(z) < 3.5);
}

TEST_CASE("Volumes that change size over the shutter are no light sources", "[volume][render][light]") {
    // A volume light's sample pdf divides by one cell's world volume: a
    // volume scaled over the shutter is no light source (its emission is
    // collected where paths cross it, unbiased); moved alone, it is one.
    for (const bool grows : {false, true}) {
        INFO("grows " << grows);
        Scene scene;
        const MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, absorbingEmitter(1.0f, 2.0f, 0.0f));
        VolumeDesc desc;
        desc.fields.push_back({"density", "density", slabGrid(1.0f), Affine3f{}});
        desc.material = m;
        desc.light = true;
        const VolumeId id = scene.createVolume();
        scene.updateVolume(id, desc);
        Mat4 end = translation({0.5f, 0.0f, 0.0f});
        if (grows)
            end.m[0][0] = end.m[1][1] = end.m[2][2] = 2.0;
        scene.updateVolumeMotion(id, {Mat4::identity(), end});
        scene.commit();
        const VolumeRecord &rec = scene.volumeRecord(id);
        CHECK((rec.lightPower > 0.0f) == !grows);
        CHECK((rec.lightIndex != kInvalidId) == !grows);
    }
}
