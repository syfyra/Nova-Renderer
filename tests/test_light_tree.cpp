// M9 light tree (docs/design/M9.md D11, D12; research
// docs/research/m9/lights.md 6.11): the probability of every light from its
// bit trail equals what sampling returns and how often it returns it; the
// probabilities sum to at most 1 and to 1 where some light can reach the
// receiver; lights at infinity share the selection by irradiance; the
// tree's estimate of the direct light from many small lights is unbiased
// and far less noisy than selection by power. Light and shadow linking
// (D15): exact for any number of receiver sets; shadow-linked domes keep
// the light that their shadow set lets through.

#include "test_helpers.h"

#include "core/lights/light_tree.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;

namespace {

// Many small sphere lights over the plane z = 0 (the research's "street"),
// a few rects facing down, a point light and a sun.
void addStreet(Scene &scene, int count, uint32_t seed, bool extras = true) {
    Rng rng = Rng::make(seed, 0u, 0u);
    for (int i = 0; i < count; ++i) {
        LightDesc s;
        s.type = LightType::Sphere;
        s.radius = 0.05f;
        s.intensity = 1.0f + 9.0f * rng.next();
        s.color = {rng.next(), rng.next(), rng.next()};
        s.transform = translation({40.0f * rng.next() - 20.0f, 40.0f * rng.next() - 20.0f, 0.5f + 2.0f * rng.next()});
        scene.updateLight(scene.createLight(), s);
    }
    scene.updateMesh(scene.createMesh(), makeQuad(25.0f)); // bounds the scene (the sun's flux)
    if (!extras)
        return;
    for (int i = 0; i < 4; ++i) {
        LightDesc r;
        r.type = LightType::Rect;
        r.width = r.height = 1.0f;
        r.intensity = 5.0f;
        r.transform = translation({-6.0f + 4.0f * float(i), 3.0f, 3.0f}); // emits down -Z
        scene.updateLight(scene.createLight(), r);
    }
    LightDesc point;
    point.type = LightType::Sphere;
    point.treatAsPoint = true;
    point.intensity = 20.0f;
    point.transform = translation({0.0f, 0.0f, 4.0f});
    scene.updateLight(scene.createLight(), point);
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.intensity = 2.0f;
    scene.updateLight(scene.createLight(), sun);
}

} // namespace

TEST_CASE("The light tree's probabilities match its sampling", "[lights][tree]") {
    Scene scene;
    addStreet(scene, 500, 1u);
    scene.commit();
    const LightTreeView tree = scene.lightTree();
    REQUIRE(tree.nodeCount > 0);
    REQUIRE(tree.infiniteCount == 1);
    const uint32_t n = uint32_t(scene.lights().size());
    Rng rng = Rng::make(5u, 0u, 9u);
    for (int test = 0; test < 12; ++test) {
        const Vec3f p{30.0f * rng.next() - 15.0f, 30.0f * rng.next() - 15.0f, test % 3 == 0 ? 1.0f : 0.01f};
        const Vec3f nr = test % 2 ? normalize(Vec3f{rng.next() - 0.5f, rng.next() - 0.5f, 1.0f}) : Vec3f{0.0f, 0.0f, 1.0f};
        const Receiver r = Receiver(test % 3);
        INFO("receiver " << test << " at (" << p.x << ", " << p.y << ", " << p.z << ")");
        double sum = 0.0;
        std::vector<double> pmf(n);
        for (uint32_t i = 0; i < n; ++i) {
            pmf[i] = tree.pmf(i, p, nr, r, 0u);
            CHECK(pmf[i] >= 0.0);
            sum += pmf[i];
        }
        CHECK(sum <= 1.0 + 1e-4);
        CHECK(sum > 0.0); // subtrees below the receiver's horizon are dead ends: no light
        std::vector<double> freq(n, 0.0);
        const int draws = 40000;
        int none = 0;
        for (int d = 0; d < draws; ++d) {
            float pm = 0.0f;
            const uint32_t light = tree.sample((float(d) + rng.next()) / float(draws), p, nr, r, 0u, pm);
            if (light == kInvalidId) {
                ++none;
                continue;
            }
            freq[light] += 1.0 / draws;
            REQUIRE(pm > 0.0f);
            CHECK_THAT(pm, WithinAbs(pmf[light], 1e-5 * pmf[light] + 1e-9));
        }
        double tv = 0.0;
        for (uint32_t i = 0; i < n; ++i)
            tv += std::fabs(freq[i] - pmf[i]);
        CHECK(tv < 0.03);                                 // stratified draws: frequencies follow the pmf
        CHECK(std::fabs(double(none) / draws - (1.0 - sum)) < 0.01);
    }
}

TEST_CASE("Lights outside the tree (Karma's sampling mode Uniform)", "[lights][tree]") {
    // D18: a rect light with samplingmode Uniform is chosen with the lights
    // at infinity; the probabilities still match the sampling, and BSDF rays
    // still find it.
    Scene scene;
    addStreet(scene, 200, 3u);
    LightDesc kept;
    kept.type = LightType::Rect;
    kept.width = kept.height = 2.0f;
    kept.intensity = 8.0f;
    kept.outsideTree = true;
    kept.transform = translation({1.0f, -2.0f, 2.5f});
    scene.updateLight(scene.createLight(), kept);
    scene.commit();
    const std::vector<LightData> &lights = scene.lights();
    const uint32_t n = uint32_t(lights.size());
    const uint32_t outside = n - 1; // the last light created
    REQUIRE(lights[outside].outsideTree);
    const LightTreeView tree = scene.lightTree();
    REQUIRE(tree.infiniteCount == 2); // the sun and the rect
    Rng rng = Rng::make(7u, 0u, 3u);
    for (int test = 0; test < 6; ++test) {
        const Vec3f p{10.0f * rng.next() - 5.0f, 10.0f * rng.next() - 5.0f, 0.01f};
        const Vec3f nr{0.0f, 0.0f, 1.0f};
        const double pOutside = tree.pmf(outside, p, nr, Receiver::OneSided, 0u);
        CHECK(pOutside > 0.0);
        int hits = 0;
        const int draws = 40000;
        for (int d = 0; d < draws; ++d) {
            float pm = 0.0f;
            const uint32_t light = tree.sample((float(d) + rng.next()) / float(draws), p, nr, Receiver::OneSided, 0u, pm);
            if (light == outside) {
                ++hits;
                CHECK_THAT(pm, WithinAbs(pOutside, 1e-5 * pOutside));
            }
        }
        CHECK(std::fabs(double(hits) / draws - pOutside) < 0.01);
    }
    // BSDF rays crossing it: the tree's traversal finds what the loop finds.
    int found = 0;
    for (int k = 0; k < 4000; ++k) {
        const Vec3f o{4.0f * rng.next() - 1.0f, 4.0f * rng.next() - 4.0f, 0.1f};
        const Vec3f d = sampleUniformSphere(rng.next2());
        std::vector<std::pair<uint32_t, float>> a, b;
        forEachAreaLightCrossing(lights.data(), n, nullptr, o, d, kInf,
                                 [&](uint32_t i, float t, Vec3f, bool) { a.emplace_back(i, t); });
        forEachAreaLightCrossing(lights.data(), n, &tree, o, d, kInf,
                                 [&](uint32_t i, float t, Vec3f, bool) { b.emplace_back(i, t); });
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        REQUIRE(a == b);
        for (const auto &x : a)
            found += x.first == outside;
    }
    CHECK(found > 50);
}

TEST_CASE("Light edits update the tree consistently", "[lights][tree]") {
    // D11: an intensity edit sums the powers up the existing tree (its
    // sampling and probabilities still agree, the root holds the new total);
    // a move builds it again, the same tree as a fresh build.
    Scene edited;
    addStreet(edited, 300, 4u);
    std::vector<LightId> ids;
    edited.commit();
    for (LightId id = 0; id < 300; ++id)
        ids.push_back(id);
    Scene fresh;
    addStreet(fresh, 300, 4u);
    Rng rng = Rng::make(4u, 0u, 0u);
    // The street's lights in creation order: re-create the edited descs.
    for (int round = 0; round < 2; ++round) {
        Rng desc = Rng::make(4u, 0u, 0u);
        for (LightId id = 0; id < 300; ++id) {
            LightDesc s;
            s.type = LightType::Sphere;
            s.radius = 0.05f;
            s.intensity = 1.0f + 9.0f * desc.next();
            s.color = {desc.next(), desc.next(), desc.next()};
            s.transform = translation({40.0f * desc.next() - 20.0f, 40.0f * desc.next() - 20.0f, 0.5f + 2.0f * desc.next()});
            if (id % 7 == 0)
                s.intensity *= round == 0 ? 3.0f : 0.5f; // power only
            if (round == 1 && id == 11)
                s.transform = translation({1.0f, 2.0f, 1.0f}); // a move
            edited.updateLight(ids[id], s);
            fresh.updateLight(id, s);
        }
        edited.commit();
        fresh.commit();
        const LightTreeView a = edited.lightTree(), b = fresh.lightTree();
        REQUIRE(a.nodeCount == b.nodeCount);
        double total = 0.0;
        for (uint32_t i = 0; i < uint32_t(edited.lights().size()); ++i)
            if (a.trail[i] != ~uint64_t(0))
                total += lightPower(edited.lights()[i], edited.sceneRadius()) * edited.lights()[i].selection;
        CHECK_THAT(a.nodes[0].phi, WithinAbs(total, 1e-4 * total));
        for (int test = 0; test < 8; ++test) {
            const Vec3f p{30.0f * rng.next() - 15.0f, 30.0f * rng.next() - 15.0f, 0.01f};
            const Vec3f n{0.0f, 0.0f, 1.0f};
            for (int d = 0; d < 200; ++d) {
                float pm = 0.0f;
                const uint32_t light = a.sample(rng.next(), p, n, Receiver::OneSided, 0u, pm);
                if (light != kInvalidId)
                    REQUIRE_THAT(pm, WithinAbs(a.pmf(light, p, n, Receiver::OneSided, 0u), 1e-5 * pm));
            }
            if (round == 1) // rebuilt: the fresh build's tree
                for (uint32_t i = 0; i < uint32_t(edited.lights().size()); ++i) {
                    const float pa = a.pmf(i, p, n, Receiver::OneSided, 0u);
                    REQUIRE_THAT(pa, WithinAbs(b.pmf(i, p, n, Receiver::OneSided, 0u), 1e-6 * pa + 1e-12));
                }
        }
    }
}

TEST_CASE("The light tree estimates direct light without bias and with less noise", "[lights][tree]") {
    // Irradiance at points on the street (the research's E1 scene: small
    // sphere lights over a plane, no occlusion), by one light sample per
    // estimate: tree against power selection.
    Scene scene;
    addStreet(scene, 2000, 2u, false);
    scene.commit();
    const LightTreeView tree = scene.lightTree();
    const LightSamplerView table = scene.lightSampler();
    const std::vector<LightData> &lights = scene.lights();
    Rng rng = Rng::make(11u, 0u, 3u);
    double relVarTree = 0.0, relVarTable = 0.0;
    for (int test = 0; test < 6; ++test) {
        const Vec3f p{20.0f * rng.next() - 10.0f, 20.0f * rng.next() - 10.0f, 0.0f}, nr{0.0f, 0.0f, 1.0f};
        // Exact irradiance from the delta and small lights by many samples each.
        const auto estimate = [&](bool useTree, Rng &r) {
            float pm = 0.0f;
            const float u = r.next();
            const uint32_t i = useTree ? tree.sample(u, p, nr, Receiver::OneSided, 0u, pm) : table.sample(u, pm);
            if (i == kInvalidId || !(pm > 0.0f))
                return 0.0;
            const LightSample ls = sampleLight(lights[i], p, r.next2());
            if (!(ls.pdf > 0.0f))
                return 0.0;
            const double cosT = std::fmax(0.0, double(dot(ls.wi, nr)));
            return double(luminance(ls.Li)) * cosT / (double(pm) * double(ls.pdf));
        };
        double s[2] = {0.0, 0.0}, sq[2] = {0.0, 0.0};
        const int m = 200000;
        for (int k = 0; k < m; ++k)
            for (int t = 0; t < 2; ++t) {
                const double e = estimate(t == 0, rng);
                s[t] += e;
                sq[t] += e * e;
            }
        const double mean[2] = {s[0] / m, s[1] / m};
        const double var[2] = {sq[0] / m - mean[0] * mean[0], sq[1] / m - mean[1] * mean[1]};
        INFO("point " << test << ": tree " << mean[0] << " (var " << var[0] << "), table " << mean[1] << " (var "
                      << var[1] << ")");
        // Unbiased: the two means agree within their standard errors.
        CHECK(std::fabs(mean[0] - mean[1]) < 5.0 * std::sqrt(var[0] / m + var[1] / m));
        relVarTree += var[0] / (mean[0] * mean[0]);
        relVarTable += var[1] / (mean[1] * mean[1]);
    }
    INFO("relative variance: tree " << relVarTree << ", table " << relVarTable);
    CHECK(relVarTree * 50.0 < relVarTable); // research E1: 107x at 1k lights (measured here: 104x)
}


TEST_CASE("Rays find the area lights they cross through the tree", "[lights][tree]") {
    // D13: the tree's box traversal finds exactly the lights the loop over
    // every light finds (rects, spheres, from inside the light field too).
    Scene scene;
    addStreet(scene, 3000, 4u);
    scene.commit();
    const LightTreeView tree = scene.lightTree();
    const std::vector<LightData> &lights = scene.lights();
    const uint32_t n = uint32_t(lights.size());
    Rng rng = Rng::make(2u, 7u, 1u);
    int found = 0;
    for (int k = 0; k < 20000; ++k) {
        const Vec3f o{40.0f * rng.next() - 20.0f, 40.0f * rng.next() - 20.0f, 3.0f * rng.next()};
        const Vec3f d = sampleUniformSphere(rng.next2());
        const float tMax = rng.next() < 0.5f ? kInf : 30.0f * rng.next();
        std::vector<std::pair<uint32_t, float>> a, b;
        forEachAreaLightCrossing(lights.data(), n, nullptr, o, d, tMax,
                                 [&](uint32_t i, float t, Vec3f, bool) { a.emplace_back(i, t); });
        forEachAreaLightCrossing(lights.data(), n, &tree, o, d, tMax,
                                 [&](uint32_t i, float t, Vec3f, bool) { b.emplace_back(i, t); });
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        INFO("ray " << k);
        REQUIRE(a == b);
        found += int(a.size());
    }
    CHECK(found > 100); // the rays do cross lights
}

TEST_CASE("Area lights with sampling quality 0 are still found by rays", "[lights][tree]") {
    // D18: quality 0 takes a light's selection weight, not its emission. The
    // rect joins the lights outside the tree with weight 0: never chosen,
    // probability 0 (BSDF rays weigh it fully), and rays crossing it find it,
    // alone (an empty tree) and next to a light in the tree.
    for (bool other : {false, true}) {
        INFO("with a light in the tree: " << other);
        Scene scene;
        scene.updateMesh(scene.createMesh(), makeQuad(5.0f));
        LightDesc rect;
        rect.type = LightType::Rect;
        rect.width = rect.height = 1.0f;
        rect.intensity = 5.0f;
        rect.samplingQuality = 0.0f;
        rect.transform = translation({0.0f, 0.0f, 3.0f}); // emits down -Z
        scene.updateLight(scene.createLight(), rect);
        if (other) {
            LightDesc sphere;
            sphere.type = LightType::Sphere;
            sphere.radius = 0.1f;
            sphere.transform = translation({2.0f, 0.0f, 2.0f});
            scene.updateLight(scene.createLight(), sphere);
        }
        scene.commit();
        const std::vector<LightData> &lights = scene.lights();
        const uint32_t n = uint32_t(lights.size());
        REQUIRE(lights[0].selection == 0.0f);
        const LightTreeView tree = scene.lightTree();
        CHECK(tree.nodeCount == (other ? 1u : 0u));
        REQUIRE(tree.infiniteCount == 1u);
        CHECK(tree.infinite[0] == 0u);
        const Vec3f p{0.2f, 0.1f, 0.0f}, nr{0.0f, 0.0f, 1.0f};
        CHECK(tree.pmf(0u, p, nr, Receiver::OneSided, 0u) == 0.0f);
        for (int d = 0; d < 256; ++d) {
            float pm = 0.0f;
            CHECK(tree.sample((float(d) + 0.5f) / 256.0f, p, nr, Receiver::OneSided, 0u, pm) != 0u);
        }
        int found = 0;
        for (int k = 0; k < 64; ++k) {
            const Vec3f o{-0.4f + 0.8f * float(k % 8) / 7.0f, -0.4f + 0.8f * float(k / 8) / 7.0f, 0.1f};
            forEachAreaLightCrossing(lights.data(), n, &tree, o, Vec3f{0.0f, 0.0f, 1.0f}, kInf,
                                     [&](uint32_t i, float, Vec3f, bool) { found += i == 0u; });
        }
        CHECK(found == 64);
    }
}

TEST_CASE("Spot lights wider than 90 degrees keep their light behind them", "[lights][tree]") {
    // A point light aimed down with a 150 degree cone of softness 1 still
    // lights a receiver above its plane, 96 degrees off its axis; a second,
    // negligible light makes selection weigh importances: the spot keeps
    // nearly all of it (before, the cone's bounds gave it none there).
    Scene scene;
    scene.updateMesh(scene.createMesh(), makeQuad(25.0f));
    LightDesc spot;
    spot.type = LightType::Sphere;
    spot.treatAsPoint = true;
    spot.intensity = 10.0f;
    spot.shaped = true;
    spot.coneAngleDeg = 150.0f;
    spot.coneSoftness = 1.0f;
    spot.transform = translation({0.0f, 0.0f, 2.0f}); // aimed down -Z
    scene.updateLight(scene.createLight(), spot);
    LightDesc dim;
    dim.type = LightType::Sphere;
    dim.treatAsPoint = true;
    dim.intensity = 0.001f;
    dim.transform = translation({40.0f, 0.0f, 2.0f});
    scene.updateLight(scene.createLight(), dim);
    scene.commit();
    const std::vector<LightData> &lights = scene.lights();
    const LightTreeView tree = scene.lightTree();
    REQUIRE(tree.nodeCount == 3u);
    const Vec3f p{3.0f, 0.0f, 2.3f};
    const Vec3f nr = normalize(Vec3f{0.0f, 0.0f, 2.0f} - p);
    const LightSample ls = sampleLight(lights[0], p, Vec2f{0.5f, 0.5f});
    REQUIRE(luminance(ls.Li) > 0.0f); // the cone emits there
    CHECK(tree.pmf(0u, p, nr, Receiver::OneSided, 0u) > 0.5f);
    CHECK(tree.pmf(0u, p, nr, Receiver::None, 0u) > 0.5f);
}

namespace {

// 256 objects in the categories of their index's bits ("c0".."c7"): object
// k is a quad of a 16 x 16 grid on the plane z = 0 (cell centre (k % 16 -
// 7.5, k / 16 - 7.5)), object 0 has no categories. 255 distinct light-link
// combinations, far more than a 64-bit mask over receiver sets holds.
std::vector<MeshId> addLinkGrid(Scene &scene) {
    std::vector<MeshId> objects;
    for (int k = 0; k < 256; ++k) {
        MeshDesc m = makeQuad(0.45f);
        m.transform = translation({float(k % 16) - 7.5f, float(k / 16) - 7.5f, 0.0f});
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, m);
        std::vector<std::string> categories;
        for (int b = 0; b < 8; ++b)
            if ((k >> b) & 1)
                categories.push_back("c" + std::to_string(b));
        scene.setMeshCategories(id, categories);
        objects.push_back(id);
    }
    return objects;
}

// Sphere light b (0..7) on a circle above the grid, linked to "c<b>"
// (`linked`) or lighting everything.
void addLinkLight(Scene &scene, int b, bool linked) {
    LightDesc l;
    l.type = LightType::Sphere;
    l.radius = 0.5f;
    l.intensity = 10.0f + 2.0f * float(b);
    const float a = kTwoPi * float(b) / 8.0f;
    l.transform = translation({6.0f * std::cos(a), 6.0f * std::sin(a), 4.0f});
    if (linked)
        l.lightLink = "c" + std::to_string(b);
    scene.updateLight(scene.createLight(), l);
}

// The mean luminance of each grid cell's inner pixels (an orthographic view
// of the grid, 4 x 4 pixels per cell; image row 0 at y = -8).
std::vector<double> cellMeans(const Scene &scene, uint32_t spp, bool lightTree) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.pixelFilterScale = 0.0f;
    settings.clampIndirect = 0.0f;
    settings.lightTree = lightTree;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 10.0f}, 8.0f, 64, 64));
    renderer.resize(64, 64);
    renderToConvergence(renderer);
    std::vector<double> mean(256, 0.0);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            mean[size_t((y / 4) * 16 + x / 4)] += double(luminance(pixelMean(renderer.film(), x, y))) / 16.0;
    return mean;
}

} // namespace

TEST_CASE("Light linking beyond 63 receiver sets is exact", "[lights][linking]") {
    // D15 (R1-28): light b < 8 links to category "c<b>", light 8 lights
    // every receiver; 255 distinct category combinations and set 0. Each
    // object's set is lit by exactly its lights: the links, and the light
    // tree's probabilities, 0 for every other light and summing to 1 over
    // its own (nothing wasted on rejections), with sampling that follows
    // them.
    Scene scene;
    const std::vector<MeshId> objects = addLinkGrid(scene);
    for (int b = 0; b < 8; ++b)
        addLinkLight(scene, b, true);
    LightDesc all;
    all.type = LightType::Sphere;
    all.radius = 0.3f;
    all.transform = translation({0.0f, 0.0f, 5.0f});
    scene.updateLight(scene.createLight(), all);
    scene.commit();
    const LightLinks links = scene.lightLinks();
    REQUIRE(links.light != nullptr);
    CHECK(scene.receiverSetCount() == 256u);
    const LightTreeView tree = scene.lightTree();
    REQUIRE(tree.nodeCount == 17u);
    Rng rng = Rng::make(3u, 1u, 4u);
    std::vector<uint32_t> sets;
    for (int k = 0; k < 256; ++k) {
        const uint32_t set = scene.receiverSet(objects[size_t(k)], 0u);
        INFO("object " << k << ", set " << set);
        CHECK(std::find(sets.begin(), sets.end(), set) == sets.end());
        sets.push_back(set);
        const Vec3f p{float(k % 16) - 7.5f, float(k / 16) - 7.5f, 0.0f}, n{0.0f, 0.0f, 1.0f};
        double sum = 0.0;
        for (uint32_t b = 0; b <= 8; ++b) {
            const bool lit = b == 8 || ((k >> b) & 1) != 0;
            CHECK(links.links(b, set) == lit);
            const float pm = tree.pmf(b, p, n, Receiver::OneSided, set);
            if (lit)
                CHECK(pm > 0.0f);
            else
                CHECK(pm == 0.0f);
            sum += pm;
        }
        CHECK_THAT(sum, WithinAbs(1.0, 1e-5));
        for (int d = 0; d < 32; ++d) {
            float pm = 0.0f;
            const uint32_t light = tree.sample((float(d) + rng.next()) / 32.0f, p, n, Receiver::OneSided, set, pm);
            REQUIRE(light <= 8u);
            CHECK((light == 8u || ((k >> light) & 1) != 0));
            CHECK_THAT(pm, WithinAbs(tree.pmf(light, p, n, Receiver::OneSided, set), 1e-6));
        }
    }
    CHECK(scene.receiverSet(objects[0], 0u) == 0u);
    CHECK(links.links(8u, 0u));
    for (uint32_t b = 0; b < 8; ++b)
        CHECK_FALSE(links.links(b, 0u));
}

TEST_CASE("Light linking beyond 63 receiver sets renders each object with its lights", "[render][lights][linking]") {
    // D15 (R1-28): every object of the grid is lit by exactly the lights
    // linked to one of its categories, by the light tree and by selection
    // by power: its mean equals the sum of what each of its lights alone
    // gives it (light linking acts on direct light; the coplanar quads see
    // no other object). Before, the objects past the 62nd combination shared
    // one set lit by the lights of all their categories.
    Scene linked;
    addLinkGrid(linked);
    for (int b = 0; b < 8; ++b)
        addLinkLight(linked, b, true);
    linked.commit();
    std::vector<std::vector<double>> alone;
    for (int b = 0; b < 8; ++b) {
        Scene one;
        addLinkGrid(one);
        addLinkLight(one, b, false);
        one.commit();
        alone.push_back(cellMeans(one, 32, true));
    }
    for (const bool lightTree : {true, false}) {
        INFO("light tree " << lightTree);
        const std::vector<double> got = cellMeans(linked, lightTree ? 64 : 256, lightTree);
        CHECK(got[0] == 0.0); // no categories: no linked light reaches it
        double sumErr = 0.0, worst = 0.0;
        int worstCell = 0;
        for (int k = 1; k < 256; ++k) {
            double want = 0.0;
            for (int b = 0; b < 8; ++b)
                if ((k >> b) & 1)
                    want += alone[size_t(b)][size_t(k)];
            REQUIRE(want > 0.0);
            const double err = std::fabs(got[size_t(k)] - want) / want;
            sumErr += err;
            if (err > worst) {
                worst = err;
                worstCell = k;
            }
        }
        INFO("mean relative error " << sumErr / 255.0 << ", worst " << worst << " at object " << worstCell);
        CHECK(sumErr / 255.0 < 0.01);
        CHECK(worst < 0.05);
    }
}

namespace {

// Shadow-linked domes (D15, R1-31): the ground z = 0 under a dome whose
// shadow set is "casters" (`linked`; else no shadow link), a black caster in
// it at z = 0.6 over x in [-3, -2.2], and a black wall x = 0.2 (z in [0, 4])
// outside the shadow set (`wall` 1), in it (2) or absent (0). Black objects
// add no light, so with the wall outside the set the ground renders as
// without it; without the wall the shadow link changes nothing. The view
// covers x in [-2, 0.2]: the ground next to the wall, never the wall.
struct DomeLinkScene {
    Scene scene;

    DomeLinkScene(bool textured, bool metal, int wall, bool linked = true) {
        const MeshId ground = scene.createMesh();
        scene.updateMesh(ground, makeQuad(8.0f));
        MaterialDesc g;
        g.inputs = lambertDefaults(spectrum(0.5f));
        if (metal) {
            g.inputs = standardSurfaceDefaults();
            g.inputs.set(surface_field::baseColor, Spectrum{0.9f, 0.8f, 0.6f});
            g.inputs.set(surface_field::metalness, 1.0f);
            g.inputs.set(surface_field::specularRoughness, 0.3f);
        }
        const MaterialId groundMaterial = scene.createMaterial();
        scene.updateMaterial(groundMaterial, g);
        scene.setMeshMaterials(ground, {groundMaterial});
        MaterialDesc b;
        b.inputs = lambertDefaults(spectrum(0.0f));
        const MaterialId black = scene.createMaterial();
        scene.updateMaterial(black, b);
        const auto quad = [&](Vec3f o, Vec3f e1, Vec3f e2, bool caster) {
            MeshDesc m;
            m.points = {o, o + e1, o + e1 + e2, o + e2};
            m.indices = {0, 1, 2, 0, 2, 3};
            const MeshId id = scene.createMesh();
            scene.updateMesh(id, m);
            scene.setMeshMaterials(id, {black});
            if (caster)
                scene.setMeshCategories(id, {"casters"});
        };
        quad({-3.0f, -6.0f, 0.6f}, {0.8f, 0.0f, 0.0f}, {0.0f, 12.0f, 0.0f}, true);
        if (wall > 0)
            quad({0.2f, -6.0f, 0.0f}, {0.0f, 12.0f, 0.0f}, {0.0f, 0.0f, 4.0f}, wall == 2);
        LightDesc dome;
        dome.type = LightType::Dome;
        dome.shadowLink = linked ? "casters" : "";
        if (textured) {
            // A dim sky, below the mean (no light-sampling density there),
            // and a bright patch.
            auto sky = std::make_shared<Image>();
            sky->width = 64;
            sky->height = 32;
            sky->pixels.assign(64 * 32, Spectrum{0.6f, 0.7f, 0.9f});
            for (int y = 6; y < 10; ++y)
                for (int x = 36; x < 42; ++x)
                    sky->pixels[size_t(y) * 64 + size_t(x)] = spectrum(12.0f);
            dome.texture = sky;
        }
        scene.updateLight(scene.createLight(), dome);
        scene.commit();
    }

    // The mean luminance over the view and over its part next to the wall
    // (x > -0.35, the 8 rightmost columns).
    void render(LightStrategy strategy, uint32_t spp, double &all, double &nearWall) const {
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = spp;
        settings.pixelFilterScale = 0.0f;
        settings.clampIndirect = 0.0f;
        settings.lightStrategy = strategy;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({-0.9f, 0.0f, 5.0f}, 1.1f, 32, 32));
        renderer.resize(32, 32);
        renderToConvergence(renderer);
        all = nearWall = 0.0;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const double v = luminance(pixelMean(renderer.film(), x, y));
                all += v / 1024.0;
                nearWall += x >= 24 ? v / 256.0 : 0.0;
            }
    }
};

} // namespace

TEST_CASE("Shadow-linked domes light through objects outside their shadow set", "[render][lights][linking]") {
    // D15 (R1-31): only a dome's shadow set blocks its light, wherever its
    // light samples have density or not: an untextured dome (BSDF samples
    // only) and a textured one whose dim sky light sampling never picks,
    // seen on a diffuse and on a rough metal ground, under every light
    // strategy (the dome's BSDF samples are shadow rays of their own, MIS
    // against its light samples): the wall outside the shadow set changes
    // nothing, and without it the shadow-linked dome renders as the plain
    // dome, whose BSDF samples ride on the continuations (the same decisions
    // and weights: both agree up to rounding here, where only the caster is
    // in the way). Before, objects outside the shadow set blocked the BSDF
    // samples where the dome's light samples had no density.
    for (const bool textured : {false, true})
        for (const bool metal : {false, true}) {
            const DomeLinkScene reference(textured, metal, 0), outside(textured, metal, 1), inside(textured, metal, 2),
                plain(textured, metal, 0, false);
            for (const LightStrategy strategy : {LightStrategy::MIS, LightStrategy::LightOnly, LightStrategy::BsdfOnly}) {
                INFO("textured " << textured << ", metal " << metal << ", strategy " << int(strategy));
                double refAll, refNear, outAll, outNear, plainAll, plainNear, inAll, inNear;
                reference.render(strategy, 64, refAll, refNear);
                outside.render(strategy, 64, outAll, outNear);
                plain.render(strategy, 64, plainAll, plainNear);
                REQUIRE(refNear > 0.0);
                INFO("next to the wall: " << outNear << " (no wall " << refNear << ", unlinked " << plainNear
                                          << "); all: " << outAll << " (" << refAll << ", " << plainAll << ")");
                CHECK_THAT(outNear / refNear, WithinAbs(1.0, 0.002));
                CHECK_THAT(outAll / refAll, WithinAbs(1.0, 0.002));
                CHECK_THAT(refNear / plainNear, WithinAbs(1.0, 0.002));
                CHECK_THAT(refAll / plainAll, WithinAbs(1.0, 0.002));
                if (strategy == LightStrategy::MIS) { // the wall in the shadow set does shadow
                    inside.render(strategy, 32, inAll, inNear);
                    CHECK(inNear < 0.8 * refNear);
                }
            }
        }
}