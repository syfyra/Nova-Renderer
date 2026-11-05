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

// A texture's data for a device (docs/design/M10.md D10), in two steps so
// that the device's texture budget picks the MIP levels before anything is
// read (core/texture/texture_budget.h): textureInfo gives the shape of the
// texture's tiles (one, or a UDIM set's), textureLevels then reads chosen
// levels of one tile, a band of rows at a time.
//
// Level L of a tile whose finest level is w x h texels has max(1, w >> L) x
// max(1, h >> L) texels. The host stores levels 0 to stored - 1 (a .tx file
// all of them, a plain image only level 0); a coarser level is made from the
// last stored one with a box filter while it is read. Texels are RGBA (a grey
// image replicated, a missing alpha 1, a color transform applied), rows from
// the top (v = 1) down.
struct TexTileInfo {
    uint32_t udim = 0;         // 1001... in a UDIM set, else 0
    int width = 0, height = 0; // level 0
    uint32_t stored = 0;       // levels stored, from level 0 on (at least 1)
};
struct TexInfo {
    bool udim = false;
    bool half = true; // 16-bit floats keep it (the source has at most 16 bits per channel)
    std::vector<TexTileInfo> tiles;
};

// What textureLevels reads: `count` levels of the tile `udim` from level
// `first` on, as IEEE half floats (4 x uint16_t per texel) or floats.
struct TexLevelRequest {
    uint32_t udim = 0;
    uint32_t first = 0;
    uint32_t count = 1;
    bool half = true;
};

// A band of rows textureLevels hands over: rows [y, y + rows) of `level`
// (width x height texels), each `width` texels in the requested format. The
// levels come in order, each from its top row down; the texels are valid
// during the call only.
struct TexRows {
    uint32_t level = 0;
    int width = 0, height = 0;
    int y = 0, rows = 0;
    const void *texels = nullptr;
};
class TexRowSink {
public:
    virtual ~TexRowSink() = default;
    virtual bool put(const TexRows &rows) = 0; // false stops the read
};

class ITextureSampler {
public:
    virtual ~ITextureSampler() = default;
    // The texture's shape for a device; false if there is none (a missing
    // file). Not while rendering.
    virtual bool textureInfo(TextureId id, TexInfo &out) const {
        (void)id;
        (void)out;
        return false;
    }
    // Reads the requested levels of one tile into `sink`; false on a read
    // error, a tile or levels the texture does not have, or when the sink
    // stopped. Host memory: a band of rows (and a row of sums for a level
    // made from a finer one), never a whole level. Not while rendering.
    virtual bool textureLevels(TextureId id, const TexLevelRequest &request, TexRowSink &sink) const {
        (void)id;
        (void)request;
        (void)sink;
        return false;
    }
    // Changes whenever the host replaces the texture's data under its id
    // (Hydra: the file changed on disk and was prepared again), so a device
    // copy (and the shape) taken at another generation is stale. Not while
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
