// Path guiding on Nova XPU (docs/design/M11-P9c.md): the same field, trained
// from the device's records, guides the GPU's paths; the image keeps the
// CPU's mean, its noise drops as on the CPU, renders are deterministic, and
// the training controls behave as on the CPU. Skipped without a usable
// NVIDIA device.

#include "guiding_scenes.h"

#include "core/gpu/device.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;

namespace {

bool gpuUsable(std::string &why) {
    gpu::Device device;
    return device.init(why);
}

RenderSettings xpuGuided(uint32_t spp, bool on, uint32_t seed = 0) {
    RenderSettings s;
    s.device = RenderDevice::Xpu;
    s.maxSamples = spp;
    s.pixelFilterScale = 0.0f;
    s.clampIndirect = 0.0f;
    s.seed = seed;
    s.guiding.enable = on;
    return s;
}

} // namespace

TEST_CASE("XPU path guiding", "[gpu][render][guiding]") {
    std::string why;
    if (!gpuUsable(why))
        SKIP("no usable GPU: " << why);
    Scene scene;
    guideRoom(scene);
    scene.commit();
    const Camera cam = guideRoomCamera(96, 72);

    SECTION("the mean stays, the noise drops, as on the CPU") {
        std::vector<std::vector<double>> off, on, cpu;
        for (uint32_t seed = 0; seed < 8; ++seed) {
            const GuideRun a = renderGuided(scene, cam, 96, 72, xpuGuided(32, false, seed));
            const GuideRun b = renderGuided(scene, cam, 96, 72, xpuGuided(32, true, seed));
            REQUIRE(a.gpu);
            REQUIRE(b.gpu);
            CHECK(b.iterations >= 5);
            off.push_back(pixelLuminance(a.film));
            on.push_back(pixelLuminance(b.film));
            RenderSettings c = xpuGuided(32, true, seed);
            c.device = RenderDevice::Cpu;
            cpu.push_back(pixelLuminance(renderGuided(scene, cam, 96, 72, c).film));
        }
        const SeedStats a = seedStats(off), b = seedStats(on), c = seedStats(cpu);
        INFO("XPU off: mean " << a.mean << " +- " << a.error << " noise " << a.noise << "; XPU on: mean " << b.mean
                              << " +- " << b.error << " noise " << b.noise << "; CPU on: mean " << c.mean << " +- "
                              << c.error << " noise " << c.noise);
        CHECK(std::fabs(a.mean - b.mean) < 4.0 * std::sqrt(a.error * a.error + b.error * b.error) + 1e-4);
        CHECK(std::fabs(c.mean - b.mean) < 4.0 * std::sqrt(c.error * c.error + b.error * b.error) + 1e-4);
        CHECK(b.noise < 0.95 * a.noise);
        CHECK(b.noise < 1.25 * c.noise);
        CHECK(c.noise < 1.25 * b.noise);
    }
    SECTION("deterministic: two renders are bit-identical") {
        const GuideRun a = renderGuided(scene, cam, 96, 72, xpuGuided(16, true));
        const GuideRun b = renderGuided(scene, cam, 96, 72, xpuGuided(16, true));
        REQUIRE(a.gpu);
        CHECK(sameSums(a.film, b.film));
    }
    SECTION("no listed component: the image of guiding off") {
        RenderSettings s = xpuGuided(8, true);
        s.guiding.components = guide::kReflect;
        const GuideRun a = renderGuided(scene, cam, 96, 72, s);
        const GuideRun b = renderGuided(scene, cam, 96, 72, xpuGuided(8, false));
        CHECK(sameSums(a.film, b.film));
    }
    SECTION("clearbuffers drops the training samples") {
        RenderSettings s = xpuGuided(16, true);
        s.guiding.trainingSamples = 4;
        s.guiding.clearBuffers = true;
        const GuideRun run = renderGuided(scene, cam, 96, 72, s);
        CHECK(run.gpu);
        CHECK(run.trainingDone);
        CHECK(run.discarded == 4u);
        CHECK(run.film.count[0] == 12u);
    }
}
