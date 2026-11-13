// M8.3 volume scattering (docs/design/M8.md D8, D10-D12): free flights with
// spectral tracking, events shaded with the phase function, light sampling
// and continuations. A furnace in thermal equilibrium renders 1 everywhere
// (also chromatic, anisotropic and with absorption balanced by emission);
// single scattering in a homogeneous sphere matches the research's
// quadrature (A E5); HG sampling matches its density (chi-square); the
// Volume ray limit and its fractional values; volume events in light path
// expressions (V without a scattering class, O for emission, A).

#if defined(_MSC_VER)
#pragma warning(disable : 4702) // unreachable code in NanoVDB's grid builder templates
#endif

#include "test_helpers.h"

#include "core/film/lpe_compile.h"
#include "core/integrator/renderer.h"
#include "core/math/rng.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"
#include "core/volume/phase.h"

#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/GridBuilder.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;
using namespace nova::shading;
using Catch::Matchers::WithinAbs;

namespace {

// A cube of constant value: voxels [-n, n)^3 of size h (voxel centres at
// i h), so the field is `value` on [-(n-1) h, (n-1) h]^3.
VolumeGridPtr cubeGrid(float value, int n, double h) {
    nanovdb::build::Grid<float> g(0.0f, "field", nanovdb::GridClass::FogVolume);
    g.setTransform(h, nanovdb::Vec3d(0.0));
    // Voxel by voxel: the builder's fill of a box turns a leaf of one value into a tile, and of the eight
    // such tiles around the origin (n = 8) the grid then lacked the one at (-8, -8, -8): 3584 voxels of 4096.
    auto acc = g.getAccessor();
    for (int z = -n; z < n; ++z)
        for (int y = -n; y < n; ++y)
            for (int x = -n; x < n; ++x)
                acc.setValue(nanovdb::Coord(x, y, z), value);
    const nanovdb::GridHandle<nanovdb::HostBuffer> handle = nanovdb::createNanoGrid(g);
    return VolumeGrid::copyOf(handle.data(), handle.size());
}

VolumeId addVolume(Scene &scene, VolumeGridPtr grid, MaterialId material) {
    VolumeDesc desc;
    desc.fields.push_back({"density", "density", std::move(grid), Affine3f{}});
    desc.material = material;
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    return id;
}

uint16_t field(uint16_t f) { return uint16_t(volume_field::kBase + f); }

// scattering = density * s, absorption = density * a, emission = density *
// e, anisotropy g (a mask can multiply density first).
MaterialDesc medium(Vec3f s, Vec3f a, Vec3f e, float g, bool unitSphere = false) {
    IrGraph graph;
    IrBuilder b(graph);
    Ref d = b.primvar("density", 1, b.constant(0.0f));
    if (unitSphere) // inside the unit sphere only
        d = b.mul(d, b.binary(Op::Greater, b.constant(1.0f), b.length(b.global(Global::Pobject))));
    b.output(field(volume_field::scattering), 3, b.mul(d, b.constant(s.x, s.y, s.z)));
    b.output(field(volume_field::absorption), 3, b.mul(d, b.constant(a.x, a.y, a.z)));
    b.output(field(volume_field::emission), 3, b.mul(d, b.constant(e.x, e.y, e.z)));
    b.output(field(volume_field::anisotropy), 1, b.constant(g));
    MaterialDesc m;
    m.program = compile(graph).program;
    return m;
}

void addDome(Scene &scene, bool visible) {
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.color = spectrum(1.0f);
    dome.cameraVisible = visible;
    scene.updateLight(scene.createLight(), dome);
}

// A delta distant light of irradiance 1 arriving from direction `from`.
void addSun(Scene &scene, Vec3f from) {
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.normalize = true;
    sun.angleDeg = 0.0f;
    const Frame f = Frame::fromNormal(normalize(from)); // the light shines down its -Z
    const Vec3f axes[3] = {f.t, f.b, f.n};
    for (int r = 0; r < 3; ++r) {
        sun.transform.m[r][0] = axes[r].x;
        sun.transform.m[r][1] = axes[r].y;
        sun.transform.m[r][2] = axes[r].z;
    }
    scene.updateLight(scene.createLight(), sun);
}

struct Options {
    uint32_t spp = 64;
    uint32_t maxDepth = 256;
    float volumeLimit = 1e9f;
    std::vector<std::string> lpe;
    LightStrategy strategy = LightStrategy::MIS;
};

Film render(const Scene &scene, const Camera &camera, int size, const Options &o) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = o.spp;
    settings.maxDepth = o.maxDepth;
    settings.clampIndirect = 0.0f;
    settings.limits.limit[int(LimitClass::Volume)] = o.volumeLimit;
    settings.lightStrategy = o.strategy;
    if (!o.lpe.empty()) {
        auto compiled = std::make_shared<lpe::Compiled>(lpe::compile(o.lpe));
        for (size_t i = 0; i < o.lpe.size(); ++i) {
            INFO(o.lpe[i]);
            REQUIRE(compiled->errors[i].empty());
        }
        settings.lpe = compiled;
    }
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(size, size);
    renderToConvergence(renderer);
    return renderer.film();
}

// Pixel statistics of one channel: mean, the spread between pixels.
struct PixelStats {
    double mean = 0.0, sd = 0.0, maxDev = 0.0;
};
PixelStats stats(const Film &film, int channel, double expected) {
    std::vector<double> v;
    for (int y = 0; y < film.height; ++y)
        for (int x = 0; x < film.width; ++x) {
            const Spectrum m = pixelMean(film, x, y);
            v.push_back(channel == 0 ? m.x : channel == 1 ? m.y : m.z);
        }
    PixelStats s;
    for (double x : v)
        s.mean += x / double(v.size());
    for (double x : v) {
        s.sd += (x - s.mean) * (x - s.mean) / double(v.size() - 1);
        s.maxDev = std::max(s.maxDev, std::fabs(x - expected));
    }
    s.sd = std::sqrt(s.sd);
    return s;
}

} // namespace

TEST_CASE("Volume furnace: thermal equilibrium renders 1 everywhere", "[volume][scatter]") {
    // A cube of density 1.5 (optical depth about 3) inside a visible dome
    // of radiance 1, every path to its end. Whatever the medium, as long as
    // it emits its absorption coefficient, the radiance is 1 in every
    // channel and pixel.
    struct Case {
        const char *name;
        Vec3f s, a;
        float g;
    };
    const Case cases[] = {
        {"white, isotropic (no absorption, no emission)", {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 0.0f},
        {"grey with emission", {0.6f, 0.6f, 0.6f}, {0.4f, 0.4f, 0.4f}, 0.0f},
        {"chromatic, forward scattering", {1.0f, 0.6f, 0.2f}, {0.2f, 0.8f, 1.5f}, 0.6f},
        {"backward scattering", {0.9f, 1.4f, 0.5f}, {0.1f, 0.0f, 0.3f}, -0.5f},
    };
    const uint64_t violations = majorantViolations();
    for (const Case &c : cases) {
        INFO(c.name);
        Scene scene;
        const MaterialId mat = scene.createMaterial();
        scene.updateMaterial(mat, medium(c.s, c.a, c.a, c.g));
        addVolume(scene, cubeGrid(1.5f, 16, 1.0 / 16.0), mat);
        addDome(scene, true);
        scene.commit();
        Options o;
        o.spp = 256;
        const Film film = render(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.9f, 16, 16), 16, o);
        for (int ch = 0; ch < 3; ++ch) {
            const PixelStats s = stats(film, ch, 1.0);
            INFO("channel " << ch << ": mean " << s.mean << ", pixel sd " << s.sd << ", max |dev| " << s.maxDev);
            CHECK(s.sd < 0.06);
            CHECK(std::fabs(s.mean - 1.0) < 3.5 * s.sd / 16.0 + 1e-6);
            CHECK(s.maxDev < 4.5 * s.sd + 1e-6);
        }
    }
    // The interval majorants are conservative: no collision exceeded them.
    CHECK(majorantViolations() == violations);
}

TEST_CASE("Single scattering in a homogeneous sphere matches the quadrature", "[volume][scatter]") {
    // Research A E5: unit sphere, sigma_t 1.5, albedo 0.7, HG g = 0.3; a
    // distant light of irradiance 1 from normalize(1, 1, 0.3); rays along
    // +z at x = impact. Mirrored in z here (rays along -z, light from
    // (1, 1, -0.3)): the same angles and paths. Volume limit 0: the
    // continuation only gathers lights and cannot reach the delta light.
    Scene scene;
    const MaterialId mat = scene.createMaterial();
    scene.updateMaterial(mat, medium({1.05f, 1.05f, 1.05f}, {0.45f, 0.45f, 0.45f}, {0.0f, 0.0f, 0.0f}, 0.3f, true));
    addVolume(scene, cubeGrid(1.0f, 18, 1.0 / 16.0), mat);
    addSun(scene, {1.0f, 1.0f, -0.3f});
    scene.commit();
    const std::pair<float, double> refs[] = {{0.0f, 0.015118}, {0.5f, 0.026927}, {0.9f, 0.033980}};
    for (const auto &[impact, ref] : refs) {
        Options o;
        o.spp = 65536;
        o.volumeLimit = 0.0f;
        const Film film = render(scene, makeOrthoCamera({impact, 0.0f, 5.0f}, 1e-4f, 4, 4), 4, o);
        const PixelStats s = stats(film, 1, ref);
        const double se = s.sd / 4.0;
        INFO("impact " << impact << ": " << s.mean << " vs " << ref << " (se " << se << ", z " << (s.mean - ref) / se
                       << ")");
        CHECK(se < 0.0015 * ref);
        CHECK(std::fabs(s.mean - ref) < 3.5 * se);
        CHECK(std::fabs(s.mean - ref) < 0.002 * ref);
    }
}

TEST_CASE("Equiangular sampling of a point light in a homogeneous medium", "[volume][scatter][equiangular]") {
    // Research A E6 (docs/design/M9.md D17): a ray of length 10 through a
    // white isotropic medium, a point light of intensity 1 at distance h from
    // it at t = 4; single scattering (volume limit 0). The ray starts at the
    // camera inside the medium and ends on a black quad.
    const auto reference = [](double sigma, double h) {
        // In theta = atan((t - 4) / h) the integrand is smooth.
        const double a = std::atan(-4.0 / h), b = std::atan(6.0 / h);
        const int n = 200000;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double theta = a + (b - a) * (i + 0.5) / n;
            const double t = 4.0 + h * std::tan(theta), r = h / std::cos(theta);
            sum += std::exp(-sigma * (t + r));
        }
        return sum * (b - a) / n * sigma / (4.0 * kPi * h);
    };
    struct Case {
        double sigma, h, karmaRef; // the research's reference (E6)
    };
    for (const Case &cs : {Case{0.1, 0.05, 0.32762}, Case{0.1, 0.5, 0.028406}, Case{1.0, 0.05, 0.08114},
                           Case{1.0, 0.5, 0.00466}}) {
        const double ref = reference(cs.sigma, cs.h);
        INFO("sigma " << cs.sigma << ", h " << cs.h << ": reference " << ref);
        REQUIRE(std::fabs(ref - cs.karmaRef) < 2e-3 * cs.karmaRef);
        double sd[2] = {};
        for (const bool equiangular : {false, true}) {
            Scene scene;
            const MaterialId mat = scene.createMaterial();
            const float s = float(cs.sigma);
            scene.updateMaterial(mat, medium({s, s, s}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f));
            addVolume(scene, cubeGrid(1.0f, 25, 0.5), mat); // constant on [-12, 12]^3
            MeshDesc wall = makeQuad(20.0f);
            wall.setDisplayColor(spectrum(0.0f));
            wall.transform = translation({0.0f, 0.0f, -5.0f});
            scene.updateMesh(scene.createMesh(), wall);
            LightDesc point;
            point.type = LightType::Sphere;
            point.treatAsPoint = true;
            point.equiangular = equiangular;
            point.transform = translation({float(cs.h), 0.0f, 1.0f});
            scene.updateLight(scene.createLight(), point);
            scene.commit();
            Options o;
            o.spp = 65536;
            o.volumeLimit = 0.0f;
            const Film film = render(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1e-4f, 4, 4), 4, o);
            const PixelStats st = stats(film, 1, ref);
            const double se = st.sd / 4.0;
            INFO((equiangular ? "equiangular" : "free flights") << ": " << st.mean << " (se " << se << ", z "
                                                                  << (st.mean - ref) / se << ")");
            CHECK(std::fabs(st.mean - ref) < 3.5 * se + 1e-7); // unbiased either way
            CHECK(se < 0.02 * ref);
            sd[equiangular] = st.sd * std::sqrt(double(o.spp)); // per sample
        }
        // Less noise everywhere; the most in thin media near the light
        // (research E6; Nova tracks the transmittance to the point, which
        // limits the gain in denser media).
        INFO("per-sample deviation: free flights " << sd[0] << ", with equiangular " << sd[1]);
        CHECK(sd[1] * (cs.sigma < 0.5 && cs.h < 0.1 ? 5.0 : 1.2) < sd[0]);
    }
}

TEST_CASE("Henyey-Greenstein sampling matches its density", "[volume][phase]") {
    // Chi-square of sampled cos(theta) over 40 bins of equal probability
    // under the analytic CDF, for single lobes and Karma's two-lobe mix.
    const auto hgCdf = [](float g, double c) {
        if (std::fabs(g) < 1e-3f)
            return 0.5 * (c + 1.0);
        const double gg = double(g);
        return (1.0 - gg * gg) / (2.0 * gg) * (1.0 / std::sqrt(1.0 + gg * gg - 2.0 * gg * c) - 1.0 / (1.0 + gg));
    };
    const PhaseFunction phases[] = {{0.0f, 0.0f, 0.0f}, {0.3f, 0.0f, 0.0f}, {0.9f, 0.0f, 0.0f},
                                    {-0.7f, 0.0f, 0.0f}, {0.8f, -0.4f, 0.3f}};
    const Vec3f d = normalize(Vec3f{0.3f, -0.5f, 0.8f});
    for (const PhaseFunction &ph : phases) {
        INFO("g " << ph.g << ", g2 " << ph.g2 << ", mix " << ph.mix);
        const auto cdf = [&](double c) { return (1.0 - ph.mix) * hgCdf(ph.g, c) + ph.mix * hgCdf(ph.g2, c); };
        // Bin edges in cos(theta) by bisection on the CDF.
        constexpr int kBins = 40;
        std::vector<double> edges{-1.0};
        for (int i = 1; i < kBins; ++i) {
            double lo = -1.0, hi = 1.0;
            for (int it = 0; it < 60; ++it)
                (cdf(0.5 * (lo + hi)) < double(i) / kBins ? lo : hi) = 0.5 * (lo + hi);
            edges.push_back(0.5 * (lo + hi));
        }
        edges.push_back(1.0);
        std::vector<int> counts(kBins, 0);
        constexpr int kSamples = 200000;
        Rng rng = Rng::make(7, 0, 0);
        int pdfMismatch = 0;
        for (int i = 0; i < kSamples; ++i) {
            float pdf = 0.0f;
            const Vec3f u{rng.next(), rng.next(), rng.next()};
            const Vec3f wi = ph.sample(d, u, pdf);
            pdfMismatch += std::fabs(pdf - ph.eval(d, wi)) > 1e-4f * pdf;
            const double c = dot(d, wi);
            const int b = int(std::upper_bound(edges.begin(), edges.end(), c) - edges.begin()) - 1;
            ++counts[std::clamp(b, 0, kBins - 1)];
        }
        CHECK(pdfMismatch == 0);
        double chi2 = 0.0;
        const double expected = double(kSamples) / kBins;
        for (int n : counts)
            chi2 += (n - expected) * (n - expected) / expected;
        INFO("chi2 " << chi2);
        CHECK(chi2 < 80.0); // 39 degrees of freedom: p < 2e-4
        // The density integrates to 1 over the sphere.
        double integral = 0.0;
        for (int i = 0; i < kSamples; ++i) {
            const float z = 1.0f - 2.0f * rng.next(), phi = kTwoPi * rng.next();
            const float r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
            integral += 4.0 * kPi * ph.eval(d, {r * std::cos(phi), r * std::sin(phi), z}) / kSamples;
        }
        CHECK_THAT(integral, WithinAbs(1.0, 0.03));
    }
}

TEST_CASE("The Volume ray limit and its fractional values", "[volume][scatter]") {
    // A white cube under a delta sun, seen from the side: limit 0 is single
    // scattering, every further event adds light, a fractional limit mixes
    // the two neighbours.
    Scene scene;
    addVolume(scene, cubeGrid(2.0f, 16, 1.0 / 16.0), kDefaultMaterialId);
    addSun(scene, {0.0f, 1.0f, 0.0f});
    scene.commit();
    const auto mean = [&](float limit) {
        Options o;
        o.spp = 256;
        o.volumeLimit = limit;
        return stats(render(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.8f, 16, 16), 16, o), 1, 0.0).mean;
    };
    const double l0 = mean(0.0f), l1 = mean(1.0f), lHalf = mean(0.5f), l8 = mean(8.0f);
    INFO("limit 0: " << l0 << ", 0.5: " << lHalf << ", 1: " << l1 << ", 8: " << l8);
    CHECK(l0 > 0.01);
    CHECK(l1 > 1.2 * l0);
    CHECK(l8 > 1.1 * l1);
    CHECK_THAT(lHalf, WithinAbs(0.5 * (l0 + l1), 0.02 * l1));
}

TEST_CASE("Karma's multiple-scattering modifiers", "[volume][scatter]") {
    // kma_volume's extinction modifier e scales the extinction by
    // exp(-e (level - start + 1)) from the start level on, its contribution
    // modifier c the albedo (research H 4.7). Camera rays are level 0.
    const auto material = [](float e, float c, float start) {
        IrGraph graph;
        IrBuilder b(graph);
        const Ref d = b.primvar("density", 1, b.constant(0.0f));
        b.output(field(volume_field::scattering), 3, b.convert(d, 3));
        b.output(field(volume_field::extinctionModifier), 1, b.constant(e));
        b.output(field(volume_field::contributionModifier), 1, b.constant(c));
        b.output(field(volume_field::modifierStartLevel), 1, b.constant(start));
        MaterialDesc m;
        m.program = compile(graph).program;
        return m;
    };
    const auto render1 = [&](float e, float c, float start, bool sun) {
        Scene scene;
        const MaterialId mat = scene.createMaterial();
        scene.updateMaterial(mat, material(e, c, start));
        addVolume(scene, cubeGrid(1.0f, 16, 1.0 / 16.0), mat);
        if (sun)
            addSun(scene, {0.0f, 1.0f, 0.0f});
        scene.commit();
        CHECK(scene.anyLevelDependentVolume() == (e > 0.0f || c < 1.0f));
        Options o;
        o.spp = 256;
        o.volumeLimit = 0.0f;
        const Film film = render(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.5f, 8, 8), 8, o);
        double alpha = 0.0;
        for (size_t i = 0; i < film.pixelCount(); ++i)
            alpha += film.alphaSum[i] / film.count[i] / double(film.pixelCount());
        return std::pair{alpha, stats(film, 1, 0.0).mean};
    };
    // The optical depth through the cube's middle: 32 voxels of 1/16 (the
    // field's trilinear edges included).
    const double tau = 2.0;
    CHECK_THAT(render1(0.0f, 1.0f, 0.0f, false).first, WithinAbs(1.0 - std::exp(-tau), 0.01));
    CHECK_THAT(render1(0.5f, 1.0f, 0.0f, false).first, WithinAbs(1.0 - std::exp(-tau * std::exp(-0.5)), 0.01));
    CHECK_THAT(render1(0.5f, 1.0f, 1.0f, false).first, WithinAbs(1.0 - std::exp(-tau), 0.01));
    // Single scattering (limit 0) at level 0: the albedo scales from start 0.
    const double plain = render1(0.0f, 1.0f, 0.0f, true).second;
    CHECK_THAT(render1(0.0f, 0.5f, 0.0f, true).second, WithinAbs(0.5 * plain, 0.02 * plain));
    CHECK_THAT(render1(0.0f, 0.5f, 1.0f, true).second, WithinAbs(plain, 0.02 * plain));
}

TEST_CASE("Overlapping volumes are one medium", "[volume][scatter]") {
    // Two volumes on the same cube, one isotropic and one scattering
    // forward (g = 0.8), each emitting a little, against one volume with
    // both coefficients summed and Karma's two-lobe mix of the two phase
    // functions with equal weights: the same medium, so the same image
    // within noise. A third, shifted volume overlaps both partially.
    const auto material = [](float g, float g2, float mix, float scale) {
        IrGraph graph;
        IrBuilder b(graph);
        const Ref d = b.mul(b.primvar("density", 1, b.constant(0.0f)), b.constant(scale));
        b.output(field(volume_field::scattering), 3, b.mul(d, b.constant(0.8f, 0.7f, 0.6f)));
        b.output(field(volume_field::absorption), 3, b.mul(d, b.constant(0.1f, 0.2f, 0.3f)));
        b.output(field(volume_field::emission), 3, b.mul(d, b.constant(0.05f, 0.02f, 0.01f)));
        b.output(field(volume_field::anisotropy), 1, b.constant(g));
        b.output(field(volume_field::secondaryAnisotropy), 1, b.constant(g2));
        b.output(field(volume_field::secondaryMix), 1, b.constant(mix));
        MaterialDesc m;
        m.program = compile(graph).program;
        return m;
    };
    const auto build = [&](Scene &scene, bool merged) {
        const MaterialId iso = scene.createMaterial(), fwd = scene.createMaterial(), mix = scene.createMaterial();
        scene.updateMaterial(iso, material(0.0f, 0.0f, 0.0f, 1.0f));
        scene.updateMaterial(fwd, material(0.8f, 0.0f, 0.0f, 1.0f));
        scene.updateMaterial(mix, material(0.0f, 0.8f, 0.5f, 2.0f));
        if (merged) {
            addVolume(scene, cubeGrid(1.0f, 16, 1.0 / 16.0), mix);
        } else {
            addVolume(scene, cubeGrid(1.0f, 16, 1.0 / 16.0), iso);
            addVolume(scene, cubeGrid(1.0f, 16, 1.0 / 16.0), fwd);
        }
        // The shifted one: the same in both scenes.
        VolumeDesc shifted;
        shifted.fields.push_back({"density", "density", cubeGrid(0.7f, 8, 1.0 / 16.0), Affine3f{}});
        shifted.material = iso;
        const VolumeId id = scene.createVolume();
        scene.updateVolume(id, shifted);
        scene.updateVolumeMotion(id, {translation({0.4f, 0.3f, 0.2f})});
        addSun(scene, {0.4f, 1.0f, 0.3f});
        scene.commit();
    };
    Scene apart, merged;
    build(apart, false);
    build(merged, true);
    Options o;
    o.spp = 512;
    o.volumeLimit = 4.0f;
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.2f, 12, 12);
    const Film a = render(apart, camera, 12, o), b = render(merged, camera, 12, o);
    // 3 x 3 blocks of 4 x 4 pixels.
    double ba[9] = {}, bb[9] = {};
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 12; ++x) {
            const Spectrum pa = pixelMean(a, x, y), pb = pixelMean(b, x, y);
            ba[(y / 4) * 3 + x / 4] += pa.x + pa.y + pa.z;
            bb[(y / 4) * 3 + x / 4] += pb.x + pb.y + pb.z;
        }
    for (int i = 0; i < 9; ++i) {
        INFO("block " << i << ": " << ba[i] << " vs " << bb[i]);
        CHECK_THAT(ba[i], WithinAbs(bb[i], 0.03 * bb[i]));
    }
}

TEST_CASE("Emissive volumes as light sources", "[volume][light]") {
    // An absorbing, emitting cube above a grey ground, seen from above (the
    // ground around it is lit by the cube only). As a light source the
    // cube's emission is light-sampled and combined with the continuations'
    // collisions by MIS (research A E8): the same image as collecting it
    // with continuations only, with less noise; light sampling alone agrees
    // as well. A white cube next to it is lit by the light at volume limit
    // 0 (Karma, research H 4.7), not without.
    const auto build = [](Scene &scene, bool light) {
        const MaterialId ground = scene.createMaterial();
        MaterialDesc grey;
        grey.inputs = lambertDefaults(spectrum(0.5f));
        scene.updateMaterial(ground, grey);
        const MeshId quad = scene.createMesh();
        scene.updateMesh(quad, makeQuad(3.0f));
        scene.setMeshMaterials(quad, {ground});
        const MaterialId fire = scene.createMaterial();
        scene.updateMaterial(fire, medium({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {2.0f, 1.0f, 0.5f}, 0.0f));
        VolumeDesc desc;
        desc.fields.push_back({"density", "density", cubeGrid(1.0f, 8, 1.0 / 16.0), Affine3f{}});
        desc.material = fire;
        desc.light = light;
        const VolumeId id = scene.createVolume();
        scene.updateVolume(id, desc);
        scene.updateVolumeMotion(id, {translation({-0.8f, 0.0f, 0.8f})});
        VolumeDesc smoke;
        smoke.fields.push_back({"density", "density", cubeGrid(2.0f, 8, 1.0 / 16.0), Affine3f{}});
        const VolumeId sid = scene.createVolume();
        scene.updateVolume(sid, smoke);
        scene.updateVolumeMotion(sid, {translation({0.9f, 0.0f, 0.8f})});
        scene.commit();
    };
    Scene plain, lit;
    build(plain, false);
    build(lit, true);
    REQUIRE(lit.lights().size() == 1);
    CHECK(lit.lights()[0].type == LightType::Volume);
    CHECK(plain.lights().empty());

    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, 16, 16);
    Options o;
    o.spp = 256;
    o.maxDepth = 3;
    o.volumeLimit = 0.0f;
    const Film a = render(plain, camera, 16, o), b = render(lit, camera, 16, o);
    o.strategy = LightStrategy::LightOnly;
    const Film c = render(lit, camera, 16, o);
    // The ground left of the cubes (x < -1.4 and the bottom and top rows),
    // which only the fire lights.
    const auto groundMean = [](const Film &f, double &sd) {
        std::vector<double> v;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                if (x < 2 || y < 3 || y > 12)
                    v.push_back(pixelMean(f, x, y).x);
        double m = 0.0;
        for (double x : v)
            m += x / double(v.size());
        sd = 0.0;
        for (double x : v)
            sd += (x - m) * (x - m) / double(v.size() - 1);
        sd = std::sqrt(sd / double(v.size()));
        return m;
    };
    double sa = 0.0, sb = 0.0, sc = 0.0;
    const double ma = groundMean(a, sa), mb = groundMean(b, sb), mc = groundMean(c, sc);
    INFO("ground: collisions only " << ma << ", MIS " << mb << ", light samples only " << mc);
    CHECK(ma > 0.01);
    CHECK_THAT(mb, WithinAbs(ma, 0.03 * ma));
    CHECK_THAT(mc, WithinAbs(mb, 0.03 * mb));
    // The white cube (the 4 x 4 pixels over x in [0.5, 1.5], y in [-0.5,
    // 0.5]) at volume limit 0, at 1024 samples (each estimate's standard
    // error there is about 1.5 %).
    const auto smokeMean = [](const Film &f) {
        double m = 0.0;
        for (int y = 6; y < 10; ++y)
            for (int x = 10; x < 14; ++x)
                m += pixelMean(f, x, y).x / 16.0;
        return m;
    };
    o.spp = 1024;
    o.strategy = LightStrategy::MIS;
    const Film bs = render(lit, camera, 16, o);
    o.strategy = LightStrategy::LightOnly;
    const Film cs = render(lit, camera, 16, o);
    o.strategy = LightStrategy::BsdfOnly;
    const Film ds = render(lit, camera, 16, o);
    INFO("smoke: " << smokeMean(a) << " without, " << smokeMean(bs) << " with the light (MIS), " << smokeMean(cs)
                   << " light samples only, " << smokeMean(ds) << " phase samples only");
    CHECK(smokeMean(bs) > 1.5 * smokeMean(a));
    CHECK_THAT(smokeMean(cs), WithinAbs(smokeMean(bs), 0.05 * smokeMean(bs)));
    CHECK_THAT(smokeMean(ds), WithinAbs(smokeMean(bs), 0.05 * smokeMean(bs)));
}

TEST_CASE("A volume light that changes size over the shutter is light-sampled without bias", "[volume][light][motion]") {
    // The emitting cube above the grey ground, growing to twice its size (8
    // times its volume) while it moves over the shutter. Its light samples
    // divide by the cell's world volume at the path's time (R1-36: such a
    // volume was no light source before, since one constant cell volume
    // would bias them: with the shutter-open volume the ground would be lit
    // by the mean of 1 / s^3, 0.375 of the light). Collecting the emission
    // with continuations only (not a light), MIS and light samples alone
    // give the same ground.
    const auto build = [](Scene &scene, bool light, bool grows) {
        const MaterialId ground = scene.createMaterial();
        MaterialDesc grey;
        grey.inputs = lambertDefaults(spectrum(0.5f));
        scene.updateMaterial(ground, grey);
        const MeshId quad = scene.createMesh();
        scene.updateMesh(quad, makeQuad(3.0f));
        scene.setMeshMaterials(quad, {ground});
        const MaterialId fire = scene.createMaterial();
        scene.updateMaterial(fire, medium({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {2.0f, 1.0f, 0.5f}, 0.0f));
        VolumeDesc desc;
        desc.fields.push_back({"density", "density", cubeGrid(1.0f, 8, 1.0 / 16.0), Affine3f{}});
        desc.material = fire;
        desc.light = light;
        const VolumeId id = scene.createVolume();
        scene.updateVolume(id, desc);
        Mat4 open = translation({-0.3f, 0.0f, 0.8f}), close = translation({0.3f, 0.0f, 1.0f});
        if (grows) // else moved alone
            close.m[0][0] = close.m[1][1] = close.m[2][2] = 2.0;
        scene.updateVolumeMotion(id, {open, close});
        scene.commit();
    };
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.5f, 16, 16);
    // The ground the cube never covers (the outer two pixel rings), which
    // only the fire lights.
    const auto groundMean = [](const Film &f) {
        double m = 0.0;
        int n = 0;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                if (x < 2 || x > 13 || y < 2 || y > 13) {
                    m += pixelMean(f, x, y).x;
                    ++n;
                }
        return m / double(n);
    };
    for (const bool grows : {true, false}) {
        INFO("grows " << grows);
        Scene plain, lit;
        build(plain, false, grows);
        build(lit, true, grows);
        CHECK(plain.lights().empty());
        REQUIRE(lit.lights().size() == 1);
        CHECK(lit.lights()[0].type == LightType::Volume);
        Options o;
        o.spp = 1024;
        o.maxDepth = 3;
        o.volumeLimit = 0.0f;
        const double collisions = groundMean(render(plain, camera, 16, o));
        const double mis = groundMean(render(lit, camera, 16, o));
        o.strategy = LightStrategy::LightOnly;
        const double lightOnly = groundMean(render(lit, camera, 16, o));
        o.strategy = LightStrategy::BsdfOnly;
        const double bsdfOnly = groundMean(render(lit, camera, 16, o));
        INFO("ground: collisions only " << collisions << ", MIS " << mis << ", light samples only " << lightOnly
                                        << ", continuations only " << bsdfOnly);
        CAPTURE(collisions, mis, lightOnly, bsdfOnly);
        CHECK(collisions > 0.005);
        CHECK_THAT(mis, WithinAbs(collisions, 0.03 * collisions));
        CHECK_THAT(lightOnly, WithinAbs(collisions, 0.03 * collisions));
        CHECK_THAT(bsdfOnly, WithinAbs(collisions, 0.03 * collisions));
    }
}

TEST_CASE("Volumes inside glass", "[volume][scatter]") {
    // The white furnace cube inside a smooth glass sphere of IOR 1 (no
    // reflection, no bending): paths cross the glass as delta events, scatter
    // in the volume inside and reach the dome through the glass; every pixel
    // stays 1.
    Scene scene;
    addVolume(scene, cubeGrid(1.5f, 16, 1.0 / 16.0), kDefaultMaterialId);
    SurfaceInputs glass = standardSurfaceDefaults();
    glass.set(surface_field::transmissionWeight, 1.0f);
    glass.set(surface_field::specularRoughness, 0.0f);
    glass.set(surface_field::specularIor, 1.0f);
    MaterialDesc glassMat;
    glassMat.inputs = glass;
    const MaterialId gm = scene.createMaterial();
    scene.updateMaterial(gm, glassMat);
    const MeshId sphere = scene.createMesh();
    scene.updateMesh(sphere, makeSphere(1.8f, 64, 32));
    scene.setMeshMaterials(sphere, {gm});
    addDome(scene, true);
    scene.commit();
    Options o;
    o.spp = 256;
    const Film film = render(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.9f, 16, 16), 16, o);
    const PixelStats s = stats(film, 1, 1.0);
    INFO("mean " << s.mean << ", pixel sd " << s.sd << ", max |dev| " << s.maxDev);
    CHECK(std::fabs(s.mean - 1.0) < 3.5 * s.sd / 16.0 + 1e-3);
    CHECK(s.maxDev < 4.5 * s.sd + 1e-3);
}

TEST_CASE("Volumes blur with their transform and their velocity", "[volume][motion]") {
    // A cube of density 2 (alpha A = 1 - e^-2 through it) moving +1 in x
    // over the shutter: the alpha at x is A times the share of the shutter
    // the cube covers x (D17). The same cube at rest with a velocity field
    // of 1 along x and lookup offsets over [0, 1] s is the same image.
    const double A = 1.0 - std::exp(-2.0);
    Scene moving, flowing;
    {
        const VolumeId id = addVolume(moving, cubeGrid(2.0f, 8, 1.0 / 16.0), kDefaultMaterialId);
        moving.updateVolumeMotion(id, {translation({0.0f, 0.0f, 0.0f}), translation({1.0f, 0.0f, 0.0f})});
        moving.commit();
    }
    {
        VolumeDesc desc;
        desc.fields.push_back({"density", "density", cubeGrid(2.0f, 8, 1.0 / 16.0), Affine3f{}});
        desc.fields.push_back({"vel.x", "vel.x", cubeGrid(1.0f, 40, 1.0 / 16.0), Affine3f{}});
        desc.fields.push_back({"vel.y", "vel.y", cubeGrid(0.0f, 40, 1.0 / 16.0), Affine3f{}});
        desc.fields.push_back({"vel.z", "vel.z", cubeGrid(0.0f, 40, 1.0 / 16.0), Affine3f{}});
        desc.velocity = "vel";
        desc.velocityTime[0] = 0.0f;
        desc.velocityTime[1] = 1.0f;
        const VolumeId id = flowing.createVolume();
        flowing.updateVolume(id, desc);
        flowing.commit();
        CHECK(flowing.volumeRecord(id).blurReach > 0.99f);
        // The majorants cover the moved cube: its bounds reach past x = 1.4.
        CHECK(flowing.volumeRecord(id).upper.x > 1.4f);
    }
    const auto alphaAt = [](const Scene &scene, float x) {
        Options o;
        o.spp = 1024;
        const Film film = render(scene, makeOrthoCamera({x, 0.0f, 5.0f}, 1e-4f, 4, 4), 4, o);
        double a = 0.0;
        for (size_t i = 0; i < film.pixelCount(); ++i)
            a += film.alphaSum[i] / film.count[i] / double(film.pixelCount());
        return a;
    };
    // The field's trilinear edges centre the cube on x = -1/32.
    const float c = -1.0f / 32.0f;
    for (const float x : {0.25f, 0.6f, 1.25f, -0.2f}) {
        const double cover = std::clamp(double(x - c) + 0.5, 0.0, 1.0) - std::clamp(double(x - c) - 0.5, 0.0, 1.0);
        const double a = alphaAt(moving, x), b = alphaAt(flowing, x);
        INFO("x " << x << ": transform " << a << ", velocity " << b << ", expected " << A * cover);
        CHECK_THAT(a, WithinAbs(A * cover, 0.02));
        CHECK_THAT(b, WithinAbs(a, 0.02));
    }
}

TEST_CASE("Volume events in light path expressions", "[volume][lpe]") {
    // A scattering, emitting cube with a delta sun and a visible dome:
    // light and emission split by their last event partition the beauty;
    // volume events have no scattering class (Karma); A after V gives the
    // event's weight, the volumes' opacity for a white medium.
    Scene scene;
    const MaterialId mat = scene.createMaterial();
    scene.updateMaterial(mat, medium({1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {0.3f, 0.2f, 0.1f}, 0.2f));
    addVolume(scene, cubeGrid(1.0f, 16, 1.0 / 16.0), mat);
    addSun(scene, {0.3f, 1.0f, 0.2f});
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.color = {0.2f, 0.3f, 0.4f};
    dome.cameraVisible = false;
    scene.updateLight(scene.createLight(), dome);
    scene.commit();
    Options o;
    o.spp = 64;
    o.volumeLimit = 4.0f;
    o.lpe = {"C.*", "CV+L", "CO", "CV+O", "C<V.>L", "CVL", "CDL", "C<VD>L", "CVA"};
    const Film film = render(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.9f, 12, 12), 12, o);
    int partition = 0, single = 0, classes = 0;
    double albedo = 0.0, alpha = 0.0;
    for (size_t i = 0; i < film.pixelCount(); ++i) {
        const Spectrum beauty = film.lpe(0)[i];
        const Spectrum sum = film.lpe(1)[i] + film.lpe(2)[i] + film.lpe(3)[i];
        partition += std::fabs(sum.x - beauty.x) > 1e-4f * std::fmax(1.0f, beauty.x);
        single += film.lpe(4)[i].y != film.lpe(5)[i].y;
        classes += film.lpe(6)[i].y != 0.0f || film.lpe(7)[i].y != 0.0f;
        albedo += film.lpe(8)[i].y / film.count[i];
        alpha += film.alphaSum[i] / film.count[i];
    }
    CHECK(partition == 0);
    CHECK(single == 0);
    CHECK(classes == 0);
    INFO("CVA " << albedo / film.pixelCount() << ", alpha " << alpha / film.pixelCount());
    CHECK_THAT(albedo / film.pixelCount(), WithinAbs(alpha / film.pixelCount(), 0.01));
    // The beauty itself is the film's colour.
    double beauty = 0.0, color = 0.0;
    for (size_t i = 0; i < film.pixelCount(); ++i) {
        beauty += film.lpe(0)[i].x;
        color += film.colorSum[i].x;
    }
    CHECK_THAT(beauty, WithinAbs(color, 1e-3 * color));
}
