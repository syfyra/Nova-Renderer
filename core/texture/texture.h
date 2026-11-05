#pragma once

// Texture lookups as the shading VM sees them (docs/design/M3.md K9,
// docs/research/m3/textures.md 6.1): a texture is an id plus a POD sampler
// state; the host implements ITextureSampler (Hydra: OIIO's TextureSystem on
// converted, linear, tiled and MIP-mapped files; tests: MemorySampler). No
// OIIO types here, so kernels can include it.
//
// Coordinates are MaterialX/USD uv: v points up, UDIM rows lie above v = 1.
// Derivatives are full-diameter footprint vectors in uv (OIIO's convention);
// all zero selects the finest MIP level.

#include "core/math/vec.h"

#include <cstdint>
#include <vector>

namespace nova {

using TextureId = uint32_t;
constexpr TextureId kNoTexture = ~0u;

// Address modes. Constant returns the fallback outside [0, 1] (MaterialX
// "constant"); Black returns 0 there.
enum class TexWrap : uint8_t { Periodic, Clamp, Mirror, Black, Constant };
enum class TexFilter : uint8_t { Closest, Linear, Cubic };

// The structural part of a texture lookup (part of the program; the texture
// itself is a value).
struct TexSlot {
    TexWrap wrapS = TexWrap::Periodic;
    TexWrap wrapT = TexWrap::Periodic;
    TexFilter filter = TexFilter::Linear;
    uint8_t channels = 3; // 1..4

    bool operator==(const TexSlot &o) const {
        return wrapS == o.wrapS && wrapT == o.wrapT && filter == o.filter && channels == o.channels;
    }
};

// One slot of a material: the slot's state and the texture bound to it.
struct TexSampler {
    TextureId texture = kNoTexture;
    TexSlot slot;
};

// One lookup over `count` lanes (SoA).
struct TexQuery {
    uint32_t count = 0;
    const float *s = nullptr, *t = nullptr;
    const float *dsdx = nullptr, *dtdx = nullptr, *dsdy = nullptr, *dtdy = nullptr; // null: 0
    const float *rnd = nullptr; // stochastic filtering sample in [0, 1), or null
};

// A texture's data for a device (docs/design/M10.md D10): its tiles (one, or
// a UDIM set's), each a MIP chain from the finest level, as RGBA floats per
// texel (a grey image replicated, a missing alpha 1, a color transform
// applied), rows from the top (v = 1) down.
struct TexLevel {
    int width = 0, height = 0;
    std::vector<float> texels;
};
struct TexTile {
    uint32_t udim = 0; // 1001... in a UDIM set, else 0
    std::vector<TexLevel> levels;
};
struct TexData {
    bool udim = false;
    bool half = true; // 16-bit floats keep it (the source has at most 16 bits per channel)
    std::vector<TexTile> tiles;
};

class ITextureSampler {
public:
    virtual ~ITextureSampler() = default;
    // The texture's data for a device; false if there is none (a missing
    // file). Not while rendering.
    virtual bool textureData(TextureId id, TexData &out) const {
        (void)id;
        (void)out;
        return false;
    }
    // Changes whenever the host replaces the texture's data under its id
    // (Hydra: the file changed on disk and was prepared again), so a device
    // copy taken from textureData at another generation is stale. Not while
    // rendering.
    virtual uint32_t textureGeneration(TextureId id) const {
        (void)id;
        return 0;
    }
    // out[c * count + i] for c < slot.channels; status[i] = 0 when the lane
    // was sampled, 1 when the caller must use the fallback (missing file or
    // UDIM tile, outside a Constant address range, read error).
    virtual void sample2D(const TexSampler &sampler, const TexQuery &query, float *out, uint8_t *status) const = 0;
};

// Context of the shade stage's texture instructions: the sampler and the
// material's slots.
struct QueryLane;
class IShadingQueries;

struct DeviceTexSlot;

struct TextureContext {
    const ITextureSampler *sampler = nullptr;
    const TexSampler *samplers = nullptr;
    uint32_t count = 0;
    const float *rnd = nullptr; // per lane: stochastic filtering sample (decorrelated per slot), or null
    // Ray queries (core/shading/query.h): per-lane hit data and the
    // integrator's implementation; null: the queries' defaults.
    const IShadingQueries *queries = nullptr;
    const QueryLane *lanes = nullptr;
    // On the GPU (docs/design/M10.md D10): the slots' device textures, in
    // place of sampler (core/gpu/device_texture.h).
    const DeviceTexSlot *device = nullptr;
};

} // namespace nova
