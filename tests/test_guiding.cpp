// Path guiding (docs/design/M11-P9c.md): the quadtree's density and its
// sampling, unbiased one-sample MIS of guided closures (their event classes
// and thin sheets smooth on one side too) and of guided renders (an analytic
// furnace, fog with equiangular light samples), the field's training and
// refinement, deterministic renders whatever the thread count, the controls
// (components, training samples, clearbuffers), the noise drop on an
// indirect-lit room, and the film's sample numbers across every restart
// (IPR, resize, frames, checkpoints, guiding off). Karma's measured
// behaviour is compared in tests/husk/check_karma_guiding.py; the XPU in
// test_gpu_guiding.cpp.

#include "guiding_scenes.h"

#include "core/bsdf/surface.h"
#include "core/film/lpe_compile.h"
#include "core/integrator/guide_field.h"
#include "core/integrator/guiding.h"
#include "core/math/frame.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sf = nova::surface_field;
using Catch::Matchers::WithinAbs;

namespace {

// A field of one spatial leaf whose sampling quadtree has uneven random
// sums, three levels deep in places.
struct HandGuide {
    std::vector<GuideSpatialNode> spatial{{0, 0}};
    std::vector<GuideLeaf> leaves{{0, 0}};
    std::vector<GuideDirNode> nodes;
    GuideView view;

    explicit HandGuide(uint32_t seed) {
        Rng rng = Rng::make(seed, 7);
        const auto node = [&] {
            GuideDirNode n{};
            for (float &s : n.sum)
                s = 0.05f + rng.next() * rng.next() * 4.0f;
            nodes.push_back(n);
            return uint32_t(nodes.size() - 1);
        };
        const auto total = [&](uint32_t n) {
            return nodes[n].sum[0] + nodes[n].sum[1] + nodes[n].sum[2] + nodes[n].sum[3];
        };
        const uint32_t root = node();
        const uint32_t a = node(), b = node(), c = node();
        nodes[root].child[1] = a;
        nodes[a].child[2] = b;
        nodes[root].child[2] = c;
        // A quadrant's sum is its child's total.
        nodes[a].sum[2] = total(b);
        nodes[root].sum[1] = total(a);
        nodes[root].sum[2] = total(c);
        view.spatial = spatial.data();
        view.leaves = leaves.data();
        view.nodes = nodes.data();
        view.origin = {-1.0f, -1.0f, -1.0f};
        view.size = 2.0f;
        view.components = guide::kDefaultComponents;
        view.sampling = 1;
    }
};

Vec3f uniformSphere(Vec2f u) {
    const float z = 1.0f - 2.0f * u.x, r = std::sqrt(std::fmax(0.0f, 1.0f - z * z)), phi = kTwoPi * u.y;
    return {r * std::cos(phi), r * std::sin(phi), z};
}

SurfaceInputs emissiveGrey(float albedo, float emission) {
    SurfaceInputs in = standardSurfaceDefaults();
    in.set(sf::baseColor, spectrum(albedo));
    in.set(sf::specularWeight, 0.0f);
    in.set(sf::emissionWeight, 1.0f);
    in.set(sf::emissionColor, spectrum(emission));
    return in;
}

RenderSettings guided(uint32_t spp, bool on = true) {
    RenderSettings s;
    s.maxSamples = spp;
    s.pixelFilterScale = 0.0f;
    s.clampIndirect = 0.0f;
    s.guiding.enable = on;
    return s;
}

} // namespace

TEST_CASE("Guide quadtree: the density integrates to one, sampling follows it", "[guiding]") {
    for (uint32_t seed : {1u, 2u, 3u}) {
        const HandGuide g(seed);
        // The density over the sphere integrates to one.
        Rng rng = Rng::make(seed, 1);
        const int n = 400000;
        double integral = 0.0;
        for (int i = 0; i < n; ++i)
            integral += guidePdf(g.view, 0, uniformSphere(rng.next2()));
        integral *= 4.0 * kPi / n;
        CHECK_THAT(integral, WithinAbs(1.0, 0.01));
        // Sampled directions carry the density guidePdf gives them, and
        // E[1 / pdf] is the sphere's area (every quadrant has energy).
        double area = 0.0;
        int agree = 0;
        double quadrant[4] = {};
        for (int i = 0; i < n; ++i) {
            float pdf = 0.0f;
            const Vec3f d = guideSample(g.view, 0, rng.next2(), pdf);
            REQUIRE(pdf > 0.0f);
            area += 1.0 / pdf;
            agree += std::fabs(guidePdf(g.view, 0, d) - pdf) <= 1e-3f * pdf;
            const Vec2f p = guideToSquare(d);
            quadrant[(p.x >= 0.5f ? 1 : 0) + (p.y >= 0.5f ? 2 : 0)] += 1.0;
        }
        CHECK_THAT(area / n, WithinAbs(4.0 * kPi, 0.15)); // a heavy-tailed estimate
        CHECK(agree >= int(0.999 * n));
        const GuideDirNode &root = g.nodes[0];
        const double total = root.sum[0] + root.sum[1] + root.sum[2] + root.sum[3];
        for (int q = 0; q < 4; ++q)
            CHECK_THAT(quadrant[q] / n, WithinAbs(root.sum[q] / total, 0.004));
    }
}

TEST_CASE("Guided closure sampling is unbiased one-sample MIS", "[guiding]") {
    const HandGuide g(5);
    // A plastic: a diffuse base under a rough specular layer (two lobes,
    // only the diffuse one guided by default), and a pure diffuse closure.
    for (const bool plastic : {true, false}) {
        SurfaceInputs in = standardSurfaceDefaults();
        in.set(sf::baseColor, Spectrum{0.6f, 0.5f, 0.4f});
        in.set(sf::specularWeight, plastic ? 1.0f : 0.0f);
        in.set(sf::specularRoughness, 0.3f);
        const ShadingGeometry geo{{0, 0, 1}, {0, 0, 1}, {1, 0, 0}, false, 0.0f};
        const Vec3f wo = normalize(Vec3f{0.3f, -0.2f, 0.9f});
        const ClosureContext ctx{&bsdfTables(), kXyzToRec709Grey};
        ClosureList cl;
        buildClosure(in, geo, wo, ctx, cl);
        ClosureList guidedList = cl;
        const GuideSite site = guideSurfaceSite(g.view, guidedList, {0, 0, 0}, 0x0F);
        REQUIRE(site.alpha > 0.0f);
        REQUIRE(guidedList.guideMask != 0);
        if (plastic)
            REQUIRE(guidedList.guideMask != (1u << guidedList.count) - 1u); // the specular lobe is not guided
        // E[weight x h(wi)] with and without the guide.
        const auto h = [](Vec3f d) { return 1.0 + 2.0 * d.x * d.x + d.y; };
        const int n = 600000;
        double sum[2] = {}, sq[2] = {};
        int consistent = 0, nonDelta = 0;
        Rng rng = Rng::make(plastic ? 11u : 12u, 3);
        for (int i = 0; i < n; ++i) {
            const Vec3f u{rng.next(), rng.next(), rng.next()};
            const float ug = rng.next();
            for (int k = 0; k < 2; ++k) {
                ClosureSample s;
                const bool ok = k == 0 ? sampleClosure(cl, wo, u, s)
                                       : sampleClosureGuided(guidedList, wo, u, ug, g.view, site, s);
                if (!ok)
                    continue;
                const double v = luminance(s.weight) * h(s.wi);
                sum[k] += v;
                sq[k] += v * v;
                if (k == 1 && !s.delta) {
                    // Light samples' MIS sees the same density.
                    ++nonDelta;
                    const float pdf = guidedPdf(g.view, site, evalClosure(guidedList, wo, s.wi), s.wi);
                    consistent += std::fabs(pdf - s.pdf) <= 1e-3f * s.pdf;
                }
            }
        }
        const double a = sum[0] / n, b = sum[1] / n;
        const double ea = std::sqrt((sq[0] / n - a * a) / n), eb = std::sqrt((sq[1] / n - b * b) / n);
        INFO("unguided " << a << " +- " << ea << ", guided " << b << " +- " << eb);
        CHECK(std::fabs(a - b) < 4.0 * std::sqrt(ea * ea + eb * eb) + 1e-4);
        CHECK(consistent >= int(0.999 * nonDelta));
    }
}

namespace {

// E[weight x h x 1{event}] of a closure's samples with and without the guide
// (alpha 0.5), within four standard errors of each other, per event.
template <typename Event>
void checkEvents(const ClosureList &cl, const ClosureList &guidedList, const GuideView &view, const GuideSite &site,
                 Vec3f wo, int events, Event event, uint32_t seed) {
    const auto h = [](Vec3f d) { return 1.0 + 2.0 * d.x * d.x + d.y; };
    const int n = 600000;
    std::vector<double> sum(size_t(events) * 2, 0.0), sq(size_t(events) * 2, 0.0);
    Rng rng = Rng::make(seed, 3);
    for (int i = 0; i < n; ++i) {
        const Vec3f u{rng.next(), rng.next(), rng.next()};
        const float ug = rng.next();
        for (int k = 0; k < 2; ++k) {
            ClosureSample s;
            const bool ok =
                k == 0 ? sampleClosure(cl, wo, u, s) : sampleClosureGuided(guidedList, wo, u, ug, view, site, s);
            if (!ok)
                continue;
            const int e = event(s);
            if (e < 0)
                continue;
            const double v = luminance(s.weight) * h(s.wi);
            sum[size_t(e) * 2 + size_t(k)] += v;
            sq[size_t(e) * 2 + size_t(k)] += v * v;
        }
    }
    for (int e = 0; e < events; ++e) {
        const double a = sum[size_t(e) * 2] / n, b = sum[size_t(e) * 2 + 1] / n;
        const double ea = std::sqrt(std::fmax(sq[size_t(e) * 2] / n - a * a, 0.0) / n);
        const double eb = std::sqrt(std::fmax(sq[size_t(e) * 2 + 1] / n - b * b, 0.0) / n);
        INFO("event " << e << ": unguided " << a << " +- " << ea << ", guided " << b << " +- " << eb);
        CHECK(std::fabs(a - b) < 4.0 * std::sqrt(ea * ea + eb * eb) + 1e-4);
    }
}

} // namespace

TEST_CASE("Guided closure samples keep the closure's event classes", "[guiding]") {
    // A guided sample's event is the lobe the closure's own sampling gives
    // its direction (not the lobe whose guide drew it): the classes' shares
    // of the weight (ray limits, fake caustics) and refractions stay.
    const HandGuide g(5);
    const ShadingGeometry geo{{0, 0, 1}, {0, 0, 1}, {1, 0, 0}, false, 0.0f};
    const Vec3f wo = normalize(Vec3f{0.3f, -0.2f, 0.9f});
    const ClosureContext ctx{&bsdfTables(), kXyzToRec709Grey};
    SECTION("a plastic: diffuse guided, the specular lobe not") {
        SurfaceInputs in = standardSurfaceDefaults();
        in.set(sf::baseColor, Spectrum{0.6f, 0.5f, 0.4f});
        in.set(sf::specularRoughness, 0.3f);
        ClosureList cl;
        buildClosure(in, geo, wo, ctx, cl);
        ClosureList guidedList = cl;
        const GuideSite site = guideSurfaceSite(g.view, guidedList, {0, 0, 0}, 0x0F);
        REQUIRE(site.alpha > 0.0f);
        REQUIRE(guidedList.guideMask != (1u << guidedList.count) - 1u);
        checkEvents(cl, guidedList, g.view, site, wo, 2,
                    [](const ClosureSample &s) { return s.rayClass == RayClass::Diffuse ? 0 : 1; }, 21);
    }
    SECTION("diffuse over rough glass: guided transmissions refract") {
        SurfaceInputs in = standardSurfaceDefaults();
        in.set(sf::baseColor, Spectrum{0.6f, 0.5f, 0.4f});
        in.set(sf::specularRoughness, 0.3f);
        in.set(sf::transmissionWeight, 0.5f);
        ClosureList cl;
        buildClosure(in, geo, wo, ctx, cl);
        ClosureList guidedList = cl;
        const GuideSite site = guideSurfaceSite(g.view, guidedList, {0, 0, 0}, 0x0F);
        REQUIRE(site.alpha > 0.0f);
        int glass = -1;
        for (int i = 0; i < cl.count; ++i)
            if (cl.lobe[i].type == LobeType::Dielectric && cl.lobe[i].transmits())
                glass = i;
        REQUIRE(glass >= 0);
        REQUIRE_FALSE(guidedList.guideMask & (1u << glass)); // only the diffuse lobe is guided
        // Every transmitted sample is the glass's refraction into it.
        Rng rng = Rng::make(9, 3);
        int transmitted = 0, refracted = 0;
        for (int i = 0; i < 200000; ++i) {
            ClosureSample s;
            if (!sampleClosureGuided(guidedList, wo, {rng.next(), rng.next(), rng.next()}, rng.next(), g.view, site,
                                     s) ||
                s.delta || !s.transmit)
                continue;
            ++transmitted;
            refracted += s.refract && s.lobe == glass && s.rayClass == RayClass::Refract &&
                         std::fabs(s.eta - cl.lobe[glass].dielectric.mediaEta()) < 1e-6f;
        }
        CHECK(transmitted > 1000);
        CHECK(refracted == transmitted);
        checkEvents(cl, guidedList, g.view, site, wo, 3,
                    [](const ClosureSample &s) {
                        return s.refract ? 2 : s.rayClass == RayClass::Diffuse ? 0 : 1;
                    },
                    22);
    }
}

TEST_CASE("Guided thin sheets smooth on one side keep their value", "[guiding]") {
    // A thin dielectric whose reflection is rough and whose transmission is
    // smooth (and the other way round): its delta part is reached on the
    // lobe's own strategy only (probability 1 - alpha) and weighs for it.
    HandGuide g(6);
    g.view.components = guide::kReflect | guide::kRefract;
    const Vec3f wo = normalize(Vec3f{0.3f, -0.2f, 0.9f});
    for (const bool roughReflection : {true, false}) {
        INFO((roughReflection ? "rough reflection, smooth transmission" : "smooth reflection, rough transmission"));
        ClosureList cl;
        cl.clear();
        cl.ng = {0, 0, 1};
        cl.frame[0] = Frame::fromNormal({0, 0, 1});
        Lobe *l = cl.push(LobeType::ThinDielectric, 0, spectrum(1.0f), 1.0f, false);
        REQUIRE(l);
        const GgxDistribution rough = GgxDistribution::make(0.3f, 0.3f), smooth = GgxDistribution::make(0.0f, 0.0f);
        l->thin = ThinDielectricLobe{roughReflection ? rough : smooth, roughReflection ? smooth : rough, 1.5f,
                                     spectrum(1.0f), spectrum(1.0f)};
        l->thin.tBoost = l->thin.tBoostSmooth = 0.0f;
        cl.finalize();
        REQUIRE(cl.nonDeltaMask == 1u);
        ClosureList guidedList = cl;
        const GuideSite site = guideSurfaceSite(g.view, guidedList, {0, 0, 0}, 0x0F);
        REQUIRE(site.alpha > 0.0f);
        REQUIRE(guidedList.guideMask == 1u);
        checkEvents(cl, guidedList, g.view, site, wo, 2, [](const ClosureSample &s) { return s.delta ? 1 : 0; },
                    roughReflection ? 31u : 32u);
    }
}

TEST_CASE("Guide field: training refines the spatial tree and the quadtrees", "[guiding]") {
    PathGuide guide;
    GuidingSettings settings;
    settings.enable = true;
    guide.reset({-1, -1, -1}, {1, 1, 1}, settings, 1);
    REQUIRE(guide.training());
    REQUIRE_FALSE(guide.sampling());
    // Iteration 0: records concentrated around +z from points spread over the
    // cube, many more than a leaf holds.
    GuideView v = guide.view();
    Rng rng = Rng::make(3, 3);
    const auto record = [&](GuideView &view, int count) {
        for (int i = 0; i < count; ++i) {
            const Vec3f P{rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f};
            Vec3f d = normalize(Vec3f{rng.next() - 0.5f, rng.next() - 0.5f, 4.0f});
            if (rng.next() < 0.1f)
                d = uniformSphere(rng.next2());
            const uint32_t leaf = guideLeafAt(view, P);
            const uint32_t slot = guideBuildSlot(view, view.leaves[leaf].buildRoot, d);
            guideAtomicAdd(view.buildSum + slot, uint64_t(view.recordScale));
            guideAtomicAdd(view.leafCount + view.buildLeaf[slot / 4], 1u);
        }
    };
    record(v, 20000);
    CHECK(guide.iterationEnds(1));
    CHECK_FALSE(guide.endPass(1));
    CHECK(guide.sampling());
    CHECK(guide.iterations() == 1);
    CHECK(guide.leaves.size() > 1); // 20000 records over a 4000-record threshold
    // The learned density favours +z.
    v = guide.view();
    const uint32_t root = v.leaves[guideLeafAt(v, {0.1f, 0.2f, 0.3f})].sampleRoot;
    REQUIRE(root != kInvalidId);
    CHECK(guidePdf(v, root, {0, 0, 1}) > 10.0f * guidePdf(v, root, {0, 0, -1}));
    // Iteration 1 covers 2 samples per pixel; the building quadtrees are
    // finer where the energy is.
    CHECK_FALSE(guide.iterationEnds(1));
    record(v, 40000);
    CHECK_FALSE(guide.endPass(1));
    CHECK(guide.iterationEnds(1));
    CHECK_FALSE(guide.endPass(1));
    CHECK(guide.iterations() == 2);
}

TEST_CASE("Guide field: training samples end training", "[guiding]") {
    PathGuide guide;
    GuidingSettings settings;
    settings.enable = true;
    settings.trainingSamples = 5;
    guide.reset({0, 0, 0}, {1, 1, 1}, settings, 1);
    // Iterations end after 1 and 3 samples, training after 5 (its last
    // iteration, cut to 2 samples, is as long as the one before: rebuilt).
    int ended = 0;
    for (int pass = 0; pass < 8; ++pass) {
        if (guide.endPass(1))
            ++ended;
    }
    CHECK(ended == 1);
    CHECK_FALSE(guide.training());
    CHECK(guide.trainedSamples() == 5);
    CHECK(guide.view().training == 0);
}

TEST_CASE("Guided renders: an analytic furnace keeps its value", "[guiding][render]") {
    // A closed box whose walls emit 1 and reflect 0.5 diffusely: L = 1 /
    // (1 - 0.5) = 2 everywhere, with unlimited bounces.
    Scene scene;
    const MaterialId m = scene.createMaterial();
    MaterialDesc d;
    d.inputs = emissiveGrey(0.5f, 1.0f);
    scene.updateMaterial(m, d);
    for (const MeshDesc &q :
         {guideQuad({-1, 0, -1}, {2, 0, 0}, {0, 0, 2}, 0.5f), guideQuad({-1, 2, -1}, {0, 0, 2}, {2, 0, 0}, 0.5f),
          guideQuad({-1, 0, -1}, {0, 2, 0}, {2, 0, 0}, 0.5f), guideQuad({-1, 0, 1}, {2, 0, 0}, {0, 2, 0}, 0.5f),
          guideQuad({-1, 0, -1}, {0, 0, 2}, {0, 2, 0}, 0.5f), guideQuad({1, 0, -1}, {0, 2, 0}, {0, 0, 2}, 0.5f)}) {
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, q);
        scene.setMeshMaterials(id, {m});
    }
    scene.commit();
    RenderSettings s = guided(64);
    s.maxDepth = 64;
    for (float &l : s.limits.limit)
        l = 64.0f;
    const auto mean = [](const Film &film) {
        double sum = 0.0;
        for (double v : pixelLuminance(film))
            sum += v;
        return sum / double(film.pixelCount());
    };
    const GuideRun run = renderGuided(scene, guideRoomCamera(24, 24), 24, 24, s);
    CHECK(run.iterations >= 5);
    s.guiding.enable = false;
    const GuideRun off = renderGuided(scene, guideRoomCamera(24, 24), 24, 24, s);
    INFO("guided " << mean(run.film) << ", unguided " << mean(off.film));
    CHECK_THAT(mean(run.film), WithinAbs(2.0, 0.03));
    CHECK_THAT(mean(run.film), WithinAbs(mean(off.film), 0.03));
}

TEST_CASE("Guided renders: the room keeps its mean, its noise drops", "[guiding][render]") {
    Scene scene;
    guideRoom(scene);
    scene.commit();
    const Camera cam = guideRoomCamera(96, 72);
    std::vector<std::vector<double>> off, on, frozen;
    for (uint32_t seed = 0; seed < 6; ++seed) {
        RenderSettings s = guided(32, false);
        s.seed = seed;
        off.push_back(pixelLuminance(renderGuided(scene, cam, 96, 72, s).film));
        s.guiding.enable = true;
        on.push_back(pixelLuminance(renderGuided(scene, cam, 96, 72, s).film));
        // 32 samples with the field trained on 32 others, frozen: the film
        // restarts after the training samples' numbers (the same numbers
        // again would correlate with the field), at the next block of 64.
        s.guiding.trainingSamples = 32;
        s.guiding.clearBuffers = true;
        s.maxSamples = 64;
        const GuideRun run = renderGuided(scene, cam, 96, 72, s);
        CHECK(run.discarded == 32u);
        CHECK(run.film.sampleOffset == 64u);
        frozen.push_back(pixelLuminance(run.film));
    }
    const SeedStats a = seedStats(off), b = seedStats(on), f = seedStats(frozen);
    INFO("off: mean " << a.mean << " +- " << a.error << " noise " << a.noise << "; on: mean " << b.mean << " +- "
                      << b.error << " noise " << b.noise << "; frozen: mean " << f.mean << " +- " << f.error
                      << " noise " << f.noise);
    CHECK(std::fabs(a.mean - b.mean) < 4.0 * std::sqrt(a.error * a.error + b.error * b.error) + 1e-4);
    CHECK(std::fabs(a.mean - f.mean) < 4.0 * std::sqrt(a.error * a.error + f.error * f.error) + 1e-4);
    CHECK(b.noise < a.noise); // training throughout: little to learn from at this size
    CHECK(f.noise < 0.75 * a.noise);
}

TEST_CASE("Guided renders are deterministic whatever the thread count", "[guiding][render]") {
    Scene scene;
    guideRoom(scene);
    scene.commit();
    const Camera cam = guideRoomCamera(64, 48);
    const RenderSettings s = guided(16);
    const GuideRun one = renderGuided(scene, cam, 64, 48, s, 1);
    const GuideRun all = renderGuided(scene, cam, 64, 48, s, 0);
    const GuideRun again = renderGuided(scene, cam, 64, 48, s, 3);
    REQUIRE(one.iterations >= 4);
    CHECK(sameSums(one.film, all.film));
    CHECK(sameSums(one.film, again.film));
}

TEST_CASE("Guiding components and training controls", "[guiding][render]") {
    Scene scene;
    guideRoom(scene);
    scene.commit();
    const Camera cam = guideRoomCamera(32, 24);
    const GuideRun off = renderGuided(scene, cam, 32, 24, guided(8, false));
    SECTION("no listed component in the scene: the image of guiding off") {
        RenderSettings s = guided(8);
        s.guiding.components = guide::kReflect | guide::kRefract;
        const GuideRun run = renderGuided(scene, cam, 32, 24, s);
        CHECK(sameSums(run.film, off.film));
    }
    SECTION("before the first iteration ends nothing is guided") {
        const GuideRun run = renderGuided(scene, cam, 32, 24, guided(1));
        const GuideRun base = renderGuided(scene, cam, 32, 24, guided(1, false));
        CHECK(sameSums(run.film, base.film));
    }
    SECTION("training samples, then the frozen field") {
        RenderSettings s = guided(16);
        s.guiding.trainingSamples = 4;
        const GuideRun run = renderGuided(scene, cam, 32, 24, s);
        CHECK(run.trainingDone);
        CHECK(run.discarded == 0);
        CHECK(run.film.count[0] == 16u);
    }
    SECTION("clearbuffers: the training samples leave the film, the total stays") {
        RenderSettings s = guided(16);
        s.guiding.trainingSamples = 4;
        s.guiding.clearBuffers = true;
        const GuideRun run = renderGuided(scene, cam, 32, 24, s);
        CHECK(run.trainingDone);
        CHECK(run.discarded == 4u);
        for (uint32_t c : run.film.count)
            CHECK(c == 12u);
    }
    SECTION("clearbuffers while training throughout drops nothing") {
        RenderSettings s = guided(16);
        s.guiding.clearBuffers = true;
        const GuideRun run = renderGuided(scene, cam, 32, 24, s);
        CHECK_FALSE(run.trainingDone);
        CHECK(run.film.count[0] == 16u);
    }
}

TEST_CASE("Guiding across IPR restarts", "[guiding][render][ipr]") {
    Scene scene;
    guideRoom(scene);
    scene.commit();
    Renderer renderer;
    renderer.setScene(&scene);
    renderer.setSettings(guided(64));
    renderer.setCamera(guideRoomCamera(32, 24));
    renderer.resize(32, 24);
    for (int pass = 0; pass < 8; ++pass)
        renderer.renderPass({});
    REQUIRE(renderer.guide());
    const uint32_t trained = renderer.guide()->trainedSamples();
    CHECK(trained == 8u);
    CHECK(renderer.guide()->iterations() == 3u);
    // A camera move restarts the film, not the field: its samples continue
    // after the ones the field learned from (at the next block of 64).
    renderer.setCamera(guideRoomCamera(32, 24));
    renderer.reset();
    CHECK(renderer.film().sampleOffset == 64u);
    renderer.renderPass({});
    CHECK(renderer.guide()->trainedSamples() == 9u);
    // A scene edit (the sun brighter) retrains from scratch.
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.angleDeg = 0.0f;
    sun.intensity = 9.0f;
    scene.updateLight(0, sun);
    scene.commit();
    renderer.reset();
    renderer.renderPass({});
    CHECK(renderer.guide()->trainedSamples() == 1u);
    CHECK(renderer.guide()->iterations() == 1u);
    CHECK(renderer.film().sampleOffset == 0u);
    // Guiding off: no field.
    RenderSettings off = guided(64, false);
    renderer.setSettings(off);
    renderer.reset();
    renderer.renderPass({});
    CHECK(renderer.film().count[0] == 1u);
}

TEST_CASE("Guiding: every film restart starts past the field's sample numbers", "[guiding][render][ipr]") {
    Scene scene;
    guideRoom(scene);
    scene.commit();
    Renderer renderer;
    renderer.setScene(&scene);
    renderer.setSettings(guided(64));
    renderer.setCamera(guideRoomCamera(32, 24));
    renderer.resize(32, 24);
    for (int pass = 0; pass < 8; ++pass)
        renderer.renderPass({});
    REQUIRE(renderer.guide());
    REQUIRE(renderer.guide()->sampling());
    SECTION("a resize, then a reset (Hydra's order)") {
        renderer.resize(40, 30);
        CHECK(renderer.film().sampleOffset == 64u);
        renderer.reset();
        CHECK(renderer.film().sampleOffset == 64u); // nothing drawn from 64 yet
        for (int pass = 0; pass < 3; ++pass)
            renderer.renderPass({});
        renderer.reset();
        CHECK(renderer.film().sampleOffset == 128u);
        CHECK(renderer.guide()->trainedSamples() == 11u); // the same field throughout
    }
    SECTION("a cancelled pass's numbers count too (its records may be in the field)") {
        renderer.reset();
        REQUIRE(renderer.film().sampleOffset == 64u);
        renderer.renderPass([] { return true; });
        renderer.reset();
        CHECK(renderer.film().sampleOffset == 128u);
    }
    SECTION("guiding off: no field, numbers from 0; on again: a new field") {
        const uint64_t generation = renderer.guide()->generation();
        renderer.setSettings(guided(64, false));
        renderer.reset();
        CHECK(renderer.film().sampleOffset == 0u);
        renderer.renderPass({});
        CHECK_FALSE(renderer.guide());
        renderer.reset();
        CHECK(renderer.film().sampleOffset == 0u);
        renderer.setSettings(guided(64));
        renderer.reset();
        renderer.renderPass({});
        REQUIRE(renderer.guide());
        CHECK(renderer.guide()->trainedSamples() == 1u);
        CHECK(renderer.guide()->generation() != generation);
        CHECK(renderer.film().sampleOffset == 0u);
    }
    SECTION("another frame: a new field (a sequence's frames are independent)") {
        RenderSettings next = guided(64);
        next.frame = 2.0f;
        renderer.setSettings(next);
        renderer.reset();
        renderer.renderPass({});
        CHECK(renderer.guide()->trainedSamples() == 1u);
        CHECK(renderer.film().sampleOffset == 0u);
    }
}

TEST_CASE("Guiding with clearbuffers: per-sample planes and checkpoints", "[guiding][render][checkpoint]") {
    Scene scene;
    guideRoom(scene);
    scene.commit();
    RenderSettings s = guided(16);
    s.guiding.trainingSamples = 4;
    s.guiding.clearBuffers = true;
    s.lpe = std::make_shared<const lpe::Compiled>(lpe::compile({"C<RD>.*L"}));
    s.utility = std::make_shared<const std::vector<UtilityRequest>>(std::vector<UtilityRequest>{
        {UtilityKind::Lpe, "C<RD>.*L", SampleFilter::Accumulate, 0.0f, 0, 0},
        {UtilityKind::PixelSamples, {}, SampleFilter::Max}});
    const auto setup = [&](Renderer &r) {
        r.setScene(&scene);
        r.setSettings(s);
        r.setCamera(guideRoomCamera(16, 12));
        r.resize(16, 12);
    };
    const auto resolvedAt = [](const Film &film, UtilityKind kind, size_t i) {
        for (const UtilityPlane &p : film.utility)
            if (p.request.kind == kind) {
                std::vector<float> out(size_t(p.width));
                resolveFilteredSample(p, i, film.count[i], out.data());
                return out[0];
            }
        return -1.0f;
    };
    const auto checkPlanes = [&](const Film &film) {
        for (const size_t i : {size_t(0), size_t(77), size_t(16 * 12 - 1)}) {
            // The samples' numbers count within the film; the filtered LPE
            // plane took each pass's sample from the film's own sums.
            CHECK(resolvedAt(film, UtilityKind::PixelSamples, i) == float(film.count[i]));
            CHECK_THAT(resolvedAt(film, UtilityKind::Lpe, i), WithinAbs(film.lpe(0)[i].x, 1e-4 + 1e-4 * film.lpe(0)[i].x));
        }
    };
    Renderer full;
    setup(full);
    while (!full.converged())
        full.renderPass({});
    CHECK(full.discardedSamples() == 4u);
    CHECK(full.film().sampleOffset == 64u);
    CHECK(full.film().count[0] == 12u);
    checkPlanes(full.film());

    // A checkpoint after the clear: the resumed render keeps the dropped
    // training samples' count and the film's sample numbers.
    Renderer first;
    setup(first);
    for (int pass = 0; pass < 6; ++pass)
        first.renderPass({});
    REQUIRE(first.discardedSamples() == 4u);
    std::vector<uint8_t> bytes;
    first.saveCheckpoint(bytes);
    Renderer resumed;
    setup(resumed);
    REQUIRE(resumed.loadCheckpoint(bytes.data(), bytes.size()));
    CHECK(resumed.discardedSamples() == 4u);
    CHECK(resumed.film().sampleOffset == 64u);
    while (!resumed.converged())
        resumed.renderPass({});
    CHECK(resumed.discardedSamples() == 4u);
    CHECK(resumed.film().sampleOffset == 64u);
    CHECK(resumed.film().count[0] == 12u);
    // A checkpoint without the guiding trailer (older files) still loads.
    Renderer old;
    setup(old);
    CHECK(old.loadCheckpoint(bytes.data(), bytes.size() - 12));
    CHECK(old.discardedSamples() == 0u);
    CHECK(old.film().sampleOffset == 0u);
}

TEST_CASE("Guided fog: equiangular light samples weigh against the guided continuation", "[guiding][render][volume]") {
    // The light samples at equiangular distances take their MIS weight
    // against the density a scattering event there continues with: the
    // guided mixture where the volume class is open. (Guards the guided
    // equiangular path; the bias of weighing against the phase function
    // alone is far below this test's resolution here: sphere-light sampling
    // densities dwarf the guided continuation's towards the light, which the
    // field learns only 1.1 to 1.4 times the phase function's.)
    Scene scene;
    guideFog(scene, 0.5f);
    scene.commit();
    const Camera cam = guideFogCamera(24, 24);
    std::vector<std::vector<double>> off, on;
    for (uint32_t seed = 0; seed < 8; ++seed) {
        off.push_back(pixelLuminance(renderGuided(scene, cam, 24, 24, guideFogSettings(64, false, seed)).film));
        const GuideRun run = renderGuided(scene, cam, 24, 24, guideFogSettings(64, true, seed));
        CHECK(run.iterations >= 5);
        on.push_back(pixelLuminance(run.film));
    }
    const SeedStats a = seedStats(off), b = seedStats(on);
    INFO("off: mean " << a.mean << " +- " << a.error << " noise " << a.noise << "; on: mean " << b.mean << " +- "
                      << b.error << " noise " << b.noise);
    CHECK(a.error < 0.01 * a.mean);
    CHECK(std::fabs(a.mean - b.mean) < 4.0 * std::sqrt(a.error * a.error + b.error * b.error) + 1e-6);
}

namespace {

int envInt(const char *name, int fallback) {
#if defined(_MSC_VER)
    char *value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value)
        return fallback;
    const int v = std::atoi(value);
    std::free(value);
    return v > 0 ? v : fallback;
#else
    const char *value = std::getenv(name);
    return value && std::atoi(value) > 0 ? std::atoi(value) : fallback;
#endif
}

} // namespace

// Exploration (hidden): noise and field size of the room at a size and
// sample count from the environment (NOVA_GUIDE_W, NOVA_GUIDE_SPP).
TEST_CASE("Guided room noise (exploration)", "[.][guidebench]") {
    const int w = envInt("NOVA_GUIDE_W", 160);
    const int h = w * 3 / 4;
    const uint32_t spp = uint32_t(envInt("NOVA_GUIDE_SPP", 32));
    Scene scene;
    guideRoom(scene);
    scene.commit();
    const Camera cam = guideRoomCamera(w, h);
    std::vector<std::vector<double>> off, on, frozen;
    size_t leaves = 0, nodes = 0;
    for (uint32_t seed = 0; seed < 6; ++seed) {
        RenderSettings s = guided(spp, false);
        s.seed = seed;
        off.push_back(pixelLuminance(renderGuided(scene, cam, w, h, s).film));
        s.guiding.enable = true;
        const GuideRun run = renderGuided(scene, cam, w, h, s);
        leaves = run.leaves;
        nodes = run.nodes;
        on.push_back(pixelLuminance(run.film));
        // spp samples with the field trained on spp, frozen.
        s.guiding.trainingSamples = spp;
        s.guiding.clearBuffers = true;
        s.maxSamples = 2 * spp;
        frozen.push_back(pixelLuminance(renderGuided(scene, cam, w, h, s).film));
    }
    const SeedStats a = seedStats(off), b = seedStats(on), f = seedStats(frozen);
    std::printf("off: mean %.5f +- %.5f noise %.4f | on: mean %.5f +- %.5f noise %.4f (x%.3f) | leaves %zu nodes %zu\n",
                a.mean, a.error, a.noise, b.mean, b.error, b.noise, b.noise / a.noise, leaves, nodes);
    std::printf("off: frozen field: mean %.5f +- %.5f noise %.4f (x%.3f)\n", f.mean, f.error, f.noise,
                f.noise / a.noise);
    // How often the trained field's directions from points on the far walls
    // reach the sunlit floor patch (x -0.4..0, z -0.2..0.2), against cosine
    // sampling.
    Renderer renderer;
    RenderSettings s = guided(spp);
    renderer.setScene(&scene);
    renderer.setSettings(s);
    renderer.setCamera(cam);
    renderer.resize(w, h);
    renderToConvergence(renderer);
    const GuideView v = renderer.guide()->view();
    Rng rng = Rng::make(9, 9);
    const auto hits = [](Vec3f o, Vec3f d) {
        if (!(d.y < 0.0f))
            return false;
        const float t = -o.y / d.y;
        const float x = o.x + t * d.x, z = o.z + t * d.z;
        return x > -0.4f && x < 0.0f && z > -0.2f && z < 0.2f;
    };
    for (const Vec3f P : {Vec3f{-0.999f, 1.0f, 0.0f}, Vec3f{-0.999f, 0.5f, -0.5f}, Vec3f{0.3f, 1.0f, -0.999f},
                          Vec3f{-0.5f, 1.5f, -0.999f}}) {
        const Vec3f n = P.x < -0.99f ? Vec3f{1, 0, 0} : Vec3f{0, 0, 1};
        const uint32_t root = v.leaves[guideLeafAt(v, P)].sampleRoot;
        int g = 0, c = 0, below = 0;
        const int N = 20000;
        for (int i = 0; i < N; ++i) {
            float pdf = 0.0f;
            const Vec3f d = root != kInvalidId ? guideSample(v, root, rng.next2(), pdf) : Vec3f{0, 1, 0};
            g += hits(P, d);
            below += dot(d, n) < 0.0f;
            const Vec3f dc = Frame::fromNormal(n).toWorld(sampleCosineHemisphere(rng.next2()));
            c += hits(P, dc);
        }
        std::printf("P (%.2f %.2f %.2f): guide hits %.3f (below the surface %.3f), cosine hits %.3f\n", P.x, P.y, P.z,
                    double(g) / N, double(below) / N, double(c) / N);
        // The estimate of the patch's cosine-weighted solid angle / pi:
        // relative variance per sample with cosine sampling and with the
        // mixture.
        if (root == kInvalidId)
            continue;
        double s1 = 0, s2 = 0, m1 = 0, m2 = 0;
        for (int i = 0; i < N; ++i) {
            const Vec3f dc = Frame::fromNormal(n).toWorld(sampleCosineHemisphere(rng.next2()));
            const double wc = hits(P, dc) ? 1.0 : 0.0;
            s1 += wc;
            s2 += wc * wc;
            float pg = 0.0f;
            const Vec3f d = rng.next() < 0.5f ? guideSample(v, root, rng.next2(), pg)
                                              : Frame::fromNormal(n).toWorld(sampleCosineHemisphere(rng.next2()));
            const float cosd = dot(d, n);
            const double pm = 0.5 * (cosd > 0 ? cosd / kPi : 0.0) + 0.5 * guidePdf(v, root, d);
            const double wm = cosd > 0 && hits(P, d) ? (cosd / kPi) / pm : 0.0;
            m1 += wm;
            m2 += wm * wm;
        }
        const double ec = s1 / N, em = m1 / N;
        std::printf("    cosine: mean %.4f relvar %.2f | mixture: mean %.4f relvar %.2f\n", ec,
                    (s2 / N - ec * ec) / (ec * ec), em, (m2 / N - em * em) / (em * em));
    }
}
