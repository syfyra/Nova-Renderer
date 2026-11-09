#pragma once

// Owen-scrambled, shuffled Sobol sampling after Burley, "Practical Hash-based
// Owen Scrambling" (JCGT 2020), with Vegdahl's improved Laine-Karras hash
// ("Building a Better LK Hash", 2021) as used by pbrt-v4's FastOwenScrambler
// and Cycles.
//
// Every sampling decision of a path draws from its own Sobol stream
// ("padding"): the stream id selects independent index shuffles and value
// scrambles, so decisions are decorrelated while each one stays well
// stratified over the samples of a pixel. Coupled decisions (pick a light and
// a point on it, pick a lobe and a direction) share one 3D stream so they are
// stratified jointly. Everything is stateless: a sample is a function of
// (pixel seed, sample index, stream), which keeps progressive, crop and tile
// renders deterministic and maps directly to GPU code.
//
// The index shuffle maps every aligned block of 2^k sample indices onto an
// aligned block of the Sobol sequence, so every progressive prefix of 2^k
// samples is a (0,k,2)-net per pixel and stream.

#include "core/math/vec.h"

namespace nova {

NOVA_INLINE uint32_t reverseBits(uint32_t v) {
#if defined(__CUDA_ARCH__)
    return __brev(v);
#else
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
#endif
}

// 32-bit integer hash (Wellons "lowbias32").
NOVA_INLINE uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

NOVA_INLINE uint32_t hashCombine(uint32_t a, uint32_t b) { return mix32(a ^ (mix32(b) + 0x9e3779b9u)); }

// Laine-Karras style hash: every output bit depends only on the input bits
// below it, so on bit-reversed values it is a nested uniform scramble.
NOVA_INLINE uint32_t lkHash(uint32_t v, uint32_t seed) {
    v ^= v * 0x3d20adeau;
    v += seed;
    v *= (seed >> 16) | 1u;
    v ^= v * 0x05526c56u;
    v ^= v * 0x53a22864u;
    return v;
}

// Nested uniform (Owen) scramble of all 32 bits, most significant first.
NOVA_INLINE uint32_t owenScramble(uint32_t v, uint32_t seed) { return reverseBits(lkHash(reverseBits(v), seed)); }

// The first three Sobol dimensions (Joe-Kuo direction numbers, natural
// index order). Dimensions 0 and 1 form a (0,2)-sequence; with dimension 2
// the first 2^k points form a (1,k,3)-net.
NOVA_INLINE uint32_t sobolDim0(uint32_t index) { return reverseBits(index); }
NOVA_INLINE uint32_t sobolDim1(uint32_t index) { // Pascal matrix mod 2
    uint32_t x = 0, v = 1u << 31;
    for (; index; index >>= 1, v ^= v >> 1)
        if (index & 1u)
            x ^= v;
    return x;
}
NOVA_INLINE uint32_t sobolDim2(uint32_t index) { // polynomial x^2 + x + 1, m = {1, 3}
    uint32_t x = 0, a = 0x80000000u, b = 0xC0000000u;
    for (; index; index >>= 1) {
        x ^= a & (0u - (index & 1u));
        const uint32_t c = b ^ a ^ (a >> 2);
        a = b;
        b = c;
    }
    return x;
}

// reverseBits(sobolDim1(i)) without the loop: by Lucas' theorem the Pascal
// matrix mod 2 is a subset sum over the bits.
NOVA_INLINE uint32_t sobolDim1Reversed(uint32_t i) {
    i ^= (i >> 1) & 0x55555555u;
    i ^= (i >> 2) & 0x33333333u;
    i ^= (i >> 4) & 0x0F0F0F0Fu;
    i ^= (i >> 8) & 0x00FF00FFu;
    i ^= (i >> 16) & 0x0000FFFFu;
    return i;
}

// [0, 1) with 24 bits of precision; never returns 1 (but may return 0).
NOVA_INLINE float unitFloat(uint32_t x) { return float(x >> 8) * (1.0f / 16777216.0f); }

struct Sampler {
    uint32_t pixelSeed; // hash of the image pixel and the render seed
    uint32_t index;     // sample index within the pixel

    // Pixels are hashed by both coordinates, so pixels outside the nominal
    // image (overscan) never share a sequence with pixels inside it.
    NOVA_INLINE static Sampler make(int32_t x, int32_t y, uint32_t sampleIndex, uint32_t seed = 0) {
        return Sampler{hashCombine(hashCombine(uint32_t(x), uint32_t(y)), seed * 0x68bc21ebu + 0x02e5be93u),
                       sampleIndex};
    }

    NOVA_FORCEINLINE float get1D(uint32_t stream) const {
        const uint32_t s = hashCombine(pixelSeed, stream);
        const uint32_t i = owenScramble(index, s); // shuffle the sequence order
        return unitFloat(owenScramble(sobolDim0(i), mix32(s ^ 0x5bd1e995u)));
    }

    // Equal to get3D(stream).x/.y, computed with three bit reversals less.
    NOVA_FORCEINLINE Vec2f get2D(uint32_t stream) const {
        const uint32_t s = hashCombine(pixelSeed, stream);
        const uint32_t i = reverseBits(lkHash(reverseBits(index), s));
        return {unitFloat(reverseBits(lkHash(i, mix32(s ^ 0x5bd1e995u)))),
                unitFloat(reverseBits(lkHash(sobolDim1Reversed(i), mix32(s ^ 0x1b873593u))))};
    }

    // (x, y) as get2D; z from Sobol dimension 2. A 1D choice in z (light or
    // lobe selection) stays stratified jointly with the 2D sample in (x, y).
    NOVA_FORCEINLINE Vec3f get3D(uint32_t stream) const {
        const Vec2f xy = get2D(stream);
        const uint32_t s = hashCombine(pixelSeed, stream);
        const uint32_t i = owenScramble(index, s);
        return {xy.x, xy.y, unitFloat(owenScramble(sobolDim2(i), mix32(s ^ 0xcc9e2d51u)))};
    }
};

// Stream ids of the sampling decisions along a path. Every bounce has a fixed
// block of streams, so a decision always uses the same stream regardless of
// what other paths of the pixel did. Ids are only hash inputs; unused slots
// are reserved so later features do not reshuffle existing noise.
namespace stream {
constexpr uint32_t kPixelFilter = 0; // 2D
constexpr uint32_t kLens = 1;        // 2D
constexpr uint32_t kTime = 2;        // 1D
constexpr uint32_t kWavelength = 3;  // 1D, reserved (spectral)
constexpr uint32_t kLimit = 4;       // 1D: the path's dither of fractional ray limits (M4.1)
constexpr uint32_t kBounceBase = 8;
constexpr uint32_t kPerBounce = 8;
constexpr uint32_t kLight = 0;    // 3D: xy position on the light, z light selection
constexpr uint32_t kBsdf = 1;     // 3D: xy direction, z lobe selection (M3)
constexpr uint32_t kRoulette = 2; // 1D
constexpr uint32_t kOpacity = 3;  // 1D, reserved: stochastic opacity (M3)
constexpr uint32_t kLayer = 4;    // reserved: layered BSDFs (M3)
constexpr uint32_t kMedium = 5;   // reserved: volumes (M8)
constexpr uint32_t kWalk = 6;     // reserved: SSS random walks (M9)
constexpr uint32_t kTexture = 7; // 1D: stochastic texture filtering (M3.9)

NOVA_INLINE constexpr uint32_t bounce(uint32_t depth, uint32_t decision) {
    return kBounceBase + depth * kPerBounce + decision;
}
// Ray queries of shading programs (M4.4): one 3D sample per hit, a block of
// its own above the bounces' (the per-bounce slots are taken).
constexpr uint32_t kShadingQueryBase = 0x10000u;
NOVA_INLINE constexpr uint32_t shadingQuery(uint32_t depth) { return kShadingQueryBase + depth; }
// Light path expressions (M6.2): the class choice of a continuation whose
// lobes fall into several LPE classes, one 1D sample per bounce.
constexpr uint32_t kLpeSplitBase = 0x20000u;
NOVA_INLINE constexpr uint32_t lpeSplit(uint32_t depth) { return kLpeSplitBase + depth; }
// The ambient occlusion AOV's ray at the camera hit (M6.4), 2D.
constexpr uint32_t kUtilityAo = 0x30000u;
// Transmittance of shadow rays through volumes (docs/design/M8.md D18): the
// first uniform per depth; the rest of the tracking draws from an Rng
// seeded with the same stream id.
constexpr uint32_t kMediumShadowBase = 0x40000u;
NOVA_INLINE constexpr uint32_t mediumShadow(uint32_t depth) { return kMediumShadowBase + depth; }
// The transmittance a continuation gathers lights with where volumes depend
// on the ray level (wavefront.cpp traceMedia), the same way.
constexpr uint32_t kMediumLightBase = 0x50000u;
NOVA_INLINE constexpr uint32_t mediumLight(uint32_t depth) { return kMediumLightBase + depth; }
// The light sample at a subsurface walk's exit (docs/design/M9.md): kLight of
// the depth may already serve the entry's light sample. The walk itself takes
// bounce(depth, kWalk) (3D: entry direction, first flight), then an Rng
// seeded with that stream id.
constexpr uint32_t kWalkLightBase = 0x60000u;
NOVA_INLINE constexpr uint32_t walkLight(uint32_t depth) { return kWalkLightBase + depth; }
// Equiangular sampling along a ray in volumes (docs/design/M9.md D17): the
// light, the distance, the light sample and the transmittance to the point,
// all from an Rng seeded with this stream id (the M8 D18 scheme).
constexpr uint32_t kEquiangularBase = 0x70000u;
NOVA_INLINE constexpr uint32_t equiangular(uint32_t depth) { return kEquiangularBase + depth; }
// The choice among several shadow-linked domes for a continuation's own
// shadow ray (docs/design/M9.md D15, core/integrator/path.h), 1D per depth.
constexpr uint32_t kShadowDomeBase = 0x80000u;
NOVA_INLINE constexpr uint32_t shadowDome(uint32_t depth) { return kShadowDomeBase + depth; }
// A pre-lit background holdout's light candidates (docs/design/M11-P2.md,
// core/integrator/holdout.h): an Rng seeded with this stream id per depth.
constexpr uint32_t kPrelitBase = 0x90000u;
NOVA_INLINE constexpr uint32_t prelit(uint32_t depth) { return kPrelitBase + depth; }
// Karma's outlines (docs/design/M11-P9a.md, core/integrator/outline.h): the
// stencil's rotation per ring, or a random pattern's points, from an Rng
// seeded with this stream id plus the ring or point.
constexpr uint32_t kOutlineBase = 0xA0000u;
NOVA_INLINE constexpr uint32_t outline(uint32_t i) { return kOutlineBase + i; }
// The simplified shading's ambient occlusion ray at the camera hit (Karma's
// disable_lighting, core/integrator/lookdev.h), 2D.
constexpr uint32_t kLookAo = 0xB0000u;
} // namespace stream

} // namespace nova
