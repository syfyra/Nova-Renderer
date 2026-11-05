#pragma once

// Shading IR (docs/design/M3.md section 3): a typed DAG of operations whose
// outputs bind to surface inputs (SurfaceInputs float offsets). Every value
// is 1 to 4 floats wide (integers and booleans are floats). Nodes reference
// only earlier nodes, so the node order is a topological order.
//
// Leaves:
//   Const    values from the constant pool (structure: part of the hash);
//   Param    a material parameter (a value: never folded, editable without
//            recompiling);
//   Global   a shading global (P, N, ...), filled per hit;
//   Primvar  a named mesh or instance attribute with a uniform fallback.

#include "core/bsdf/surface.h"
#include "core/shading/ops.h"
#include "core/shading/query.h"
#include "core/texture/texture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nova::shading {

using Ref = uint32_t;
constexpr Ref kNoRef = ~0u;

// Shading globals (world space unless noted). N is the interpolated shading
// normal, not face-forwarded (MaterialX semantics); Ng the geometric normal
// in authored orientation; I the incident ray direction (towards the
// surface). Pobject/Nobject are in the mesh's own (prototype) space. Frame
// is the frame number, Time Karma's (frame - 1) / fps. ObjX/Y/Z/T: the
// object-to-world transform at the hit (the images of the object axes and
// of the origin), for MaterialX space transforms. PDx/PDy and
// PobjectDx/PobjectDy are P and Pobject moved by the hit's two texture
// footprint vectors (the compiler's derivative clones read them, docs/
// design/M3.md K7). DPdu/DPdv: the world-space derivatives of P along the
// graph's tangent uv set (Karma's tangent frame source; without that set,
// the first triangle edge). Ray state (docs/design/M4.md 3.1): RayType is the
// class of the event that spawned the ray (core/bsdf/closure.h RayClass,
// Karma's numbering: 0 camera ... 6 shadow), RayDepth the scattering events
// before the hit (Karma's ray:level), HitDist the length of the ray segment
// to the hit, Backface 1 if the ray hit the side the geometric normal points
// away from.
enum class Global : uint8_t {
    P, Pobject, N, Ng, Nobject, I, Time, PrimId, InstanceId, Frame, ObjX, ObjY, ObjZ, ObjT,
    PDx, PDy, PobjectDx, PobjectDy, DPdu, DPdv,
    RayType, RayDepth, HitDist, Backface,
    Count
};

// Primvar nodes: imm = primvar index | shift << 24 (0: at the hit, 1 and 2:
// at the hit moved by the first or second footprint vector).
constexpr uint32_t kPrimvarIndexMask = 0xFFFFFFu;
constexpr int kPrimvarShiftBit = 24;

NOVA_INLINE int globalWidth(Global g) {
    switch (g) {
    case Global::Time:
    case Global::PrimId:
    case Global::InstanceId:
    case Global::Frame:
    case Global::RayType:
    case Global::RayDepth:
    case Global::HitDist:
    case Global::Backface:
        return 1;
    default:
        return 3;
    }
}

struct IrNode {
    Op op = Op::Const;
    uint8_t width = 1;
    Ref a = kNoRef, b = kNoRef, c = kNoRef;
    uint32_t imm = 0; // Const: pool offset; Param: parameter offset; Global: id; Primvar: index; ops: see ops.h
};

struct IrOutput {
    uint16_t field = 0; // SurfaceInputs float offset (surface_field::...)
    uint8_t width = 1;
    Ref value = kNoRef;
};

struct IrGraph {
    std::vector<IrNode> nodes;
    std::vector<float> constants;
    std::vector<std::string> primvars; // attribute names, by Primvar index
    uint32_t paramCount = 0;           // floats
    std::vector<IrOutput> outputs;
    SurfaceModel model = SurfaceModel::Lambert;
    std::vector<TexSlot> textures; // texture slots, by Tex2D imm (the files are material values)
    std::string tangentUv = "st";  // the primvar DPdu/DPdv derive from
    // Shader AOVs (kma_aov, docs/design/M6.md D9): name -> the field of the
    // input block that holds the value (3 floats, an output like the others).
    std::vector<std::pair<std::string, uint16_t>> aovs;
    // SurfaceModel::Graph: the closure structure over the outputs' fields.
    std::shared_ptr<const ClosureGraph> closure;
};

// Appends nodes with width checks: operands of per-component operations are
// either 1 wide (splat) or as wide as the result.
class IrBuilder {
public:
    explicit IrBuilder(IrGraph &graph) : m_g(graph) {}

    Ref constant(const float *values, int width);
    Ref constant(float v) { return constant(&v, 1); }
    Ref constant(float x, float y, float z) {
        const float v[3] = {x, y, z};
        return constant(v, 3);
    }
    Ref param(uint32_t offset, int width);
    // A new parameter at the end of the block; returns its node.
    Ref newParam(int width, uint32_t *offset = nullptr);
    Ref global(Global g);
    Ref primvar(const std::string &name, int width, Ref fallback);

    Ref unary(Op op, Ref a);
    Ref binary(Op op, Ref a, Ref b);
    Ref ternary(Op op, Ref a, Ref b, Ref c);
    Ref dot(Ref a, Ref b);
    Ref length(Ref a);
    Ref distance(Ref a, Ref b);
    Ref normalize(Ref a) { return node(Op::Normalize, width(a), a); }
    Ref cross(Ref a, Ref b);
    Ref swizzle(Ref a, int c0, int c1 = -1, int c2 = -1, int c3 = -1);
    Ref component(Ref a, int k) { return swizzle(a, k); }
    Ref concat(Ref a, Ref b);
    // Procedurals (ops.h): position p (2 or 3 wide), result `width`.
    Ref noise(Ref p, int width) { return node(Op::Noise, width, p, kNoRef, kNoRef, noiseImm(this->width(p), width)); }
    Ref fractal(Ref p, Ref octaves, Ref lacunarity, Ref diminish, int width) {
        return node(Op::Fractal, width, p, convert(octaves, 1), concat(convert(lacunarity, 1), convert(diminish, 1)),
                    noiseImm(this->width(p), width));
    }
    Ref cellNoise(Ref p, int width) {
        return node(Op::CellNoise, width, p, kNoRef, kNoRef, noiseImm(this->width(p), width));
    }
    Ref worley(Ref p, Ref jitter, int style, int width) {
        return node(Op::Worley, width, p, convert(jitter, 1), kNoRef, noiseImm(this->width(p), width, style));
    }
    Ref rgbToHsv(Ref c) { return node(Op::RgbToHsv, 3, convert(c, 3)); }
    Ref hsvToRgb(Ref c) { return node(Op::HsvToRgb, 3, convert(c, 3)); }
    // Voronoi at p (2 or 3 wide): part 0 the distances (width 1-3), 1-3 the
    // nearest seed points (3 wide).
    Ref voronoi(Ref p, Ref jitter, int metric, int part, int width) {
        return node(Op::Voronoi, width, p, convert(jitter, 1), kNoRef,
                    noiseImm(this->width(p), width, metric) | uint32_t(part) << 12);
    }
    // MaterialX flakes at p (flake units): (priority, xi0, xi1, rand).
    Ref flake(Ref p, Ref coverage) { return node(Op::Flake, 4, convert(p, 3), convert(coverage, 1)); }
    // A ray query (core/shading/query.h): direction (3 wide; zero: the
    // shading normal), parameters (4 wide).
    Ref query(QueryKind kind, Ref direction, Ref params) {
        return node(Op::Query, queryWidth(kind), convert(direction, 3), convert(params, 4), kNoRef, uint32_t(kind));
    }
    // Derivatives of a (1 wide) along the two footprint vectors (2 wide).
    Ref deriv(Ref a) { return node(Op::Deriv, 2, convert(a, 1)); }
    // A new texture slot (the caller shares slots of the same texture and
    // state) and a lookup in it: uv (2 wide), fallback (converted to the
    // slot's channel count).
    uint32_t textureSlot(const TexSlot &slot);
    Ref tex2D(uint32_t slot, Ref uv, Ref fallback) {
        const int w = m_g.textures[slot].channels;
        return node(Op::Tex2D, w, convert(uv, 2), kNoRef, convert(fallback, w), slot);
    }
    // A lookup with explicit footprint derivatives (dsdx, dtdx, dsdy, dtdy)
    // instead of the compiler's (hex tiling filters with the unrotated ones).
    Ref tex2D(uint32_t slot, Ref uv, Ref derivatives, Ref fallback) {
        const int w = m_g.textures[slot].channels;
        return node(Op::Tex2D, w, convert(uv, 2), convert(derivatives, 4), convert(fallback, w), slot);
    }
    // Value of `width` components: a splat, a truncation or zero padding.
    Ref convert(Ref a, int width);

    Ref add(Ref a, Ref b) { return binary(Op::Add, a, b); }
    Ref sub(Ref a, Ref b) { return binary(Op::Sub, a, b); }
    Ref mul(Ref a, Ref b) { return binary(Op::Mul, a, b); }
    Ref div(Ref a, Ref b) { return binary(Op::Div, a, b); }

    void output(uint16_t field, int width, Ref value);

    int width(Ref r) const { return m_g.nodes[r].width; }
    const IrGraph &graph() const { return m_g; }

    Ref node(Op op, int width, Ref a = kNoRef, Ref b = kNoRef, Ref c = kNoRef, uint32_t imm = 0);

private:
    IrGraph &m_g;
};

// One operation on operand values (each of its width; operands of width 1
// splat in per-component operations). The compiler folds constants with it.
void evaluateOp(const IrNode &n, const float *a, int wa, const float *b, int wb, const float *c, int wc,
                float *out);

// Reference evaluator for tests: evaluates the graph for one shading point
// by walking the nodes in order (no optimization, no registers). `globals`
// holds globalWidth(g) floats per Global; `primvars` the value of each
// primvar (its width), or nullptr to use the fallback.
void evaluateGraph(const IrGraph &g, const float *params, const float *const *globals,
                   const float *const *primvars, std::vector<float> &nodeValues);

} // namespace nova::shading
