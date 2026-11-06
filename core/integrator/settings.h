#pragma once

#include "core/film/filter.h"
#include "core/lights/light_strategy.h"
#include "core/scene/ray_limits.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace nova {

namespace lpe {
struct Compiled;
}
struct UtilityRequest;
struct CryptoRequest;

// Where passes render (docs/design/M10.md D1, D15): the CPU, or the GPU with
// the CPU as fallback (Nova XPU).
enum class RenderDevice : uint8_t { Cpu, Xpu };

struct RenderSettings {
    RenderDevice device = RenderDevice::Cpu;
    // The GPU's texture memory in MB (docs/design/M10.md D10; 0: half the
    // memory free at the first texture upload). Past it finer MIP levels drop.
    uint32_t gpuTextureBudget = 0;
    uint32_t maxSamples = 64;     // progressive target (samples per pixel; the maximum when adaptive)
    // Adaptive sampling (docs/design/M7.md D4): pixels stop once their noise
    // estimate is below the threshold; the minimum is checked first
    // (0: automatic, max(16, sqrt(maxSamples)); rounded up to a power of two).
    bool adaptive = false;
    uint32_t oracleMinSamples = 0;
    float oracleThreshold = 0.01f;
    uint32_t maxDepth = 6;        // lit scattering events per path (1 = direct light only); a cap with limits
    RayLimits limits;             // per-ray-type limits, the objects' defaults (core: none)
    uint32_t rrStartDepth = 3;    // Russian roulette starts at this depth
    float clampIndirect = 10.0f;  // firefly clamp for indirect light (0 = off)
    // Karma's colour limits (docs/design/M11-P1.md, measured): clampDirect
    // limits what the camera sees directly (emission, lights) and the light
    // gathered at the first hit (light samples, BSDF rays reaching lights);
    // with clampEmissionIndirect, emission of objects that are not lights
    // found by the first hit's BSDF ray counts as indirect (Karma), else as
    // direct. 0: off (Nova's default: no direct clamp).
    float clampDirect = 0.0f;
    bool clampEmissionIndirect = false;
    // Karma's constrainmaxrough: a glossy lobe is at least as rough as the
    // roughest glossy event before it on the path.
    bool constrainRoughness = false;
    uint32_t seed = 0;
    PixelFilterType pixelFilter = PixelFilterType::Gaussian;
    float pixelFilterScale = 1.0f; // multiplies the filter's base size (0 = point sampling)
    LightStrategy lightStrategy = LightStrategy::MIS;
    // Light selection by the light tree (docs/design/M9.md D11), else by power
    // (Karma's light_sampling_mode Uniform; a debugging mode).
    bool lightTree = true;
    // The frame being rendered and the stage's frames per second (MaterialX
    // frame and time nodes; Karma: time = (frame - 1) / fps).
    float frame = 1.0f;
    float fps = 24.0f;
    // Texture footprints: the ray cone's diameter at a hit times this scale
    // (Karma's measured MIP choice is 0.25 of the pixel footprint), 0 for
    // the finest level everywhere.
    float textureFilterWidth = 0.25f;
    // Light path expressions to render besides the beauty (core/film/lpe.h);
    // output i is expression i. Null: none.
    std::shared_ptr<const lpe::Compiled> lpe;
    // Utility AOVs to render (core/film/film.h); null: none.
    std::shared_ptr<const std::vector<UtilityRequest>> utility;
    // Cryptomattes to render (core/film/crypto.h); null: none.
    std::shared_ptr<const std::vector<CryptoRequest>> crypto;

    bool operator==(const RenderSettings &) const = default;
};

} // namespace nova
