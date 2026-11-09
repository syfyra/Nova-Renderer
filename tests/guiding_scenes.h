#pragma once

// Scenes of the path guiding tests (docs/design/M11-P9c.md), CPU and XPU.

#include "test_helpers.h"

#include "core/bsdf/surface.h"
#include "core/scene/scene.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

namespace nova::test {

// A quad from a corner and two edges, the default material's colour.
inline MeshDesc guideQuad(Vec3f o, Vec3f e1, Vec3f e2, float grey) {
    MeshDesc m;
    m.points = {o, o + e1, o + e1 + e2, o + e2};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.setDisplayColor(spectrum(grey));
    return m;
}

// A closed grey room [-1, 1] x [0, 2] x [-1, 1] lit only by a sun through a
// 0.4 x 0.4 window in its +x wall (y 1..1.4, z -0.2..0.2), shining down at
// 45 degrees onto a patch of the floor: the rest is lit by light bounced off
// that patch. With `fog`, a homogeneous scattering medium fills the room
// (density `fog`, white, isotropic).
inline void guideRoom(Scene &scene, float grey = 0.7f) {
    const auto add = [&](const MeshDesc &m) { scene.updateMesh(scene.createMesh(), m); };
    add(guideQuad({-1, 0, -1}, {2, 0, 0}, {0, 0, 2}, grey));  // floor
    add(guideQuad({-1, 2, -1}, {0, 0, 2}, {2, 0, 0}, grey));  // ceiling
    add(guideQuad({-1, 0, -1}, {0, 2, 0}, {2, 0, 0}, grey));  // -z
    add(guideQuad({-1, 0, 1}, {2, 0, 0}, {0, 2, 0}, grey));   // +z
    add(guideQuad({-1, 0, -1}, {0, 0, 2}, {0, 2, 0}, grey));  // -x
    // +x with the window.
    add(guideQuad({1, 0, -1}, {0, 1, 0}, {0, 0, 2}, grey));       // below
    add(guideQuad({1, 1.4f, -1}, {0, 0.6f, 0}, {0, 0, 2}, grey)); // above
    add(guideQuad({1, 1, -1}, {0, 0.4f, 0}, {0, 0, 0.8f}, grey)); // -z side
    add(guideQuad({1, 1, 0.2f}, {0, 0.4f, 0}, {0, 0, 0.8f}, grey)); // +z side
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.angleDeg = 0.0f;
    sun.intensity = 6.0f;
    // Local -Z towards (-1, -1, 0): rows are the images of local X, Y, Z.
    const float a = std::sqrt(0.5f);
    Mat4 m = Mat4::identity();
    m.m[0][0] = 0.0f, m.m[0][1] = 0.0f, m.m[0][2] = 1.0f;
    m.m[1][0] = a, m.m[1][1] = -a, m.m[1][2] = 0.0f;
    m.m[2][0] = a, m.m[2][1] = a, m.m[2][2] = 0.0f;
    sun.transform = m;
    scene.updateLight(scene.createLight(), sun);
}

// The camera inside the room near the window wall, looking at the far
// corner (-x, -z) and the floor.
inline Camera guideRoomCamera(int width, int height) {
    Camera c = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, 0.016, 0.036, width, height);
    // Yaw 45 degrees towards -x, pitch -10 degrees, at (0.85, 1.2, 0.85).
    const float yaw = 0.7853982f, pitch = -0.1745329f;
    Mat4 r = Mat4::identity();
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    // Rows: images of view X, Y, Z (the camera looks down -Z).
    r.m[0][0] = cy, r.m[0][1] = 0.0f, r.m[0][2] = -sy;
    r.m[1][0] = sy * sp, r.m[1][1] = cp, r.m[1][2] = cy * sp;
    r.m[2][0] = sy * cp, r.m[2][1] = -sp, r.m[2][2] = cy * cp;
    r.m[3][0] = 0.85f, r.m[3][1] = 1.2f, r.m[3][2] = 0.85f;
    c.viewToWorld = r;
    c.worldToView = inverse(r);
    return c;
}

struct GuideRun {
    Film film;
    uint32_t iterations = 0;
    size_t leaves = 0, nodes = 0; // the field at the end
    uint32_t discarded = 0;
    bool trainingDone = false;
    bool gpu = true;
};

inline GuideRun renderGuided(const Scene &scene, const Camera &camera, int width, int height, RenderSettings settings,
                             int threads = 0) {
    Renderer renderer;
    renderer.setThreadLimit(threads);
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(width, height);
    GuideRun run;
    while (!renderer.converged()) {
        renderer.renderPass({});
        run.gpu = run.gpu && renderer.lastPassOnGpu();
    }
    run.film = renderer.film();
    if (const PathGuide *g = renderer.guide()) {
        run.iterations = g->iterations();
        run.leaves = g->leaves.size();
        run.nodes = g->nodes.size();
        run.trainingDone = !g->training();
    }
    run.discarded = renderer.discardedSamples();
    return run;
}

// Whether two films hold bit-identical sums.
inline bool sameSums(const Film &a, const Film &b) {
    return a.colorSum.size() == b.colorSum.size() &&
           std::memcmp(a.colorSum.data(), b.colorSum.data(), a.colorSum.size() * sizeof(Spectrum)) == 0;
}

// Per-pixel mean luminance of a film.
inline std::vector<double> pixelLuminance(const Film &film) {
    std::vector<double> v(film.colorSum.size());
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = film.count[i] ? double(luminance(film.colorSum[i])) / double(film.count[i]) : 0.0;
    return v;
}

// Statistics over renders with different seeds: the image mean (and its
// standard error) and the median over pixels of the relative variance
// across the seeds.
struct SeedStats {
    double mean = 0.0, error = 0.0, noise = 0.0;
};

inline SeedStats seedStats(const std::vector<std::vector<double>> &images) {
    SeedStats s;
    const size_t n = images.size(), pixels = images[0].size();
    std::vector<double> means(n, 0.0);
    for (size_t k = 0; k < n; ++k) {
        for (double v : images[k])
            means[k] += v;
        means[k] /= double(pixels);
        s.mean += means[k] / double(n);
    }
    double var = 0.0;
    for (double m : means)
        var += (m - s.mean) * (m - s.mean);
    s.error = std::sqrt(var / double(n - 1) / double(n));
    std::vector<double> rel;
    for (size_t p = 0; p < pixels; ++p) {
        double mu = 0.0, sq = 0.0;
        for (size_t k = 0; k < n; ++k)
            mu += images[k][p];
        mu /= double(n);
        if (!(mu > 1e-4))
            continue;
        for (size_t k = 0; k < n; ++k)
            sq += (images[k][p] - mu) * (images[k][p] - mu);
        rel.push_back(sq / double(n - 1) / (mu * mu));
    }
    std::nth_element(rel.begin(), rel.begin() + rel.size() / 2, rel.end());
    s.noise = rel.empty() ? 0.0 : rel[rel.size() / 2];
    return s;
}

} // namespace nova::test
