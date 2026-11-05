// M9 light tree (docs/design/M9.md D11, D12; research
// docs/research/m9/lights.md 6.11): the probability of every light from its
// bit trail equals what sampling returns and how often it returns it; the
// probabilities sum to at most 1 and to 1 where some light can reach the
// receiver; lights at infinity share the selection by irradiance; the
// tree's estimate of the direct light from many small lights is unbiased
// and far less noisy than selection by power.

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

TEST_CASE("Light linking beyond 63 receiver sets drops no linked light", "[lights][linking]") {
    // D15: light b links to category "c<b>"; object k (1..127) is in the
    // categories of its index's bits: 127 distinct combinations. The first
    // 62 get exact sets, the rest share the overflow set 63, which every
    // light linked to one of their categories lights: every object is still
    // lit by each light linked to it, exact sets by those alone, and an
    // object without categories (set 0) by no linked light.
    Scene scene;
    for (int b = 0; b < 7; ++b) {
        LightDesc l;
        l.type = LightType::Sphere;
        l.radius = 0.1f;
        l.transform = translation({float(b), 0.0f, 3.0f});
        l.lightLink = "c" + std::to_string(b);
        scene.updateLight(scene.createLight(), l);
    }
    std::vector<MeshId> objects;
    for (int k = 1; k <= 127; ++k) {
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, makeQuad(0.1f));
        std::vector<std::string> categories;
        for (int b = 0; b < 7; ++b)
            if ((k >> b) & 1)
                categories.push_back("c" + std::to_string(b));
        scene.setMeshCategories(id, categories);
        objects.push_back(id);
    }
    const MeshId plain = scene.createMesh();
    scene.updateMesh(plain, makeQuad(0.1f));
    scene.commit();
    const uint64_t *mask = scene.lightLinkMasks();
    REQUIRE(mask != nullptr);
    int exact = 0;
    for (int k = 1; k <= 127; ++k) {
        const uint32_t set = scene.receiverSet(objects[size_t(k - 1)], 0u);
        INFO("object " << k << ", set " << set);
        REQUIRE(set < 64u);
        CHECK(set != 0u);
        exact += set < 63u ? 1 : 0;
        for (int b = 0; b < 7; ++b) {
            const bool lit = ((mask[b] >> set) & 1u) != 0;
            if ((k >> b) & 1)
                CHECK(lit);
            else if (set < 63u)
                CHECK_FALSE(lit);
        }
    }
    CHECK(exact == 62);
    CHECK(scene.receiverSet(plain, 0u) == 0u);
    for (int b = 0; b < 7; ++b)
        CHECK((mask[b] & 1u) == 0u);
}