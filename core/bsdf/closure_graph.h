#pragma once

// The structure of a generic MaterialX surface (docs/design/M4.md 3.4,
// docs/research/m4/bsdf-graphs.md 5): pbrlib BSDF graphs (layer, mix, add,
// multiply over the leaves), their EDFs, and mixes of whole surface shaders
// (mix_surfaceshader) as weighted segments, each a native model or a graph.
// Fixed-size POD owned by the material's shading program; every number it
// refers to lives in the material's SurfaceInputs block at the given offsets
// (the program computes them per hit). The closure builder
// (core/bsdf/graph_closure.h) walks it per hit without recursion.

#include <cstdint>

namespace nova {

enum class SurfaceModel : uint8_t;

constexpr uint16_t kNoSlot = 0xFFFF;

enum class GraphOp : uint8_t {
    Leaf,
    Mix,      // a = fg, b = bg, param = mix amount (1)
    Multiply, // a = in, param = weight (3, >= 0)
    Add,      // a, b
    Layer,    // a = top, b = base
    LayerVdf, // a = top over a medium, param = absorption (3)
};

// pbrlib leaves. Parameter records at GraphNode::param (floats):
//   Diffuse, Burley, Sheen: weight, color[3], roughness           (5)
//   Translucent: weight, color[3]                                  (4)
//   Subsurface: weight, color[3], radius[3], anisotropy             (8)
//   ChiangHair: weight, tint R/TT/TRT[3 each], ior, roughness (v, s) R/TT/TRT[2 each],
//               cuticle angle, absorption[3]                       (21)
//   Dielectric: weight, tint[3], ior, alpha[2], film thickness, film ior (9)
//   Conductor: weight, n[3], k[3], alpha[2], film thickness, film ior (11)
//   Schlick: weight, color0[3], color82[3], color90[3], exponent, alpha[2],
//            film thickness, film ior                              (15)
enum class LeafKind : uint8_t { Diffuse, Burley, Translucent, Subsurface, Dielectric, Conductor, Schlick, Sheen, ChiangHair };

enum GraphLeafFlags : uint8_t {
    kLeafReflect = 1u << 0,         // scatter mode R (dielectric, Schlick); RT = both bits
    kLeafTransmit = 1u << 1,        // scatter mode T
    kLeafEnergyComp = 1u << 2,      // oren_nayar energy_compensation (EON)
    kLeafZeltner = 1u << 3,         // sheen mode zeltner (else conty_kulla: Charlie)
    kLeafInTop = 1u << 4,           // inside a layer's top: its albedo matters
    kLeafOverRefraction = 1u << 5,  // a reflection in a top whose base refracts (dropped from inside)
};

struct GraphNode {
    GraphOp op;
    LeafKind kind;
    uint8_t flags;  // GraphLeafFlags
    uint8_t frame;  // leaves: index into ClosureGraph::frame
    uint16_t a, b;  // children (post-order: before this node)
    uint16_t param; // block offset (see GraphOp and LeafKind)
};

// An emission term: color (3, all linear factors applied), optionally
// scaled by generalized_schlick_edf's mix(color0, color90, (1 - mu)^e) at
// schlick (color0[3], color90[3], exponent).
struct GraphEmission {
    uint16_t color;
    uint16_t schlick; // kNoSlot: none
};

// A shading frame of the graph's leaves: block offsets of the normal and
// tangent (3 each; kNoSlot: the geometry's).
struct GraphFrame {
    uint16_t normal;
    uint16_t tangent;
};

struct GraphSegment {
    SurfaceModel model;          // a native model, or SurfaceModel::Graph
    uint8_t root;                // Graph: the root node (kNoNode: no BSDF)
    uint8_t emissionBegin, emissionEnd;
    uint16_t base;               // native: block offset of its SurfaceInputs layout
    uint16_t weight;             // block offset of the segment's weight (1 float); kNoSlot: 1
    uint16_t thinWalled;         // Graph: block offset (1 float); kNoSlot: not thin-walled
};

struct ClosureGraph {
    static constexpr int kMaxNodes = 48, kMaxEmission = 8, kMaxFrames = 4, kMaxSegments = 4;
    static constexpr uint8_t kNoNode = 0xFF;

    uint8_t nodeCount = 0, emissionCount = 0, frameCount = 0, segmentCount = 0;
    bool refractive = false; // some graph leaf refracts (T or RT, not thin-walled)
    uint8_t reserved = 0;    // no padding: the program's structure hash reads the bytes
    GraphNode node[kMaxNodes] = {};
    GraphEmission emission[kMaxEmission] = {};
    GraphFrame frame[kMaxFrames] = {};
    GraphSegment segment[kMaxSegments] = {};
};

static_assert(sizeof(GraphNode) == 10 && sizeof(GraphSegment) == 10, "graph records must have no padding");

// Derives the structural flags from the nodes (post-order, at most 48, so a
// node's descendants fit in a 64-bit mask): kLeafInTop for leaves below a
// layer's top, kLeafOverRefraction for reflection-only leaves in a top whose
// base refracts, and `refractive`.
inline void finalizeClosureGraph(ClosureGraph &g) {
    uint64_t below[ClosureGraph::kMaxNodes] = {};
    uint64_t transmitting = 0, inTop = 0, overRefraction = 0;
    for (int i = 0; i < g.nodeCount; ++i) {
        GraphNode &n = g.node[i];
        below[i] = uint64_t(1) << i;
        if (n.op == GraphOp::Leaf) {
            n.flags &= uint8_t(~(kLeafInTop | kLeafOverRefraction));
            const bool refracting = (n.kind == LeafKind::Dielectric || n.kind == LeafKind::Schlick) &&
                                    (n.flags & kLeafTransmit);
            if (refracting)
                transmitting |= below[i];
            continue;
        }
        below[i] |= below[n.a];
        if (n.op == GraphOp::Mix || n.op == GraphOp::Add || n.op == GraphOp::Layer)
            below[i] |= below[n.b];
        if (n.op == GraphOp::Layer || n.op == GraphOp::LayerVdf) {
            inTop |= below[n.a];
            if (n.op == GraphOp::Layer && (below[n.b] & transmitting))
                overRefraction |= below[n.a];
        }
    }
    g.refractive = transmitting != 0;
    for (int i = 0; i < g.nodeCount; ++i) {
        GraphNode &n = g.node[i];
        if (n.op != GraphOp::Leaf)
            continue;
        if (inTop & (uint64_t(1) << i))
            n.flags |= kLeafInTop;
        const bool reflectOnly = (n.flags & (kLeafReflect | kLeafTransmit)) == kLeafReflect;
        if (reflectOnly && (overRefraction & (uint64_t(1) << i)))
            n.flags |= kLeafOverRefraction;
    }
}

} // namespace nova
