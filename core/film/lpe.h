#pragma once

// Light path expressions at render time (docs/design/M6.md D2-D4). All bound
// expressions are compiled into one minimized automaton over path events
// (core/film/lpe_compile.h); a path carries one state and every event is one
// table lookup. An event is (type, scatter, tag, label): the event type
// (camera, reflection, ...), the scattering kind, the object's or light's
// LPE tag and the BSDF component's label, the last two as program ids
// (0: untagged/unlabelled or named in no expression).

#include "core/math/common.h" // NOVA_INLINE

#include <cstdint>

namespace nova::lpe {

enum Type : uint8_t {
    TypeCamera,     // C
    TypeReflect,    // R
    TypeTransmit,   // T
    TypeVolume,     // V
    TypeLight,      // L
    TypeObject,     // O, emission of an object that is not a light
    TypeBackground, // B, a ray that left the scene
    TypeAlbedo,     // A, the albedo of the preceding scattering event
    // Internal (not in the expression syntax): a holdout path, entered before
    // the camera event (docs/design/M11-P2.md). holdout; outputs start with
    // it, no other expression accepts it.
    TypeHoldout,
    kTypeCount
};

enum Scatter : uint8_t {
    ScatterDiffuse,  // D
    ScatterGlossy,   // G (Karma: every specular event too)
    ScatterSingular, // S (OSL; Karma emits none)
    ScatterStraight, // s (OSL; Karma emits none)
    ScatterNone,     // x: camera, lights, objects, background, albedo
    kScatterCount
};

// Output flags (Karma's prefixes and negation).
enum OutputFlag : uint8_t {
    OutputUnoccluded = 1, // unoccluded;  light before shadowing
    OutputShadow = 2,     // shadow;      the occluded fraction
    OutputHoldout = 4,    // holdout;     light of paths whose camera ray hit a holdout
    OutputInvert = 8,     // !            1 - value
};

using State = uint16_t; // 0: dead (no event leads to an output any more)

struct Program {
    uint32_t stateCount = 0;
    uint32_t classCount = 0;
    uint32_t tagCount = 1;      // tag ids 0..tagCount-1
    uint32_t labelCount = 1;    // label ids 0..labelCount-1
    const State *next = nullptr;          // [state * classCount + class]
    const uint16_t *symbolClass = nullptr; // [symbol] -> class
    const uint32_t *outBegin = nullptr;   // [state] .. [state + 1] into outs
    const uint16_t *outs = nullptr;       // output ids per state
    const uint8_t *outputFlags = nullptr; // per output
    State start = 0;                      // after the camera event
    // A path whose camera ray hit a holdout continues from here (after the
    // holdout and camera events; 0 without holdout; outputs).
    State holdoutStart = 0;

    NOVA_INLINE uint32_t symbol(Type type, Scatter scatter, uint32_t tag, uint32_t label) const {
        return ((uint32_t(type) * kScatterCount + scatter) * tagCount + tag) * labelCount + label;
    }
    NOVA_INLINE State step(State s, uint32_t symbol) const { return next[s * classCount + symbolClass[symbol]]; }
    NOVA_INLINE uint32_t outputCount(State s) const { return outBegin[s + 1] - outBegin[s]; }
    NOVA_INLINE const uint16_t *outputs(State s) const { return outs + outBegin[s]; }
};

} // namespace nova::lpe
