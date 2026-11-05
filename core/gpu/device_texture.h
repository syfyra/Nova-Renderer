#pragma once

// Textures on the GPU (docs/design/M10.md D10): the texture objects of a
// material's slots, made by core/gpu/gpu_renderer.cpp from the host's MIP
// chains (ITextureSampler::textureData), and the lookup the VM's Tex2D does
// with them in place of the host sampler. Hardware filtering: trilinear
// across the MIP levels with up to 8x anisotropy (the host's OIIO filters
// stochastically; the GPU matches it statistically, D11).

#include "core/math/common.h"
#include "core/texture/texture.h"

#include <cmath>

namespace nova {

constexpr int kUdimTiles = 100; // tiles 1001 to 1100

struct DeviceTexSlot {
    unsigned long long object = 0;             // the texture (0: none or missing)
    const unsigned long long *tiles = nullptr; // a UDIM set: kUdimTiles textures (0: a missing tile)
    TexSlot slot;
};

#if defined(__CUDACC__)
// RGBA of the slot's texture at (s, t), v up, with footprint derivatives
// d = (dsdx, dtdx, dsdy, dtdy) (null: the finest level). False where the
// caller must use the fallback (as ITextureSampler::sample2D's status).
__device__ inline bool sampleDeviceTexture(const DeviceTexSlot &d, float s, float t, const float *dv, float out[4]) {
    const auto outside = [](float x) { return !(x >= 0.0f && x <= 1.0f); };
    if ((d.slot.wrapS == TexWrap::Constant && outside(s)) || (d.slot.wrapT == TexWrap::Constant && outside(t)))
        return false;
    unsigned long long object = d.object;
    if (d.tiles) { // UDIM: tile 1001 + floor(u) + 10 floor(v), clamped inside it
        // Bounded in float before the int conversion: a huge or infinite t
        // would saturate or overflow it into a negative tile index.
        if (!(s >= 0.0f && s < 10.0f && t >= 0.0f && t < 10.0f))
            return false;
        const float u = std::floor(s), v = std::floor(t);
        const int tile = int(u) + 10 * int(v);
        if (tile >= kUdimTiles)
            return false;
        object = d.tiles[tile];
        s -= u;
        t -= v;
    }
    if (!object)
        return false;
    // CUDA's rows run down from the top: v = 1 - t.
    const float4 r = dv ? tex2DGrad<float4>(object, s, 1.0f - t, make_float2(dv[0], -dv[1]), make_float2(dv[2], -dv[3]))
                        : tex2DLod<float4>(object, s, 1.0f - t, 0.0f);
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
    out[3] = r.w;
    return true;
}
#endif

} // namespace nova
