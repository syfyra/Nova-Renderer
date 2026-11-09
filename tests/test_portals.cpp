// Light portals (Karma's isportal, portaldomelights, portalmisbias;
// docs/design/M11-P9b.md): the dome lists, portal sampling against its
// density, and a room lit by a dome through a window. The Karma comparisons
// are in tests/husk/check_karma_p9b.py; the XPU case in test_gpu_render.cpp.

#include "test_helpers.h"

#include "core/lights/lights.h"
#include "core/math/sampling.h"
#include "core/math/sobol.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// The window of the room below: x = 2, y -0.75..0.75, z 1..2.
std::vector<Vec3f> windowTriangles() {
    return {{2.0f, -0.75f, 1.0f}, {2.0f, 0.75f, 1.0f}, {2.0f, 0.75f, 2.0f},
            {2.0f, -0.75f, 1.0f}, {2.0f, 0.75f, 2.0f}, {2.0f, -0.75f, 2.0f}};
}

std::vector<PortalTriangle> portalTriangles(const std::vector<Vec3f> &v) {
    std::vector<PortalTriangle> out;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        PortalTriangle t;
        t.v0 = v[i];
        t.e1 = v[i + 1] - v[i];
        t.e2 = v[i + 2] - v[i];
        const Vec3f n = cross(t.e1, t.e2);
        t.n = normalize(n);
        t.area = 0.5f * length(n);
        out.push_back(t);
    }
    return out;
}

MeshDesc wallQuad(Vec3f o, Vec3f e1, Vec3f e2) {
    MeshDesc m;
    m.points = {o, o + e1, o + e1 + e2, o + e2};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

// A closed 4 x 4 x 3 room (z up) of grey Lambert walls with a window in
// its +x wall (y -0.75..0.75, z 1..2), and with `second` one in its -x
// wall, lit by a white constant dome (path /World/dome).
void room(Scene &scene, bool second) {
    MaterialDesc grey;
    grey.inputs = lambertDefaults(spectrum(0.5f));
    const MaterialId g = scene.createMaterial();
    scene.updateMaterial(g, grey);
    std::vector<MeshDesc> walls = {
        wallQuad({-2, -2, 0}, {4, 0, 0}, {0, 4, 0}),  // floor
        wallQuad({-2, -2, 3}, {4, 0, 0}, {0, 4, 0}),  // ceiling
        wallQuad({-2, -2, 0}, {4, 0, 0}, {0, 0, 3}),  // -y
        wallQuad({-2, 2, 0}, {4, 0, 0}, {0, 0, 3}),   // +y
    };
    for (float x : {2.0f, -2.0f}) {
        if (x > 0.0f || second) { // around the window
            walls.push_back(wallQuad({x, -2, 0}, {0, 4, 0}, {0, 0, 1}));
            walls.push_back(wallQuad({x, -2, 2}, {0, 4, 0}, {0, 0, 1}));
            walls.push_back(wallQuad({x, -2, 1}, {0, 1.25f, 0}, {0, 0, 1}));
            walls.push_back(wallQuad({x, 0.75f, 1}, {0, 1.25f, 0}, {0, 0, 1}));
        } else {
            walls.push_back(wallQuad({x, -2, 0}, {0, 4, 0}, {0, 0, 3}));
        }
    }
    for (const MeshDesc &w : walls) {
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, w);
        scene.setMeshMaterials(id, {g});
    }
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.path = "/World/dome";
    scene.updateLight(scene.createLight(), dome);
}

Film renderRoom(const Scene &scene, uint32_t spp, RenderDevice device = RenderDevice::Cpu) {
    Renderer renderer;
    RenderSettings s;
    s.device = device;
    s.maxSamples = spp;
    s.maxDepth = 2;
    s.clampIndirect = 0.0f;
    s.limits.limit[int(LimitClass::Diffuse)] = 1.0f;
    renderer.setScene(&scene);
    renderer.setSettings(s);
    renderer.setCamera(makePerspectiveCamera({0.0f, 0.0f, 2.9f}, 12.0, 20.955, 32, 32));
    renderer.resize(32, 32);
    renderToConvergence(renderer);
    return renderer.film();
}

double meanOf(const Film &film) {
    double sum = 0.0;
    for (size_t i = 0; i < film.colorSum.size(); ++i)
        sum += luminance(film.colorSum[i]) / float(film.count[i]);
    return sum / double(film.colorSum.size());
}

double rmsDifference(const Film &a, const Film &b) {
    double sum = 0.0;
    for (size_t i = 0; i < a.colorSum.size(); ++i) {
        const double d = luminance(a.colorSum[i]) / float(a.count[i]) - luminance(b.colorSum[i]) / float(b.count[i]);
        sum += d * d;
    }
    return std::sqrt(sum / double(a.colorSum.size()));
}

} // namespace

// Karma's list (measured): space-separated paths, * for every dome; a bare
// name is no path.
TEST_CASE("Portal dome lists", "[portal]") {
    CHECK(portalServes("*", "/World/dome"));
    CHECK(portalServes("/World/dome", "/World/dome"));
    CHECK(portalServes("/World/domeB /World/dome", "/World/dome"));
    CHECK(portalServes("  /World/dom?  ", "/World/dome"));
    CHECK(portalServes("/World/*", "/World/dome"));
    CHECK(!portalServes("dome", "/World/dome"));
    CHECK(!portalServes("", "/World/dome"));
    CHECK(!portalServes("/World/domeB", "/World/dome"));
}

// Portal sampling (mixed with the dome's distribution) returns the density
// domeSamplePdf gives the same direction, and estimates the light through
// the window: L times its solid angle.
TEST_CASE("Portal sampling against its density", "[portal][lights]") {
    LightDesc desc;
    desc.type = LightType::Dome;
    desc.intensity = 2.0f;
    LightData light = makeLightData(desc);
    const std::vector<PortalTriangle> tris = portalTriangles(windowTriangles());
    light.portals = tris.data();
    light.portalCount = uint32_t(tris.size());
    light.portalArea = tris[0].area + tris[1].area;
    const Vec3f P{0.3f, 0.2f, 0.4f};
    double sum = 0.0;
    const uint32_t n = 1u << 16;
    for (uint32_t i = 0; i < n; ++i) {
        const Vec2f u{unitFloat(owenScramble(sobolDim0(i), 0x9e37u)), unitFloat(owenScramble(sobolDim1(i), 0x51edu))};
        const LightSample s = sampleLight(light, P, u);
        if (!(s.pdf > 0.0f))
            continue;
        bool seen = false;
        const float pdf = domeSamplePdf(light, P, s.wi, seen);
        REQUIRE(seen);
        REQUIRE_THAT(pdf, WithinRel(s.pdf, 1e-4f));
        sum += s.Li.x / s.pdf;
    }
    const double omega = portalSolidAngle(tris.data(), uint32_t(tris.size()), P);
    CHECK_THAT(sum / n, WithinRel(2.0 * omega, 0.01));
    // Outside the window: nothing, and no density.
    bool seen = true;
    CHECK(domeSamplePdf(light, P, normalize(Vec3f{-1.0f, 0.0f, 0.2f}), seen) == 0.0f);
    CHECK(!seen);

    // A textured dome: portal and map samples mixed (portalMix), against
    // the light through the window integrated over its area.
    Image sky;
    sky.width = 64;
    sky.height = 32;
    for (int y = 0; y < sky.height; ++y)
        for (int x = 0; x < sky.width; ++x)
            sky.pixels.push_back(spectrum(0.2f + float(x) / 64.0f + (x > 40 && x < 48 && y > 8 && y < 14 ? 30.0f : 0.0f)));
    Distribution2D dist;
    REQUIRE(buildDomeDistribution(sky, dist));
    light.texels = sky.pixels.data();
    light.texWidth = uint32_t(sky.width);
    light.texHeight = uint32_t(sky.height);
    light.distribution = dist.view();
    // (Without portal samples the map leaves its below-average part to BSDF
    // samples: not a complete estimate on its own.)
    for (const float mix : {0.25f, 0.5f, 1.0f}) {
        light.portalMix = mix;
        double estimate = 0.0, reference = 0.0;
        for (uint32_t i = 0; i < n; ++i) {
            const Vec2f u{unitFloat(owenScramble(sobolDim0(i), 0x1234u)), unitFloat(owenScramble(sobolDim1(i), 0x5678u))};
            const LightSample s = sampleLight(light, P, u);
            if (s.pdf > 0.0f) {
                bool through = false;
                REQUIRE_THAT(domeSamplePdf(light, P, s.wi, through), WithinRel(s.pdf, 1e-4f));
                REQUIRE(through);
                estimate += s.Li.x / s.pdf;
            }
            // The reference: uniform points on the window.
            const PortalTriangle &t = tris[i & 1u];
            float b1 = u.x, b2 = u.y;
            if (b1 + b2 > 1.0f) {
                b1 = 1.0f - b1;
                b2 = 1.0f - b2;
            }
            const Vec3f d = t.v0 + t.e1 * b1 + t.e2 * b2 - P;
            const float r2 = dot(d, d);
            const Vec3f w = d * (1.0f / std::sqrt(r2));
            reference += domeRadiance(light, w).x * std::fabs(dot(t.n, w)) / r2 * (2.0f * t.area);
        }
        INFO("portal mix " << mix);
        CHECK_THAT(estimate / n, WithinRel(reference / n, 0.02));
    }
}

// A room lit by a dome through a window: a portal in the window keeps the
// light and takes most of its noise away; light through openings without a
// portal no longer arrives (Karma, measured: the dome lights through its
// portals only).
TEST_CASE("Light portals in a room", "[render][portal]") {
    Scene plain;
    room(plain, false);
    plain.commit();
    Scene portal;
    room(portal, false);
    portal.setPortal("/World/window", PortalDesc{windowTriangles(), "*"});
    portal.commit();
    REQUIRE(portal.lights()[0].portalCount == 2);
    const Film reference = renderRoom(portal, 512);
    const double mean = meanOf(reference);
    CHECK_THAT(meanOf(renderRoom(plain, 512)), WithinRel(mean, 0.03));
    const double noisePlain = rmsDifference(renderRoom(plain, 8), reference);
    const double noisePortal = rmsDifference(renderRoom(portal, 8), reference);
    INFO("noise at 8 samples: " << noisePlain << " without, " << noisePortal << " with the portal");
    CHECK(noisePortal < 0.4 * noisePlain);

    // A second window without a portal adds nothing.
    Scene second;
    room(second, true);
    second.setPortal("/World/window", PortalDesc{windowTriangles(), "/World/dome"});
    second.commit();
    CHECK_THAT(meanOf(renderRoom(second, 256)), WithinRel(mean, 0.03));
    Scene open;
    room(open, true);
    open.commit();
    CHECK(meanOf(renderRoom(open, 256)) > 1.8 * mean);
    // A portal for another dome changes nothing for this one.
    Scene other;
    room(other, true);
    other.setPortal("/World/window", PortalDesc{windowTriangles(), "/World/sky"});
    other.commit();
    CHECK(other.lights()[0].portalCount == 0);
}
