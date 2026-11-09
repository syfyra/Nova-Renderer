// Path guiding (docs/design/M11-P9c.md): the quadtree's density and its
// sampling, unbiased one-sample MIS of guided closures and of guided renders
// (an analytic furnace), the field's training and refinement, deterministic
// renders whatever the thread count, the controls (components, training
// samples, clearbuffers) and the noise drop on an indirect-lit room. Karma's
// measured behaviour is compared in tests/husk/check_karma_guiding.py; the
// XPU in test_gpu_guiding.cpp.

#include "guiding_scenes.h"

#include "core/bsdf/surface.h"
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
        // restarts after the training samples and continues their sample
        // numbers (the same numbers again would correlate with the field).
        s.guiding.trainingSamples = 32;
        s.guiding.clearBuffers = true;
        s.maxSamples = 64;
        const GuideRun run = renderGuided(scene, cam, 96, 72, s);
        CHECK(run.discarded == 32u);
        CHECK(run.film.sampleOffset == 32u);
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

// Exploration (hidden): noise and field size of the room at a size and
// sample count from the environment (NOVA_GUIDE_W, NOVA_GUIDE_SPP).
TEST_CASE("Guided room noise (exploration)", "[.][guidebench]") {
    const int w = std::getenv("NOVA_GUIDE_W") ? std::atoi(std::getenv("NOVA_GUIDE_W")) : 160;
    const int h = w * 3 / 4;
    const uint32_t spp = std::getenv("NOVA_GUIDE_SPP") ? uint32_t(std::atoi(std::getenv("NOVA_GUIDE_SPP"))) : 32;
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
