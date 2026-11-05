// The OptiX denoiser (docs/design/M7.md D1, M7.4). Skipped without an NVIDIA
// driver: there the denoiser must only report itself unavailable.

#include "test_helpers.h"

#include "core/denoise/denoiser.h"
#include "core/integrator/renderer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace nova;
using namespace nova::test;

namespace {

LightId addLight(Scene &scene) {
    LightDesc light;
    light.type = LightType::Distant;
    light.intensity = 2.0f;
    light.normalize = true;
    light.angleDeg = 25.0f;
    const LightId id = scene.createLight();
    scene.updateLight(id, light);
    return id;
}

struct FilmImage {
    std::vector<float> rgba, albedo, normal; // rows top-down
};

// The film as the denoiser takes it: mean RGBA, the first-hit guides, rows
// top-down (the film's row 0 is the bottom).
FilmImage fromFilm(const Film &film) {
    FilmImage im;
    const size_t n = film.pixelCount();
    im.rgba.resize(n * 4);
    im.albedo.resize(n * 3);
    im.normal.resize(n * 3);
    for (int y = 0; y < film.height; ++y)
        for (int x = 0; x < film.width; ++x) {
            const size_t src = size_t(y) * size_t(film.width) + size_t(x);
            const size_t dst = size_t(film.height - 1 - y) * size_t(film.width) + size_t(x);
            const float inv = 1.0f / float(film.count[src]);
            const Spectrum c = film.colorSum[src] * inv;
            const float v[4] = {c.x, c.y, c.z, film.alphaSum[src] * inv};
            for (int k = 0; k < 4; ++k)
                im.rgba[dst * 4 + size_t(k)] = v[k];
            const float a[3] = {film.albedo[src].x, film.albedo[src].y, film.albedo[src].z};
            const float nn[3] = {film.normal[src].x, film.normal[src].y, film.normal[src].z};
            for (int k = 0; k < 3; ++k) {
                im.albedo[dst * 3 + size_t(k)] = a[k];
                im.normal[dst * 3 + size_t(k)] = nn[k];
            }
        }
    return im;
}

double relMse(const std::vector<float> &a, const std::vector<float> &truth) {
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (size_t k = 0; k < 3; ++k, ++n) {
            const double d = double(a[i + k]) - double(truth[i + k]);
            sum += d * d / (double(truth[i + k]) * double(truth[i + k]) + 0.01);
        }
    return sum / double(n);
}

} // namespace

TEST_CASE("The OptiX denoiser", "[denoise]") {
    // Without a driver (forced here) the denoiser is unavailable, nothing else.
    {
#if defined(_WIN32)
        _putenv("NOVA_DENOISE_DISABLE=1");
#endif
        Denoiser off;
        CHECK_FALSE(off.available());
        CHECK_FALSE(off.message().empty());
        float pixel[4] = {1.0f, 2.0f, 3.0f, 0.5f};
        const Denoiser::Layer layer{pixel, pixel};
        CHECK_FALSE(off.denoise(1, 1, &layer, 1, nullptr, nullptr));
        CHECK(pixel[1] == 2.0f);
#if defined(_WIN32)
        _putenv("NOVA_DENOISE_DISABLE=");
#endif
    }
    Denoiser &denoiser = sharedDenoiser();
    if (!denoiser.available())
        SKIP("OptiX denoiser unavailable: " << denoiser.message());

    // A ball on the ground under a wide distant light: a noisy penumbra.
    Scene scene;
    const MeshId ground = scene.createMesh();
    MeshDesc quad = makeQuad(100.0f);
    quad.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(ground, quad);
    const MeshId ball = scene.createMesh();
    MeshDesc sphere = makeSphere(0.3f, 32, 16);
    for (Vec3f &p : sphere.points)
        p.z += 0.4f;
    sphere.setDisplayColor(Spectrum{0.8f, 0.3f, 0.1f});
    scene.updateMesh(ball, sphere);
    addLight(scene);
    scene.commit();
    const int w = 96, h = 64;
    const auto render = [&](uint32_t samples, uint32_t seed) {
        auto renderer = std::make_unique<Renderer>();
        RenderSettings settings;
        settings.maxSamples = samples;
        settings.seed = seed;
        renderer->setScene(&scene);
        renderer->setSettings(settings);
        renderer->setCamera(makeOrthoCamera({0.0f, 0.0f, 3.0f}, 1.0f, w, h));
        renderer->resize(w, h);
        renderToConvergence(*renderer);
        return fromFilm(renderer->film());
    };
    const FilmImage noisy = render(8, 0);
    const FilmImage truth = render(1024, 7);

    std::vector<float> out(noisy.rgba.size());
    const Denoiser::Layer layer{noisy.rgba.data(), out.data()};
    REQUIRE(denoiser.denoise(w, h, &layer, 1, noisy.albedo.data(), noisy.normal.data()));
    const double before = relMse(noisy.rgba, truth.rgba), after = relMse(out, truth.rgba);
    INFO("relMSE noisy " << before << ", denoised " << after);
    CHECK(after < before / 3.0);
    bool alpha = true; // copied, not denoised
    for (size_t i = 3; i < out.size(); i += 4)
        alpha = alpha && out[i] == noisy.rgba[i];
    CHECK(alpha);

    // Tiles with OptiX's overlap give the whole frame's result.
    std::vector<float> tiled(noisy.rgba.size());
    denoiser.setMaxTile(64); // two tiles across
    const Denoiser::Layer tiledLayer{noisy.rgba.data(), tiled.data()};
    REQUIRE(denoiser.denoise(w, h, &tiledLayer, 1, noisy.albedo.data(), noisy.normal.data()));
    denoiser.setMaxTile(0);
    double diff = 0.0, mean = 0.0;
    for (size_t i = 0; i < out.size(); i += 4)
        for (size_t k = 0; k < 3; ++k) {
            diff += std::fabs(double(tiled[i + k]) - double(out[i + k]));
            mean += std::fabs(double(out[i + k]));
        }
    CHECK(diff / mean < 5e-3); // 0.13 % here, where borders are a large part of the image (0.02 % at 4K)

    // A second layer (an AOV) is denoised with the beauty's kernels.
    std::vector<float> outBeauty(noisy.rgba.size()), outAov(noisy.rgba.size());
    const Denoiser::Layer two[2] = {{noisy.rgba.data(), outBeauty.data()}, {noisy.rgba.data(), outAov.data()}};
    REQUIRE(denoiser.denoise(w, h, two, 2, noisy.albedo.data(), noisy.normal.data()));
    CHECK(relMse(outAov, truth.rgba) < before / 3.0);
}
