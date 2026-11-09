// M11 P6 (docs/design/M11-P6.md): Karma's per-var sample filters
// (karma:filter) on utility, beauty and LPE planes, the new raw sources,
// deep camera maps and checkpoints, on the CPU; test_gpu_render.cpp checks
// the XPU against these.

#include "test_helpers.h"

#include "core/film/lpe_compile.h"
#include "core/integrator/renderer.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

MeshDesc rect(float x0, float x1, float z, int32_t userId) {
    MeshDesc m;
    m.points = {{x0, -1, z}, {x1, -1, z}, {x1, 1, z}, {x0, 1, z}};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.triangleFaces = {0, 0};
    m.userId = userId;
    return m;
}

// Orthographic camera at z = 10 down -Z, 20 x 20 pixels over [-1, 1]^2
// (column c covers x in [-1 + c/10, -0.9 + c/10]): a back plane at z = 0
// (depth 10) over everything, a front one at z = 1 (depth 9) over
// x in [-1, 0.07], so column 10 is 70 % front.
struct EdgeScene {
    Scene scene;
    Camera camera = makeOrthoCamera({0.0f, 0.0f, 10.0f}, 1.0f, 20, 20);
    EdgeScene() {
        scene.updateMesh(scene.createMesh(), rect(-1.0f, 1.0f, 0.0f, 1));
        scene.updateMesh(scene.createMesh(), rect(-1.0f, 0.07f, 1.0f, 2));
        LightDesc sun;
        sun.type = LightType::Distant;
        scene.updateLight(scene.createLight(), sun);
        scene.commit();
    }
};

RenderSettings baseSettings(uint32_t spp) {
    RenderSettings s;
    s.maxSamples = spp;
    s.maxDepth = 1;
    s.pixelFilter = PixelFilterType::Box;
    return s;
}

const UtilityPlane *plane(const Film &film, UtilityKind kind, SampleFilter filter) {
    for (const UtilityPlane &p : film.utility)
        if (p.request.kind == kind && p.request.filter == filter)
            return &p;
    return nullptr;
}

bool sameBits(const std::vector<Spectrum> &a, const std::vector<Spectrum> &b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Spectrum)) == 0);
}

std::vector<float> resolved(const Film &film, UtilityKind kind, SampleFilter filter, int x, int y) {
    const UtilityPlane *p = plane(film, kind, filter);
    REQUIRE(p);
    std::vector<float> out(p->width);
    const size_t i = size_t(y) * size_t(film.width) + size_t(x);
    resolveFilteredSample(*p, i, film.count[i], out.data());
    return out;
}

} // namespace

TEST_CASE("Karma's sample filter texts", "[aov][filter]") {
    using F = SampleFilter;
    std::string w;
    CHECK(parseSampleFilter("", false) == F::Center);
    CHECK(parseSampleFilter("[\"point\",{}]", false) == F::Center);
    CHECK(parseSampleFilter("[\"ubox\",{}]", false) == F::Box);
    CHECK(parseSampleFilter("[\"accumulate\",{}]", false) == F::Accumulate);
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"zmin\"}]", false) == F::ZMin);
    CHECK(parseSampleFilter("[\"minmax\", {\"mode\": \"zmax\"}]", false) == F::ZMax);
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"min\"}]", false) == F::Min);
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"max\"}]", false) == F::Max);
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"last\"}]", false) == F::Last);
    CHECK(parseSampleFilter("[\"stats\",{\"mode\":\"var\"}]", false) == F::Var);
    CHECK(parseSampleFilter("[\"stats\",{\"mode\":\"mse\"}]", false) == F::Mse);
    CHECK(parseSampleFilter("[\"stats\",{\"mode\":\"even\"}]", false) == F::Even);
    CHECK(parseSampleFilter("[\"stats\",{\"mode\":\"odd\"}]", false) == F::Odd);
    // Karma's errors (measured): coverage modes average without a PrimId
    // var; omin/omax pick by depth with one; unknown minmax modes are min.
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"idcover\"}]", false, &w) == F::Box);
    CHECK(!w.empty());
    w.clear();
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"idcover\"}]", true, &w) == F::Box);
    CHECK(w.empty());
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"omin\"}]", false) == F::Box);
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"omin\"}]", true) == F::ZMin);
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"omax\"}]", true) == F::ZMax);
    CHECK(parseSampleFilter("[\"minmax\",{\"mode\":\"median\"}]", false, &w) == F::Min);
    CHECK(!w.empty());
}

TEST_CASE("Sample filters combine samples as Karma's", "[aov][filter]") {
    // One pixel, samples 0..3: hits 10, 9, miss, 10 (Karma's z_* columns).
    const auto run = [](SampleFilter f, float defaultValue = 0.0f) {
        Film film;
        film.resize(1, 1);
        UtilityRequest r{UtilityKind::Z, {}, f, defaultValue};
        film.setUtility({r});
        UtilityPlane &p = film.utility[0];
        const float v[4][1] = {{10.0f}, {9.0f}, {0.0f}, {10.0f}};
        const bool hit[4] = {true, true, false, true};
        for (uint32_t s = 0; s < 4; ++s)
            addFilteredSample(p, 0, s, v[s], hit[s], v[s][0]);
        float out = -1.0f;
        resolveFilteredSample(p, 0, 4, &out);
        return out;
    };
    using F = SampleFilter;
    CHECK(run(F::Box) == 29.0f / 4.0f);       // misses add 0
    CHECK(run(F::Box, 5.0f) == 5.0f + 29.0f / 4.0f); // the default value offsets (measured)
    CHECK(run(F::Accumulate) == 29.0f);
    CHECK(run(F::Min) == 0.0f);               // the miss counts as the default value
    CHECK(run(F::Min, 5.0f) == 5.0f);
    CHECK(run(F::Max) == 10.0f);
    CHECK(run(F::ZMin) == 9.0f);              // hits only
    CHECK(run(F::ZMax) == 10.0f);
    CHECK(run(F::Last) == 10.0f);
    CHECK(run(F::Even) == 5.0f);              // samples 0 and 2
    CHECK(run(F::Odd) == 9.5f);               // samples 1 and 3
    const float mean = 29.0f / 4.0f, var = ((10 - mean) * (10 - mean) * 2 + (9 - mean) * (9 - mean) + mean * mean) / 3.0f;
    CHECK_THAT(run(F::Var), WithinRel(var, 1e-5f));
    CHECK_THAT(run(F::Mse), WithinRel(var / 4.0f, 1e-5f));
    // Min and max order vectors by length: (0, 0, 1) is larger than (0.5, 0.5, 0.5).
    Film film;
    film.resize(1, 1);
    film.setUtility({{UtilityKind::P, {}, F::Max}, {UtilityKind::P, {}, F::Min}});
    const float grey[3] = {0.5f, 0.5f, 0.5f}, blue[3] = {0.0f, 0.0f, 1.0f};
    for (UtilityPlane &p : film.utility) {
        addFilteredSample(p, 0, 0, grey, true, 1.0f);
        addFilteredSample(p, 0, 1, blue, true, 1.0f);
    }
    float out[3];
    resolveFilteredSample(film.utility[0], 0, 2, out);
    CHECK(out[2] == 1.0f);
    resolveFilteredSample(film.utility[1], 0, 2, out);
    CHECK(out[0] == 0.5f);
}

TEST_CASE("Filtered utility planes at an edge", "[render][aov][filter]") {
    EdgeScene s;
    RenderSettings settings = baseSettings(256);
    using U = UtilityKind;
    using F = SampleFilter;
    settings.utility = std::make_shared<const std::vector<UtilityRequest>>(std::vector<UtilityRequest>{
        {U::Z, {}, F::Box}, {U::Z, {}, F::ZMin}, {U::Z, {}, F::ZMax}, {U::Z, {}, F::Var}, {U::P, {}, F::Box},
        {U::ObjectId, {}, F::Box}, {U::Mask, {}, F::Box}, {U::PixelSamples, {}, F::Max}, {U::Beauty, {}, F::Max},
        {U::Time, {}, F::Box}, {U::Near, {}, F::Box}, {U::RayOrigin, {}, F::Box}, {U::HitUv, {}, F::Box},
        {U::Z, {}, F::Center}});
    Renderer renderer;
    renderer.setScene(&s.scene);
    renderer.setSettings(settings);
    renderer.setCamera(s.camera);
    renderer.resize(20, 20);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    // Column 10, 70 % front (depth 9), 30 % back (depth 10).
    const float zBox = resolved(film, U::Z, F::Box, 10, 10)[0];
    CHECK_THAT(zBox, WithinAbs(0.7 * 9 + 0.3 * 10, 0.02));
    CHECK(resolved(film, U::Z, F::ZMin, 10, 10)[0] == 9.0f);
    CHECK(resolved(film, U::Z, F::ZMax, 10, 10)[0] == 10.0f);
    CHECK_THAT(resolved(film, U::Z, F::Var, 10, 10)[0], WithinAbs(0.21 * 256.0 / 255.0, 0.01));
    CHECK_THAT(resolved(film, U::P, F::Box, 10, 10)[2], WithinAbs(0.7, 0.02));
    CHECK_THAT(resolved(film, U::ObjectId, F::Box, 10, 10)[0], WithinAbs(0.7 * 2 + 0.3 * 1, 0.02));
    // Inside a face the filters agree with the centre's first hit.
    CHECK(resolved(film, U::Z, F::Box, 15, 10)[0] == 10.0f);
    CHECK(resolved(film, U::Z, F::ZMin, 5, 10)[0] == 9.0f);
    CHECK(plane(film, U::Z, F::Center)->data[10 * 20 + 5] == 9.0f);
    CHECK(resolved(film, U::Mask, F::Box, 5, 10)[0] == 1.0f);
    CHECK(resolved(film, U::PixelSamples, F::Max, 5, 10)[0] == 256.0f);
    CHECK_THAT(resolved(film, U::Time, F::Box, 5, 10)[0], WithinAbs(0.5, 0.01)); // shutter positions
    CHECK_THAT(resolved(film, U::Near, F::Box, 5, 10)[0], WithinAbs(0.0, 1e-5)); // the camera's near plane at the eye
    const std::vector<float> o = resolved(film, U::RayOrigin, F::Box, 5, 10);
    CHECK_THAT(o[2], WithinAbs(10.0, 1e-4)); // on the lens plane
    CHECK_THAT(o[0], WithinAbs(-0.45, 1e-3));
    // The face's own (u, v): x in [-1, 0.07] -> u = (x + 1) / 1.07.
    CHECK_THAT(resolved(film, U::HitUv, F::Box, 5, 10)[0], WithinAbs(0.55 / 1.07, 2e-3));
    // The beauty's maximum is at least its mean.
    const size_t i = 10 * 20 + 10;
    const std::vector<float> maxC = resolved(film, U::Beauty, F::Max, 10, 10);
    CHECK(maxC[0] >= film.colorSum[i].x / float(film.count[i]) - 1e-6f);
    CHECK(maxC[3] == 1.0f);
}

TEST_CASE("Filtered LPE planes take each pass's sample", "[render][aov][filter][lpe]") {
    EdgeScene s;
    RenderSettings settings = baseSettings(64);
    settings.adaptive = true; // passes stay at one sample per pixel with LPE planes
    settings.oracleThreshold = 1.0f;
    settings.lpe = std::make_shared<const lpe::Compiled>(lpe::compile({"C<RD>L"}));
    settings.utility = std::make_shared<const std::vector<UtilityRequest>>(std::vector<UtilityRequest>{
        {UtilityKind::Lpe, "C<RD>L", SampleFilter::Accumulate, 0.0f, 0, 0},
        {UtilityKind::Lpe, "C<RD>L", SampleFilter::Var, 0.0f, 0, 0}});
    Renderer renderer;
    renderer.setScene(&s.scene);
    renderer.setSettings(settings);
    renderer.setCamera(s.camera);
    renderer.resize(20, 20);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    for (int x : {5, 10, 15}) {
        const size_t i = 10 * 20 + size_t(x);
        // The accumulated samples are the film's sum.
        const float sum = resolved(film, UtilityKind::Lpe, SampleFilter::Accumulate, x, 10)[0];
        CHECK_THAT(sum, WithinRel(film.lpe(0)[i].x, 1e-4f));
        CHECK(resolved(film, UtilityKind::Lpe, SampleFilter::Var, x, 10)[0] >= 0.0f);
    }
}

TEST_CASE("Deep camera maps flatten to the beauty", "[render][aov][deep]") {
    EdgeScene s;
    RenderSettings settings = baseSettings(32);
    for (int compression : {0, 5}) {
        DeepRequest d;
        d.compression = compression;
        d.extras = {{UtilityKind::Z, {}}};
        settings.deep = std::make_shared<const DeepRequest>(d);
        Renderer renderer;
        renderer.setScene(&s.scene);
        renderer.setSettings(settings);
        renderer.setCamera(s.camera);
        renderer.resize(20, 20);
        renderToConvergence(renderer);
        const Film &film = renderer.film();
        REQUIRE(film.deep.size() == 1);
        const DeepPlane &deep = film.deep[0];
        for (int x : {5, 10, 15}) {
            const size_t i = 10 * 20 + size_t(x);
            const std::vector<float> &list = deep.pixels[i];
            Spectrum c{0, 0, 0};
            float a = 0.0f, z = 0.0f;
            float zmin = 1e30f, zmax = 0.0f;
            for (size_t r = 0; r < list.size(); r += deep.stride) {
                c = c + Spectrum{list[r + 1], list[r + 2], list[r + 3]};
                a += list[r + 4];
                z += list[r + 5];
                zmin = std::min(zmin, list[r]);
                zmax = std::max(zmax, list[r]);
            }
            const float n = float(film.count[i]);
            CHECK_THAT(c.x / n, WithinRel(film.colorSum[i].x / n, 1e-5f));
            CHECK_THAT(a / n, WithinAbs(film.alphaSum[i] / n, 1e-6));
            CHECK(zmin >= 9.0f - 1e-4f); // depths from the ray (float)
            CHECK(zmax <= 10.0f + 1e-4f);
            if (x == 10) // both planes
                CHECK_THAT(zmax - zmin, WithinAbs(1.0, 1e-4));
            if (compression > 0 && x != 10) // one depth: one record
                CHECK(list.size() == deep.stride);
            if (compression == 0)
                CHECK(list.size() == deep.stride * film.count[i]);
            CHECK_THAT(z / a, WithinAbs(x < 10 ? 9.0 : x > 10 ? 10.0 : 9.3, x == 10 ? 0.05 : 1e-5));
        }
    }
}

TEST_CASE("A resumed checkpoint equals the uninterrupted render", "[render][checkpoint]") {
    EdgeScene s;
    RenderSettings settings = baseSettings(16);
    settings.maxDepth = 3;
    settings.lpe = std::make_shared<const lpe::Compiled>(lpe::compile({"C<RD>L"}));
    settings.utility = std::make_shared<const std::vector<UtilityRequest>>(std::vector<UtilityRequest>{
        {UtilityKind::Z, {}, SampleFilter::Box}, {UtilityKind::P, {}, SampleFilter::Center}});
    DeepRequest d;
    settings.deep = std::make_shared<const DeepRequest>(d);
    const auto setup = [&](Renderer &r) {
        r.setScene(&s.scene);
        r.setSettings(settings);
        r.setCamera(s.camera);
        r.resize(20, 20);
    };
    Renderer full;
    setup(full);
    renderToConvergence(full);

    Renderer first;
    setup(first);
    for (int pass = 0; pass < 5; ++pass)
        first.renderPass({});
    std::vector<uint8_t> bytes;
    first.saveCheckpoint(bytes);
    Renderer resumed;
    setup(resumed);
    REQUIRE(resumed.loadCheckpoint(bytes.data(), bytes.size()));
    CHECK(resumed.samplesDone() == 5);
    renderToConvergence(resumed);
    const Film &a = full.film(), &b = resumed.film();
    CHECK(sameBits(a.colorSum, b.colorSum));
    CHECK(a.alphaSum == b.alphaSum);
    CHECK(a.count == b.count);
    CHECK(sameBits(a.lpeSum, b.lpeSum));
    CHECK(a.utility[0].data == b.utility[0].data);
    CHECK(a.utility[1].data == b.utility[1].data);
    CHECK(a.deep[0].pixels == b.deep[0].pixels);
    // A film of another layout is refused.
    RenderSettings other = settings;
    other.lpe.reset();
    Renderer mismatch;
    mismatch.setScene(&s.scene);
    mismatch.setSettings(other);
    mismatch.setCamera(s.camera);
    mismatch.resize(20, 20);
    CHECK(!mismatch.loadCheckpoint(bytes.data(), bytes.size()));
    CHECK(!resumed.loadCheckpoint(bytes.data(), bytes.size() - 1));
}
