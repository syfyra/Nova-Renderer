// M9 subsurface scattering (docs/design/M9.md, research
// docs/research/m9/sss.md 5.10): the albedo inversion; the random walk on
// the research lab's analytic geometry against its numbers (half space,
// slab, sphere, anisotropy, the segment cap, chromatic mean free paths with
// spectral MIS); closest-hit queries against one mesh instance
// (intersectLocal: instanced, moving, deforming, other objects ignored); a
// subsurface sphere in a white furnace through the renderer, also past the
// SSS limit.

#include "test_helpers.h"

#include "core/bsdf/subsurface.h"
#include "core/integrator/renderer.h"
#include "core/integrator/wavefront.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
namespace sf = nova::surface_field;

namespace {

// The lab's geometry: the medium z < 0 (half space), -d < z < 0 (slab) or
// |p| < R (sphere). prim reports the face crossed: 0 top, 1 bottom.
struct Analytic {
    enum Shape { HalfSpace, Slab, Sphere } shape = HalfSpace;
    float d = 1.0f, R = 1.0f;

    bool operator()(Vec3f p, Vec3f dir, float tMax, float &t, uint32_t &prim, Vec2f &uv) const {
        uv = {0.0f, 0.0f};
        prim = 0;
        switch (shape) {
        case HalfSpace:
            if (!(dir.z > 0.0f))
                return false;
            t = -p.z / dir.z;
            break;
        case Slab:
            if (dir.z > 0.0f) {
                t = -p.z / dir.z;
            } else if (dir.z < 0.0f) {
                t = (-d - p.z) / dir.z;
                prim = 1;
            } else {
                return false;
            }
            break;
        default: {
            const double b = dot(p, dir), c = double(dot(p, p)) - double(R) * R;
            t = float(-b + std::sqrt(std::fmax(0.0, b * b - c)));
            break;
        }
        }
        t = std::fmax(t, 0.0f);
        return t <= tMax;
    }
};

struct WalkStats {
    double mean[3] = {0.0, 0.0, 0.0};
    double error[3] = {0.0, 0.0, 0.0}; // standard error of the mean
    double segments = 0.0;             // per walk
    double capped = 0.0;               // fraction
};

// `count` walks entering at the top (half space, slab: the origin; sphere:
// its north pole), the escape weights through `face` (-1: any).
WalkStats walks(const Analytic &geo, const SubsurfaceLobe &m, int count, int face = -1,
                int maxSegments = kMaxWalkSegments, uint32_t seed = 1) {
    double sum[3] = {0.0, 0.0, 0.0}, sq[3] = {0.0, 0.0, 0.0};
    WalkStats s;
    const Vec3f entry = geo.shape == Analytic::Sphere ? Vec3f{0.0f, 0.0f, geo.R} : Vec3f{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < count; ++i) {
        Rng rng = Rng::make(uint32_t(i), 0u, seed);
        const Vec3f u{rng.next(), rng.next(), rng.next()};
        const WalkResult r = randomWalk(m, entry, {0.0f, 0.0f, -1.0f}, u, rng, maxSegments, geo);
        s.segments += r.segments;
        s.capped += r.capped ? 1.0 : 0.0;
        if (!r.exited || (face >= 0 && int(r.prim) != face))
            continue;
        for (int c = 0; c < 3; ++c) {
            sum[c] += r.weight[c];
            sq[c] += double(r.weight[c]) * r.weight[c];
        }
    }
    for (int c = 0; c < 3; ++c) {
        s.mean[c] = sum[c] / count;
        s.error[c] = std::sqrt(std::fmax(sq[c] / count - s.mean[c] * s.mean[c], 0.0) / count);
    }
    s.segments /= count;
    s.capped /= count;
    return s;
}

void checkMeans(const WalkStats &s, const double expected[3], double slack = 2e-3) {
    for (int c = 0; c < 3; ++c) {
        INFO("channel " << c << ": " << s.mean[c] << " +- " << s.error[c] << ", expected " << expected[c]);
        CHECK(std::fabs(s.mean[c] - expected[c]) < 5.0 * s.error[c] + slack);
    }
}

// van de Hulst's half-space albedo for the similarity parameter s.
double vanDeHulst(double s) { return (1.0 - s) * (1.0 - 0.139 * s) / (1.0 + 1.17 * s); }

} // namespace

TEST_CASE("The subsurface albedo inverts van de Hulst's half space", "[sss]") {
    CHECK(subsurfaceAlbedo(0.0f, 0.0f) < 1e-5f);
    CHECK(subsurfaceAlbedo(1.0f, 0.0f) == kMaxSubsurfaceAlbedo);
    for (float g : {-0.5f, 0.0f, 0.5f, 0.9f})
        for (float A = 0.05f; A < 1.0f; A += 0.05f) {
            const double alpha = subsurfaceAlbedo(A, g);
            // Undo the anisotropy correction, then the similarity parameter.
            const double aStar = alpha * (1.0 - g) / (1.0 - g * alpha);
            INFO("A " << A << ", g " << g << ": alpha " << alpha);
            CHECK_THAT(vanDeHulst(std::sqrt(1.0 - aStar)), WithinAbs(A, 2e-4));
            CHECK(alpha >= subsurfaceAlbedo(A - 0.01f, g)); // monotonic
        }
    // The medium: extinction 1 / mfp (clamped), anisotropy kept inside +-0.99.
    const SubsurfaceLobe m = SubsurfaceLobe::make({0.5f, 0.5f, 2.0f}, {0.5f, 0.0f, 2.0f}, 3.0f);
    CHECK_THAT(m.sigmaT.x, WithinAbs(2.0, 1e-6));
    CHECK_THAT(m.sigmaT.y, WithinAbs(1.0 / kMinMeanFreePath, 1.0));
    CHECK(m.g == 0.99f);
    CHECK(m.alpha.z == kMaxSubsurfaceAlbedo); // colour clamped to 1
    CHECK(m.chromatic());
}

TEST_CASE("Random walks reproduce the lab's half space", "[sss]") {
    // Research 5.10 / lab E1, E4: cosine entry, index-matched, uncapped
    // R = 0.2021 / 0.5008 / 0.7996; with Karma's 256 segments 0.9101 /
    // 0.9184 for A = 0.95 / 0.99. The half space does not depend on the
    // mean free path, chromatic or not.
    const Analytic half;
    {
        const double expected[3] = {0.2021, 0.5008, 0.7996};
        checkMeans(walks(half, SubsurfaceLobe::make({0.2f, 0.5f, 0.8f}, spectrum(1.0f), 0.0f), 200000, -1, 1 << 20),
                   expected);
        checkMeans(walks(half, SubsurfaceLobe::make({0.2f, 0.5f, 0.8f}, {1.0f, 0.5f, 0.25f}, 0.0f), 200000, -1, 1 << 20),
                   expected, 3e-3);
    }
    {
        const WalkStats s = walks(half, SubsurfaceLobe::make({0.5f, 0.95f, 0.99f}, spectrum(0.01f), 0.0f), 100000);
        const double expected[3] = {0.5008, 0.9101, 0.9184};
        checkMeans(s, expected, 3e-3);
        INFO("capped " << s.capped << ", segments " << s.segments);
        CHECK(s.capped > 0.05); // the cap is what darkens 0.95 and 0.99
    }
    // Anisotropy (uncapped, research 4.2 lab column of f_gp5 / f_gm5).
    {
        const double plus[3] = {0.2000, 0.5005, 0.8004}, minus[3] = {0.2090, 0.5043, 0.8001};
        checkMeans(walks(half, SubsurfaceLobe::make({0.2f, 0.5f, 0.8f}, spectrum(1.0f), 0.5f), 200000, -1, 1 << 20),
                   plus, 3e-3);
        checkMeans(walks(half, SubsurfaceLobe::make({0.2f, 0.5f, 0.8f}, spectrum(1.0f), -0.5f), 200000, -1, 1 << 20),
                   minus, 3e-3);
    }
}

TEST_CASE("Random walks reproduce the lab's slab and sphere", "[sss]") {
    // A slab of 2 mean free paths lit from the top: what leaves through the
    // bottom, scattered or not (lab E6): 0.2768 / 0.3745 / 0.3892.
    Analytic slab;
    slab.shape = Analytic::Slab;
    slab.d = 2.0f;
    const double through[3] = {0.2768, 0.3745, 0.3892};
    checkMeans(walks(slab, SubsurfaceLobe::make({0.5f, 0.8f, 0.95f}, spectrum(1.0f), 0.0f), 200000, 1, 1 << 20),
               through);
    // A sphere of radius 1 mfp in a white furnace (lab E5): 0.6195 /
    // 0.8911 / 0.9876.
    Analytic sphere;
    sphere.shape = Analytic::Sphere;
    const double furnace[3] = {0.6195, 0.8911, 0.9876};
    checkMeans(walks(sphere, SubsurfaceLobe::make({0.2f, 0.5f, 0.8f}, spectrum(1.0f), 0.0f), 200000), furnace);
}

TEST_CASE("Chromatic walks equal separate walks per channel", "[sss]") {
    // Scene f_chroma: A 0.8, mfp 0.3 x (1, 0.5, 0.25) in a sphere of radius
    // 1 (lab 0.9599 / 0.9256 / 0.8792): one walk with spectral MIS against
    // three achromatic walks.
    Analytic sphere;
    sphere.shape = Analytic::Sphere;
    const Spectrum mfp{0.3f, 0.15f, 0.075f};
    const WalkStats one = walks(sphere, SubsurfaceLobe::make(spectrum(0.8f), mfp, 0.0f), 200000);
    const double lab[3] = {0.9599, 0.9256, 0.8792};
    checkMeans(one, lab, 3e-3);
    double separate[3];
    for (int c = 0; c < 3; ++c)
        separate[c] = walks(sphere, SubsurfaceLobe::make(spectrum(0.8f), spectrum(mfp[c]), 0.0f), 100000, -1,
                            kMaxWalkSegments, 7)
                          .mean[c];
    checkMeans(one, separate, 3e-3);
}

TEST_CASE("intersectLocal sees one mesh instance only", "[sss][accel]") {
    // Two instances of a unit sphere at x = 0 and x = 3 and a large quad
    // through both: queries from inside instance 0 find its own surface.
    Scene scene;
    const MeshId ball = scene.createMesh();
    scene.updateMesh(ball, makeSphere(1.0f, 64, 32));
    scene.setMeshInstances(ball, {toXform34(translation({0.0f, 0.0f, 0.0f})), toXform34(translation({3.0f, 0.0f, 0.0f}))});
    const MeshId wall = scene.createMesh();
    scene.updateMesh(wall, makeQuad(10.0f)); // the plane z = 0
    scene.commit();
    float t = 0.0f;
    uint32_t prim = 0;
    Vec2f uv;
    REQUIRE(scene.intersectLocal(ball, 0, 0.0f, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f, t, prim, uv));
    CHECK_THAT(t, WithinAbs(1.0, 2e-3));
    REQUIRE(scene.intersectLocal(ball, 1, 0.0f, {3.0f, 0.0f, 0.5f}, {0.0f, 0.0f, 1.0f}, 100.0f, t, prim, uv));
    CHECK_THAT(t, WithinAbs(std::sqrt(1.0 - 0.0) - 0.5, 2e-3));
    // The quad and instance 1 are not there for instance 0; tMax is kept.
    CHECK_FALSE(scene.intersectLocal(ball, 0, 0.0f, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0.5f, t, prim, uv));
    CHECK_FALSE(scene.intersectLocal(ball, 0, 0.0f, {-5.0f, 5.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f, t, prim, uv));
    // A scaled instance: t stays the world distance.
    Mat4 big = translation({0.0f, 0.0f, 0.0f});
    big.m[0][0] = big.m[1][1] = big.m[2][2] = 2.0f;
    scene.setMeshInstances(ball, {toXform34(big)});
    scene.commit();
    REQUIRE(scene.intersectLocal(ball, 0, 0.0f, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 100.0f, t, prim, uv));
    CHECK_THAT(t, WithinAbs(2.0, 4e-3));

    // Transform motion: the sphere moves from x = 0 to x = 2 over the shutter.
    Scene moving;
    const MeshId m = moving.createMesh();
    moving.updateMesh(m, makeSphere(1.0f, 64, 32));
    moving.updateMeshMotion(m, {translation({0.0f, 0.0f, 0.0f}), translation({2.0f, 0.0f, 0.0f})});
    moving.commit();
    REQUIRE(moving.intersectLocal(m, 0, 0.5f, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f, t, prim, uv));
    CHECK_THAT(t, WithinAbs(1.0, 2e-3));
    REQUIRE(moving.intersectLocal(m, 0, 1.0f, {1.5f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f, t, prim, uv));
    CHECK_THAT(t, WithinAbs(1.5, 2e-3));

    // Deformation: the points grow to radius 2 over the shutter.
    Scene deforming;
    MeshDesc d = makeSphere(1.0f, 64, 32);
    d.pointSteps = 2;
    const std::vector<Vec3f> p0 = d.points;
    for (const Vec3f &p : p0)
        d.points.push_back(p * 2.0f);
    const MeshId g = deforming.createMesh();
    deforming.updateMesh(g, d);
    deforming.commit();
    REQUIRE(deforming.intersectLocal(g, 0, 0.5f, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 100.0f, t, prim, uv));
    CHECK_THAT(t, WithinAbs(1.5, 3e-3));
}

TEST_CASE("A subsurface sphere in a white furnace", "[sss][render]") {
    // Standard Surface with subsurface 1 (no specular, no base): a sphere of
    // radius 10 mean free paths in a dome of radiance 1 renders the lab's
    // 0.2334 / 0.5725 / 0.8993 (Karma f_s01: 0.2335 / 0.5730 / 0.9006). At
    // SSS limit 0 the walk still runs and its exit still gathers the dome
    // (Karma, research 4.6).
    for (const float limit : {1.0f, 0.0f, -1.0f}) {
        // -1: limit 1 on an analytic sphere (Embree's sphere points)
        INFO("ssslimit " << limit);
        Scene scene;
        const MeshId sphere = scene.createMesh();
        if (limit < 0.0f) {
            MeshDesc analytic;
            analytic.shape = MeshShape::Sphere;
            analytic.points = {{0.0f, 0.0f, 0.0f}};
            analytic.sphereRadius = {1.0f};
            scene.updateMesh(sphere, analytic);
        } else {
            scene.updateMesh(sphere, makeSphere(1.0f, 128, 64));
        }
        SurfaceInputs in = standardSurfaceDefaults();
        in.set(sf::baseWeight, 0.0f);
        in.set(sf::specularWeight, 0.0f);
        in.set(sf::subsurfaceWeight, 1.0f);
        in.set(sf::subsurfaceColor, {0.2f, 0.5f, 0.8f});
        in.set(sf::subsurfaceRadius, spectrum(1.0f));
        in.set(sf::subsurfaceScale, 0.1f);
        MaterialDesc desc;
        desc.inputs = in;
        const MaterialId mat = scene.createMaterial();
        scene.updateMaterial(mat, desc);
        scene.setMeshMaterials(sphere, {mat});
        LightDesc dome;
        dome.type = LightType::Dome;
        dome.cameraVisible = true;
        scene.updateLight(scene.createLight(), dome);
        scene.commit();

        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 128;
        settings.maxDepth = 64;
        settings.clampIndirect = 0.0f;
        settings.limits.limit[int(LimitClass::Sss)] = std::fabs(limit);
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        const int size = 32;
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size));
        renderer.resize(size, size);
        uint64_t walks0 = 0, capped0 = 0;
        subsurfaceWalkStats(walks0, capped0);
        renderToConvergence(renderer);
        uint64_t walks1 = 0, capped1 = 0;
        subsurfaceWalkStats(walks1, capped1);
        CHECK(walks1 > walks0);
        double sum[3] = {0.0, 0.0, 0.0}, sq[3] = {0.0, 0.0, 0.0};
        int n = 0;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const float px = (x + 0.5f) / size * 2.0f - 1.0f, py = (y + 0.5f) / size * 2.0f - 1.0f;
                if (px * px + py * py > 0.85f * 0.85f)
                    continue;
                const Spectrum v = pixelMean(renderer.film(), x, y);
                for (int c = 0; c < 3; ++c) {
                    sum[c] += v[c];
                    sq[c] += double(v[c]) * v[c];
                }
                ++n;
            }
        const double expected[3] = {0.2334, 0.5725, 0.8993};
        for (int c = 0; c < 3; ++c) {
            const double mean = sum[c] / n, error = std::sqrt(std::fmax(sq[c] / n - mean * mean, 0.0) / n);
            INFO("channel " << c << ": " << mean << " +- " << error);
            CHECK(std::fabs(mean - expected[c]) < 5.0 * error + 0.01 * expected[c]);
        }
    }
}
