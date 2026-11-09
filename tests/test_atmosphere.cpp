// The Karma Sky Atmosphere (docs/design/M11-P7.md): the planetary medium's
// optical depth against a fine quadrature, the in-scattering proposal, the
// Rayleigh phase function, and renders whose result is exact: absorbing
// atmospheres (the sun's transmittance on a surface, alpha, the ground's
// light, the planet's shadow) and single scattering against a quadrature of
// the in-scattering integral.

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/math/frame.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"
#include "core/volume/atmosphere.h"
#include "core/volume/phase.h"
#include "core/volume/shell.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <random>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPiD = 3.14159265358979323846;
constexpr double kR = 6371000.0;   // the planet centre's distance below the origin (+Z up)
constexpr double kSea = 6370900.0; // the origin is 100 m above sea level

// A scene with Karma's default atmosphere, +Z up, the planet below the origin.
std::shared_ptr<AtmosphereDesc> defaultDesc() {
    auto d = std::make_shared<AtmosphereDesc>();
    d->center[0] = 0.0;
    d->center[1] = 0.0;
    d->center[2] = -kR;
    d->seaLevel = kSea;
    return d;
}

VolumeId addAtmosphere(Scene &scene, std::shared_ptr<AtmosphereDesc> desc, float volumeLimit = 0.0f) {
    VolumeDesc v;
    v.name = "/atmosphere";
    v.atmosphere = std::move(desc);
    v.volumeLimit = volumeLimit;
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, v);
    return id;
}

// A sun (irradiance 1, a delta distant light) at altitude `alt` degrees,
// in the +Y direction's half (azimuth 0 = towards +Y).
void addSun(Scene &scene, float altDeg, float angleDeg = 0.0f) {
    const float a = altDeg * kPi / 180.0f;
    const Vec3f to{0.0f, std::cos(a), std::sin(a)};
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.normalize = true;
    sun.angleDeg = angleDeg;
    const Frame f = Frame::fromNormal(to); // the light shines down its -Z
    const Vec3f axes[3] = {f.t, f.b, f.n};
    for (int r = 0; r < 3; ++r) {
        sun.transform.m[r][0] = axes[r].x;
        sun.transform.m[r][1] = axes[r].y;
        sun.transform.m[r][2] = axes[r].z;
    }
    scene.updateLight(scene.createLight(), sun);
}

Vec3f sunDir(float altDeg) {
    const float a = altDeg * kPi / 180.0f;
    return {0.0f, std::cos(a), std::sin(a)};
}

// The model in double, for references.
struct Model {
    AtmosphereDesc d;
    double top;
    explicit Model(const AtmosphereDesc &desc) : d(desc), top(desc.topAltitude()) {}
    double altitude(const double p[3]) const {
        const double x = p[0] - d.center[0], y = p[1] - d.center[1], z = p[2] - d.center[2];
        return std::sqrt(x * x + y * y + z * z) - d.seaLevel;
    }
    void coefficients(double h, double ext[3], double ray[3], double mie[3]) const {
        for (int c = 0; c < 3; ++c)
            ext[c] = ray[c] = mie[c] = 0.0;
        if (!(h >= 0.0) || h > top)
            return;
        const double r = std::exp(-h / d.rayleighHeight), m = std::exp(-h / d.mieHeight);
        const double o = std::max(0.0, 1.0 - std::fabs(h - d.ozoneAltitude) / (0.5 * d.ozoneWidth));
        for (int c = 0; c < 3; ++c) {
            ray[c] = d.rayleigh[c] * r;
            mie[c] = d.mieScatter[c] * m;
            ext[c] = ray[c] + (d.mieScatter[c] + d.mieAbsorb[c]) * m + d.ozone[c] * o;
        }
    }
    // Midpoint optical depth over [t0, t1] with n steps.
    void depth(const double o[3], const double dir[3], double t0, double t1, int n, double tau[3]) const {
        tau[0] = tau[1] = tau[2] = 0.0;
        const double dt = (t1 - t0) / n;
        for (int i = 0; i < n; ++i) {
            const double t = t0 + (i + 0.5) * dt;
            const double p[3] = {o[0] + dir[0] * t, o[1] + dir[1] * t, o[2] + dir[2] * t};
            double e[3], r[3], m[3];
            coefficients(altitude(p), e, r, m);
            for (int c = 0; c < 3; ++c)
                tau[c] += e[c] * dt;
        }
    }
    // Where the ray leaves the top sphere (from inside).
    double exitTop(const double o[3], const double dir[3]) const {
        const double x = o[0] - d.center[0], y = o[1] - d.center[1], z = o[2] - d.center[2];
        const double b = x * dir[0] + y * dir[1] + z * dir[2];
        const double R = d.seaLevel + top;
        const double q = x * x + y * y + z * z - R * R;
        return -b + std::sqrt(b * b - q);
    }
    // Single scattering of the sun (irradiance 1 from s) along a ray to the
    // top: n steps on the ray, m on each sun path.
    void single(const double o[3], const double dir[3], const double s[3], double L[3], int n = 4000,
                int m = 1500) const {
        L[0] = L[1] = L[2] = 0.0;
        const double tEnd = exitTop(o, dir);
        const double dt = tEnd / n;
        double tau[3] = {0.0, 0.0, 0.0};
        const double cosTheta = dir[0] * s[0] + dir[1] * s[1] + dir[2] * s[2];
        const double pr = 3.0 / (16.0 * kPiD) * (1.0 + cosTheta * cosTheta);
        const double g = d.mieG;
        const double pm = (1.0 - g * g) / (4.0 * kPiD * std::pow(1.0 + g * g - 2.0 * g * cosTheta, 1.5));
        for (int i = 0; i < n; ++i) {
            const double t = (i + 0.5) * dt;
            const double p[3] = {o[0] + dir[0] * t, o[1] + dir[1] * t, o[2] + dir[2] * t};
            double e[3], r[3], mi[3];
            coefficients(altitude(p), e, r, mi);
            double ts[3];
            depth(p, s, 0.0, exitTop(p, s), m, ts);
            for (int c = 0; c < 3; ++c) {
                const double Tc = std::exp(-(tau[c] + 0.5 * e[c] * dt));
                L[c] += Tc * (r[c] * pr + mi[c] * pm) * std::exp(-ts[c]) * dt;
                tau[c] += e[c] * dt;
            }
        }
    }
};

void build(Scene &scene) {
    scene.commit();
    REQUIRE(scene.atmosphere() != nullptr);
    REQUIRE(scene.activeVolumes().empty()); // no fields: not a grid volume
}

Spectrum toSpectrum(const double v[3]) { return {float(v[0]), float(v[1]), float(v[2])}; }

} // namespace

TEST_CASE("The atmosphere's optical depth matches a fine quadrature", "[atmosphere]") {
    Scene scene;
    addAtmosphere(scene, defaultDesc());
    build(scene);
    const AtmosphereView &a = *scene.atmosphere();
    CHECK(a.levelCount >= 4);
    CHECK_THAT(double(a.topAltitude), WithinAbs(56000.0, 1e-3)); // 7 Rayleigh scale heights
    const Model model(*defaultDesc());
    struct Case {
        Vec3f o;
        Vec3f d;
    };
    const float h = 30000.0f;
    const Case cases[] = {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},                                  // up
        {{0.0f, 0.0f, 0.0f}, normalize(Vec3f{0.0f, 1.0f, 1.0f})},                  // 45 degrees
        {{0.0f, 0.0f, 0.0f}, normalize(Vec3f{0.0f, 1.0f, 0.087f})},                // 5 degrees
        {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},                                  // the horizon
        {{0.0f, 0.0f, h}, normalize(Vec3f{1.0f, 0.0f, -0.05f})},                  // from 30 km, below the horizontal
        {{0.0f, 0.0f, 0.0f}, normalize(Vec3f{0.0f, 1.0f, -0.003f})},               // grazing down, past the tangent
    };
    for (const Case &c : cases) {
        AtmosphereRay r;
        r.init(a, c.o, c.d);
        const float ground = atmosphereGroundHit(a, r, kInf);
        float t0 = 0.0f, t1 = 0.0f;
        REQUIRE(atmosphereShell(a, r, ground < kInf ? ground : kInf, t0, t1));
        const Spectrum tau = atmosphereOpticalDepth(a, r, t0, t1);
        const double o[3] = {c.o.x, c.o.y, c.o.z}, d[3] = {c.d.x, c.d.y, c.d.z};
        double ref[3];
        model.depth(o, d, t0, t1, 400000, ref);
        INFO("dir " << c.d.x << " " << c.d.y << " " << c.d.z << " from " << c.o.z << ": [" << t0 << ", " << t1
                    << "] tau " << tau.x << " " << tau.y << " " << tau.z << ", reference " << ref[0] << " "
                    << ref[1] << " " << ref[2]);
        for (int k = 0; k < 3; ++k)
            CHECK_THAT(double(tau[k]), WithinRel(ref[k], 2e-5));
    }
}

TEST_CASE("The atmosphere's ground and shell", "[atmosphere]") {
    Scene scene;
    addAtmosphere(scene, defaultDesc());
    build(scene);
    const AtmosphereView &a = *scene.atmosphere();
    AtmosphereRay r;
    // Straight down from 100 m: the ground at 100 m.
    r.init(a, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
    CHECK_THAT(double(atmosphereGroundHit(a, r, kInf)), WithinAbs(100.0, 1e-3));
    CHECK_THAT(double(r.altitude(50.0f)), WithinAbs(50.0, 1e-3));
    // 10 degrees down: the sphere's root.
    const Vec3f d = normalize(Vec3f{0.0f, 1.0f, -std::tan(10.0f * kPi / 180.0f)});
    r.init(a, {0.0f, 0.0f, 0.0f}, d);
    const double b = kR * double(d.z), q = kR * kR - kSea * kSea;
    CHECK_THAT(double(atmosphereGroundHit(a, r, kInf)), WithinRel(-b - std::sqrt(b * b - q), 1e-6));
    // Up: no ground; from a point on the ground upwards: none either.
    r.init(a, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    CHECK(atmosphereGroundHit(a, r, kInf) == kInf);
    r.init(a, {0.0f, 0.0f, -100.0f}, normalize(Vec3f{0.0f, 1.0f, 0.2f}));
    CHECK(atmosphereGroundHit(a, r, kInf) == kInf);
    // The top: from the origin straight up at 55.9 km.
    float t0 = 0.0f, t1 = 0.0f;
    r.init(a, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    REQUIRE(atmosphereShell(a, r, kInf, t0, t1));
    CHECK(t0 == 0.0f);
    CHECK_THAT(double(t1), WithinRel(55900.0, 1e-6));
    // From space, missing the planet's atmosphere: nothing.
    r.init(a, {0.0f, 0.0f, 400000.0f}, {1.0f, 0.0f, 0.0f});
    CHECK_FALSE(atmosphereShell(a, r, kInf, t0, t1));
}

TEST_CASE("The atmosphere's in-scattering proposal", "[atmosphere]") {
    Scene scene;
    addAtmosphere(scene, defaultDesc());
    build(scene);
    const AtmosphereView &a = *scene.atmosphere();
    for (const Vec3f dir : {Vec3f{0.0f, 0.0f, 1.0f}, normalize(Vec3f{0.0f, 1.0f, 0.02f})}) {
        AtmosphereRay r;
        r.init(a, {0.0f, 0.0f, 0.0f}, dir);
        float t0 = 0.0f, t1 = 0.0f;
        REQUIRE(atmosphereShell(a, r, kInf, t0, t1));
        const Spectrum w{0.2f, 0.3f, 0.5f};
        AtmosphereSegment seg;
        seg.build(a, r, t0, t1, w);
        REQUIRE(seg.count > 1);
        REQUIRE(seg.total() > 0.0f);
        // The density integrates to one, its distribution function matches.
        double integral = 0.0;
        const int n = 200000;
        for (int i = 0; i < n; ++i) {
            const double t = t0 + (i + 0.5) * (double(t1) - t0) / n;
            integral += seg.pdf(float(t)) * (double(t1) - t0) / n;
        }
        CHECK_THAT(integral, WithinAbs(1.0, 2e-3));
        // Samples land where their u says, with their density; the estimate
        // of the integral of w . sigma_s T is unbiased.
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> uni(0.0f, 1.0f);
        double mean = 0.0, mean2 = 0.0;
        const int m = 40000;
        for (int i = 0; i < m; ++i) {
            const float u = uni(rng);
            float density = 0.0f;
            const float x = seg.sample(u, density);
            REQUIRE(x >= t0);
            REQUIRE(x <= t1);
            CHECK_THAT(double(seg.cdfAt(x)), WithinAbs(double(u), 2e-3));
            CHECK_THAT(double(density), WithinRel(double(seg.pdf(x)), 1e-3));
            const AtmosphereCoefficients c = atmosphereAt(a, r.altitude(x));
            const double f = dot(w, (c.rayleigh + c.mie) * expNeg(seg.tauAt(a, r, x)));
            mean += f / density;
            mean2 += (f / density) * (f / density);
        }
        mean /= m;
        const double sigma = std::sqrt((mean2 / m - mean * mean) / m);
        double ref = 0.0;
        for (int i = 0; i < n; ++i) {
            const float t = float(t0 + (i + 0.5) * (double(t1) - t0) / n);
            const AtmosphereCoefficients c = atmosphereAt(a, r.altitude(t));
            ref += dot(w, (c.rayleigh + c.mie) * expNeg(seg.tauAt(a, r, t))) * (double(t1) - t0) / n;
        }
        INFO("estimate " << mean << " +- " << sigma << ", quadrature " << ref);
        CHECK(std::fabs(mean - ref) < 4.0 * sigma + 1e-6 * ref);
        CHECK(sigma < 0.05 * ref); // the proposal follows the integrand
    }
}

TEST_CASE("Rayleigh's phase function", "[atmosphere][volume][phase]") {
    // Normalized, sampled by its own density: E[cos^2] = 2/5.
    double integral = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const float c = -1.0f + (float(i) + 0.5f) / 1000.0f;
        integral += 2.0 * kPiD * rayleighPhase(c) / 1000.0;
    }
    CHECK_THAT(integral, WithinAbs(1.0, 1e-6));
    PhaseFunction p;
    p.kind = PhaseFunction::kRayleigh;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    double c2 = 0.0;
    const int n = 200000;
    const Vec3f d{0.0f, 0.0f, 1.0f};
    for (int i = 0; i < n; ++i) {
        float pdf = 0.0f;
        const Vec3f wi = p.sample(d, {uni(rng), uni(rng), uni(rng)}, pdf);
        REQUIRE_THAT(double(pdf), WithinRel(double(rayleighPhase(dot(d, wi))), 1e-4));
        c2 += double(dot(d, wi)) * dot(d, wi);
    }
    CHECK_THAT(c2 / n, WithinAbs(0.4, 0.003));
    for (float u : {0.0f, 0.25f, 0.5f, 0.75f, 0.9999f}) {
        const float c = sampleRayleighCos(u);
        CHECK_THAT(double((3.0f * c + c * c * c + 4.0f) / 8.0f), WithinAbs(double(u), 1e-5));
    }
}

namespace {

// An absorbing atmosphere (Mie absorption and ozone, nothing scatters): its
// renders are exact.
std::shared_ptr<AtmosphereDesc> absorbingDesc() {
    auto d = defaultDesc();
    d->rayleigh = spectrum(0.0f);
    d->mieScatter = spectrum(0.0f);
    d->mieAbsorb = Spectrum{3e-5f, 6e-5f, 1e-4f};
    return d;
}

Film renderOrtho(const Scene &scene, Vec3f eye, uint32_t spp, int size = 8, float halfWidth = 0.4f) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = 4;
    settings.clampIndirect = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera(eye, halfWidth, size, size));
    renderer.resize(size, size);
    renderToConvergence(renderer);
    return renderer.film();
}

Spectrum filmMean(const Film &film) {
    Spectrum s{0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < film.colorSum.size(); ++i)
        s += film.colorSum[i] / float(film.count[i]);
    return s / float(film.colorSum.size());
}

double alphaMean(const Film &film) {
    double s = 0.0;
    for (size_t i = 0; i < film.alphaSum.size(); ++i)
        s += film.alphaSum[i] / film.count[i];
    return s / double(film.alphaSum.size());
}

} // namespace

TEST_CASE("The sun reaches surfaces through the atmosphere, not below the horizon", "[atmosphere][render]") {
    for (const float alt : {30.0f, 5.0f, -3.0f}) {
        Scene scene;
        addAtmosphere(scene, absorbingDesc());
        addSun(scene, alt);
        const MaterialId white = scene.createMaterial();
        MaterialDesc m;
        m.inputs = lambertDefaults(spectrum(1.0f));
        scene.updateMaterial(white, m);
        const MeshId quad = scene.createMesh();
        scene.updateMesh(quad, makeQuad(1.0f)); // at the origin, 100 m above sea level, facing +Z
        scene.setMeshMaterials(quad, {white});
        build(scene);
        const Film film = renderOrtho(scene, {0.0f, 0.0f, 5.0f}, 4);
        const Spectrum got = filmMean(film);
        const Model model(*absorbingDesc());
        const Vec3f s = sunDir(alt);
        Spectrum expected = spectrum(0.0f);
        if (alt > 0.0f) {
            const double o[3] = {0.0, 0.0, 0.0}, d[3] = {s.x, s.y, s.z};
            double tau[3], view[3];
            model.depth(o, d, 0.0, model.exitTop(o, d), 400000, tau);
            const double eye[3] = {0.0, 0.0, 5.0}, down[3] = {0.0, 0.0, -1.0};
            model.depth(eye, down, 0.0, 5.0, 100, view); // the camera's 5 m to the quad
            for (int c = 0; c < 3; ++c)
                expected[c] = float(std::sin(alt * kPiD / 180.0) / kPiD * std::exp(-tau[c] - view[c]));
        }
        INFO("sun at " << alt << ": " << got.x << " " << got.y << " " << got.z << ", expected " << expected.x << " "
                       << expected.y << " " << expected.z);
        for (int c = 0; c < 3; ++c)
            CHECK_THAT(double(got[c]), WithinAbs(double(expected[c]), 2e-5 * double(expected[c]) + 1e-7));
    }
}

TEST_CASE("The atmosphere's alpha and its visible ground", "[atmosphere][render]") {
    const Model model(*absorbingDesc());
    { // camera rays up: alpha 1 - mean T to the top, colour black (nothing scatters, no lights)
        Scene scene;
        addAtmosphere(scene, absorbingDesc());
        build(scene);
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 2;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        Camera cam = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, 50.0, 1.0, 4, 4); // narrow, looking down -Z
        // Look up instead: flip the view.
        Mat4 flip = Mat4::identity();
        flip.m[1][1] = -1.0;
        flip.m[2][2] = -1.0;
        cam.viewToWorld = flip;
        cam.worldToView = inverse(flip);
        renderer.setCamera(cam);
        renderer.resize(4, 4);
        renderToConvergence(renderer);
        const double o[3] = {0.0, 0.0, 0.0}, d[3] = {0.0, 0.0, 1.0};
        double tau[3];
        model.depth(o, d, 0.0, model.exitTop(o, d), 400000, tau);
        const double alpha = 1.0 - (std::exp(-tau[0]) + std::exp(-tau[1]) + std::exp(-tau[2])) / 3.0;
        CHECK_THAT(alphaMean(renderer.film()), WithinRel(alpha, 2e-3)); // the narrow view's spread
    }
    { // the ground seen from above (visible to the camera): albedo / pi E cos T_sun T_view, alpha 1
        auto desc = absorbingDesc();
        desc->groundVisibility = kVisibleAll;
        desc->groundAlbedo = Spectrum{0.5f, 0.3f, 0.2f};
        Scene scene;
        addAtmosphere(scene, desc);
        addSun(scene, 40.0f);
        build(scene);
        const Film film = renderOrtho(scene, {0.0f, 0.0f, 0.0f}, 2, 4, 0.1f);
        const Spectrum got = filmMean(film);
        const Vec3f s = sunDir(40.0f);
        const double g[3] = {0.0, 0.0, -100.0}, ds[3] = {s.x, s.y, s.z};
        double ts[3];
        model.depth(g, ds, 0.0, model.exitTop(g, ds), 400000, ts);
        const double o[3] = {0.0, 0.0, 0.0}, down[3] = {0.0, 0.0, -1.0};
        double tv[3];
        model.depth(o, down, 0.0, 100.0, 4000, tv);
        for (int c = 0; c < 3; ++c) {
            const double expected =
                desc->groundAlbedo[c] / kPiD * std::sin(40.0 * kPiD / 180.0) * std::exp(-ts[c] - tv[c]);
            CHECK_THAT(double(got[c]), WithinRel(expected, 1e-4));
        }
        CHECK_THAT(alphaMean(film), WithinAbs(1.0, 1e-6));
    }
}

TEST_CASE("Single scattering in the atmosphere matches a quadrature", "[atmosphere][render]") {
    // Volume limit 0: the sun scattered once (the continuation only gathers
    // lights). A narrow camera at 100 m looking 20 degrees up, the sun 30
    // degrees up and 40 degrees to the side.
    auto desc = defaultDesc();
    const float sunAlt = 30.0f;
    Scene scene;
    addAtmosphere(scene, desc, 0.0f);
    addSun(scene, sunAlt);
    build(scene);
    // The view: from the origin towards (sin 40 cos 20, cos 40 cos 20, sin 20).
    const float az = 40.0f * kPi / 180.0f, el = 20.0f * kPi / 180.0f;
    const Vec3f view{std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)};
    const Frame f = Frame::fromNormal(-view); // the camera looks down its -Z
    Mat4 m = Mat4::identity();
    const Vec3f axes[3] = {f.t, f.b, f.n};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m.m[r][c] = axes[r][c];
    Camera cam = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, 100.0, 0.2, 2, 2, 0.01, 1.0e7);
    cam.viewToWorld = m;
    cam.worldToView = inverse(m);
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 16384;
    settings.maxDepth = 8;
    settings.clampIndirect = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(cam);
    renderer.resize(2, 2);
    renderToConvergence(renderer);
    const Spectrum got = filmMean(renderer.film());
    const Model model(*desc);
    const double o[3] = {0.0, 0.0, 0.0}, d[3] = {view.x, view.y, view.z};
    const Vec3f s = sunDir(sunAlt);
    const double sd[3] = {s.x, s.y, s.z};
    double L[3];
    model.single(o, d, sd, L);
    INFO("Nova " << got.x << " " << got.y << " " << got.z << ", quadrature " << L[0] << " " << L[1] << " " << L[2]);
    for (int c = 0; c < 3; ++c)
        CHECK_THAT(double(got[c]), WithinRel(L[c], 0.01));
}

// --- The visible ground with several suns and with volumes (M11 review) ----

namespace {

// A sun (a delta distant light of irradiance `intensity`) at altitude
// `altDeg` and azimuth `azDeg` (0: towards +Y, 90: towards +X).
Vec3f addSunAt(Scene &scene, float altDeg, float azDeg, float intensity) {
    const float a = altDeg * kPi / 180.0f, z = azDeg * kPi / 180.0f;
    const Vec3f to{std::sin(z) * std::cos(a), std::cos(z) * std::cos(a), std::sin(a)};
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.normalize = true;
    sun.angleDeg = 0.0f;
    sun.intensity = intensity;
    const Frame f = Frame::fromNormal(to);
    const Vec3f axes[3] = {f.t, f.b, f.n};
    for (int r = 0; r < 3; ++r) {
        sun.transform.m[r][0] = axes[r].x;
        sun.transform.m[r][1] = axes[r].y;
        sun.transform.m[r][2] = axes[r].z;
    }
    scene.updateLight(scene.createLight(), sun);
    return to;
}

// A shell box [lo, hi] of constant absorption, scattering and emission
// (a light source with `light`).
VolumeId addMediumBox(Scene &scene, Vec3f lo, Vec3f hi, Spectrum absorption, Spectrum scattering,
                      Spectrum emission = spectrum(0.0f), bool light = false) {
    using namespace shading;
    IrGraph g;
    IrBuilder b(g);
    const auto field = [](uint16_t f) { return uint16_t(volume_field::kBase + f); };
    b.output(field(volume_field::scattering), 3, b.constant(scattering.x, scattering.y, scattering.z));
    b.output(field(volume_field::absorption), 3, b.constant(absorption.x, absorption.y, absorption.z));
    b.output(field(volume_field::emission), 3, b.constant(emission.x, emission.y, emission.z));
    MaterialDesc m;
    m.program = compile(g).program;
    REQUIRE(m.program);
    const std::vector<Vec3f> p = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                                  {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    const std::vector<uint32_t> idx = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                       1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
    VolumeDesc v;
    v.name = "/box";
    v.shell = makeTriangleShell(p.data(), p.size(), idx.data(), idx.size() / 3);
    v.material = scene.createMaterial();
    v.light = light;
    scene.updateMaterial(v.material, m);
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, v);
    scene.updateVolumeMotion(id, {Mat4::identity()});
    return id;
}

} // namespace

// With groundreceiveshadow the lit suns shadow the ground through shadow
// rays; a path keeps one light sample per event (the XPU's one shadow ray per
// path and slot, the CPU's queue of four per path): the ground picks one sun
// in proportion to its light. Five lit suns (they overflowed the CPU's queue
// and raced on the XPU) give the exact sum of their light.
TEST_CASE("The atmosphere's ground under several suns with groundreceiveshadow", "[atmosphere][render]") {
    auto desc = absorbingDesc();
    desc->groundVisibility = kVisibleAll;
    desc->groundShadows = true;
    desc->groundAlbedo = Spectrum{0.5f, 0.3f, 0.2f};
    Scene scene;
    addAtmosphere(scene, desc);
    struct Sun {
        float alt, az, intensity;
    };
    const Sun suns[] = {{40.0f, 0.0f, 1.0f}, {40.0f, 120.0f, 0.5f}, {40.0f, 240.0f, 2.0f},
                        {25.0f, 60.0f, 1.0f}, {60.0f, 200.0f, 1.5f}, {-10.0f, 90.0f, 3.0f}}; // the last below the horizon
    std::vector<Vec3f> dirs;
    for (const Sun &s : suns)
        dirs.push_back(addSunAt(scene, s.alt, s.az, s.intensity));
    build(scene);
    const Film film = renderOrtho(scene, {0.0f, 0.0f, 0.0f}, 256, 4, 0.1f);
    const Spectrum got = filmMean(film);
    const Model model(*desc);
    const double g[3] = {0.0, 0.0, -100.0};
    const double o[3] = {0.0, 0.0, 0.0}, down[3] = {0.0, 0.0, -1.0};
    double tv[3];
    model.depth(o, down, 0.0, 100.0, 4000, tv);
    double expected[3] = {0.0, 0.0, 0.0};
    for (size_t i = 0; i < dirs.size(); ++i) {
        if (!(suns[i].alt > 0.0f))
            continue;
        const double ds[3] = {dirs[i].x, dirs[i].y, dirs[i].z};
        double ts[3];
        model.depth(g, ds, 0.0, model.exitTop(g, ds), 400000, ts);
        for (int c = 0; c < 3; ++c)
            expected[c] += desc->groundAlbedo[c] / kPiD * suns[i].intensity * std::sin(suns[i].alt * kPiD / 180.0) *
                           std::exp(-ts[c] - tv[c]);
    }
    INFO("Nova " << got.x << " " << got.y << " " << got.z << ", expected " << expected[0] << " " << expected[1] << " "
                 << expected[2]);
    for (int c = 0; c < 3; ++c)
        CHECK_THAT(double(got[c]), WithinRel(expected[c], 2e-3));
    CHECK_THAT(alphaMean(film), WithinAbs(1.0, 1e-6));
}

// A visible ground ends the volumes' free flight: a fog box from 10 m below
// to 10 m above sea level (no ground mesh) dims the ground seen from above
// by its upper half only; the half behind the ground neither dims it nor
// takes paths away.
TEST_CASE("Volumes behind the atmosphere's visible ground do not reach it", "[atmosphere][render][volume]") {
    auto desc = absorbingDesc();
    desc->groundVisibility = kVisibleAll;
    desc->groundAlbedo = Spectrum{0.5f, 0.3f, 0.2f};
    const float sigma = 0.05f;
    for (const bool scatter : {false, true}) {
        Scene scene;
        addAtmosphere(scene, desc);
        addSun(scene, 40.0f);
        // Sea level is 100 m below the origin.
        addMediumBox(scene, {-5.0f, -5.0f, -110.0f}, {5.0f, 5.0f, -90.0f}, spectrum(scatter ? 0.0f : sigma),
                     spectrum(scatter ? sigma : 0.0f));
        scene.commit();
        REQUIRE(scene.atmosphere() != nullptr);
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 1024;
        settings.maxDepth = scatter ? 4 : 1;
        settings.clampIndirect = 0.0f;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 0.0f}, 0.1f, 4, 4));
        renderer.resize(4, 4);
        renderToConvergence(renderer);
        const Spectrum got = filmMean(renderer.film());
        const Model model(*desc);
        const Vec3f s = sunDir(40.0f);
        const double g[3] = {0.0, 0.0, -100.0}, ds[3] = {s.x, s.y, s.z};
        double ts[3];
        model.depth(g, ds, 0.0, model.exitTop(g, ds), 400000, ts);
        const double o[3] = {0.0, 0.0, 0.0}, down[3] = {0.0, 0.0, -1.0};
        double tv[3];
        model.depth(o, down, 0.0, 100.0, 4000, tv);
        INFO((scatter ? "scattering" : "absorbing") << " fog: Nova " << got.x << " " << got.y << " " << got.z);
        for (int c = 0; c < 3; ++c) {
            // The ground's light (without groundreceiveshadow the volumes do
            // not shadow it) through the fog's upper 10 m.
            const double ground = desc->groundAlbedo[c] / kPiD * std::sin(40.0 * kPiD / 180.0) *
                                  std::exp(-ts[c] - tv[c]) * std::exp(-double(sigma) * 10.0);
            if (scatter) // and the fog's own in-scattered light on top
                CHECK(double(got[c]) > 0.98 * ground);
            else
                CHECK_THAT(double(got[c]), WithinRel(ground, 0.01));
        }
    }
}

// A volume light in front of a visible ground still adds its light to rays
// that only gather lights (here camera rays at path length 0): the tracker
// runs up to the ground.
TEST_CASE("Volume lights in front of the atmosphere's ground", "[atmosphere][render][volume]") {
    auto desc = defaultDesc();
    desc->rayleigh = spectrum(0.0f);
    desc->mieScatter = spectrum(0.0f);
    desc->mieAbsorb = spectrum(0.0f);
    desc->ozone = spectrum(0.0f);
    desc->groundVisibility = kVisibleAll;
    Scene scene;
    addAtmosphere(scene, desc);
    // 20 m of emission between 40 and 60 m below the camera.
    addMediumBox(scene, {-1.0f, -1.0f, -60.0f}, {1.0f, 1.0f, -40.0f}, spectrum(0.0f), spectrum(0.0f),
                 Spectrum{0.1f, 0.05f, 0.025f}, true);
    scene.commit();
    REQUIRE(scene.atmosphere() != nullptr);
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 64;
    settings.maxDepth = 0;
    settings.clampIndirect = 0.0f;
    settings.clampDirect = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 0.0f}, 0.1f, 4, 4));
    renderer.resize(4, 4);
    renderToConvergence(renderer);
    const Spectrum got = filmMean(renderer.film());
    INFO("Nova " << got.x << " " << got.y << " " << got.z);
    CHECK_THAT(double(got.x), WithinRel(2.0, 0.01));
    CHECK_THAT(double(got.y), WithinRel(1.0, 0.01));
    CHECK_THAT(double(got.z), WithinRel(0.5, 0.01));
}
