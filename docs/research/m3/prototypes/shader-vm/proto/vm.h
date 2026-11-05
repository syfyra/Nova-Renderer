#pragma once
// Prototype of core/shading/{program.h, ops.h, vm_lane.h, vm_batch.h}.
// POD program + scalar op semantics shared by a per-lane executor
// (NOVA_HOST_DEVICE: CUDA, anyhit/opacity, displacement, tests) and a SoA
// batch executor (CPU shade stage).

#include <cmath>
#include <cstdint>

#ifndef NOVA_HOST_DEVICE
#define NOVA_HOST_DEVICE
#endif
#define NOVA_INLINE NOVA_HOST_DEVICE inline
#if defined(_MSC_VER)
#define NOVA_RESTRICT __restrict
#else
#define NOVA_RESTRICT __restrict__
#endif

namespace nova::shading {

enum class Op : uint16_t {
    End,
    // component-wise over `width` components; operands may be uniform
    // (flags bit 0..2) and/or a scalar broadcast over components (bit 3..5)
    Mov, Add, Sub, Mul, Div, Mad, Mix, Clamp, Min, Max, Floor, Fract, Abs, Sqrt, Sin, Gt, Select,
    // reductions / special shapes
    Dot,       // a.w, b.w -> 1
    Normalize, // a.w -> w
    Extract,   // a.w -> 1, imm = component
    Combine3,  // a, b, c scalars -> 3
    Count
};

constexpr uint8_t kUniformA = 1, kUniformB = 2, kUniformC = 4;
constexpr uint8_t kSplatA = 8, kSplatB = 16, kSplatC = 32;

// 16-byte instruction (one uint4, like a Cycles SVM node word).
struct Instr {
    Op op;
    uint8_t width;  // components of the result (component-wise ops)
    uint8_t flags;  // kUniform*/kSplat*
    uint16_t dst;   // varying register row (or uniform slot in the prologue)
    uint16_t a, b, c;
    uint32_t imm;
};
static_assert(sizeof(Instr) == 16);

// ---- scalar semantics (core/shading/ops.h). Branch-free selects and
// ternary min/max so MSVC vectorizes the batch loops (std::fmin/fmax and
// libm calls under /fp:precise do not vectorize).
NOVA_INLINE float opMin(float x, float y) { return x < y ? x : y; }
NOVA_INLINE float opMax(float x, float y) { return x > y ? x : y; }
NOVA_INLINE float opDiv(float x, float y) { return y != 0.0f ? x / y : 0.0f; } // MaterialX GLSL: x / y; safe variant
NOVA_INLINE float opMix(float bg, float fg, float t) { return bg * (1.0f - t) + fg * t; } // GLSL/OSL mix(); exact at t = 0 and t = 1
NOVA_INLINE float opClamp(float x, float lo, float hi) { return opMin(opMax(x, lo), hi); }
NOVA_INLINE float opFract(float x) { return x - std::floor(x); }
NOVA_INLINE float opSqrt(float x) { return std::sqrt(opMax(x, 0.0f)); }

template <Op O> struct Fn;
template <> struct Fn<Op::Mov>   { NOVA_INLINE static float eval(float x, float, float) { return x; } };
template <> struct Fn<Op::Add>   { NOVA_INLINE static float eval(float x, float y, float) { return x + y; } };
template <> struct Fn<Op::Sub>   { NOVA_INLINE static float eval(float x, float y, float) { return x - y; } };
template <> struct Fn<Op::Mul>   { NOVA_INLINE static float eval(float x, float y, float) { return x * y; } };
template <> struct Fn<Op::Div>   { NOVA_INLINE static float eval(float x, float y, float) { return opDiv(x, y); } };
template <> struct Fn<Op::Mad>   { NOVA_INLINE static float eval(float x, float y, float z) { return x * y + z; } };
template <> struct Fn<Op::Mix>   { NOVA_INLINE static float eval(float x, float y, float z) { return opMix(x, y, z); } };
template <> struct Fn<Op::Clamp> { NOVA_INLINE static float eval(float x, float y, float z) { return opClamp(x, y, z); } };
template <> struct Fn<Op::Min>   { NOVA_INLINE static float eval(float x, float y, float) { return opMin(x, y); } };
template <> struct Fn<Op::Max>   { NOVA_INLINE static float eval(float x, float y, float) { return opMax(x, y); } };
template <> struct Fn<Op::Floor> { NOVA_INLINE static float eval(float x, float, float) { return std::floor(x); } };
template <> struct Fn<Op::Fract> { NOVA_INLINE static float eval(float x, float, float) { return opFract(x); } };
template <> struct Fn<Op::Abs>   { NOVA_INLINE static float eval(float x, float, float) { return std::fabs(x); } };
template <> struct Fn<Op::Sqrt>  { NOVA_INLINE static float eval(float x, float, float) { return opSqrt(x); } };
template <> struct Fn<Op::Sin>   { NOVA_INLINE static float eval(float x, float, float) { return std::sin(x); } };
template <> struct Fn<Op::Gt>    { NOVA_INLINE static float eval(float x, float y, float) { return x > y ? 1.0f : 0.0f; } };
template <> struct Fn<Op::Select>{ NOVA_INLINE static float eval(float c, float a, float b) { return c != 0.0f ? a : b; } };

constexpr int arity(Op o) {
    switch (o) {
    case Op::Mov: case Op::Floor: case Op::Fract: case Op::Abs: case Op::Sqrt: case Op::Sin:
    case Op::Normalize: case Op::Extract: return 1;
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Min: case Op::Max: case Op::Gt:
    case Op::Dot: return 2;
    case Op::Mad: case Op::Mix: case Op::Clamp: case Op::Select: case Op::Combine3: return 3;
    default: return 0;
    }
}
constexpr bool componentWise(Op o) { return o >= Op::Mov && o <= Op::Select; }

// ---------------------------------------------------------------- per lane
// Registers of one lane: R[row]. Uniform block: U[slot]. On CUDA R lives in
// local memory (same row index across a warp -> coalesced), U in global
// memory read through the read-only cache (same address -> broadcast).
struct LaneRegs {
    float *R;
    const float *U;
    NOVA_INLINE float ld(uint16_t idx, bool uniform, int k, bool splat) const {
        const int c = splat ? 0 : k;
        return uniform ? U[idx + c] : R[idx + c];
    }
};

template <Op O>
NOVA_INLINE void laneCw(const Instr &in, LaneRegs &r) {
    for (int k = 0; k < in.width; ++k)
        r.R[in.dst + k] = Fn<O>::eval(r.ld(in.a, in.flags & kUniformA, k, in.flags & kSplatA),
                                      r.ld(in.b, in.flags & kUniformB, k, in.flags & kSplatB),
                                      r.ld(in.c, in.flags & kUniformC, k, in.flags & kSplatC));
}

// Executes code until End. In the prologue R == the (writable) uniform block.
NOVA_INLINE void runLane(const Instr *code, float *R, const float *U) {
    LaneRegs r{R, U};
    for (const Instr *ip = code;; ++ip) {
        const Instr &in = *ip;
        switch (in.op) {
        case Op::End: return;
#define NOVA_CW(o) case Op::o: laneCw<Op::o>(in, r); break;
        NOVA_CW(Mov) NOVA_CW(Add) NOVA_CW(Sub) NOVA_CW(Mul) NOVA_CW(Div) NOVA_CW(Mad) NOVA_CW(Mix)
        NOVA_CW(Clamp) NOVA_CW(Min) NOVA_CW(Max) NOVA_CW(Floor) NOVA_CW(Fract) NOVA_CW(Abs) NOVA_CW(Sqrt)
        NOVA_CW(Sin) NOVA_CW(Gt) NOVA_CW(Select)
#undef NOVA_CW
        case Op::Dot: {
            float s = 0.0f;
            for (int k = 0; k < int(in.imm); ++k)
                s += r.ld(in.a, in.flags & kUniformA, k, false) * r.ld(in.b, in.flags & kUniformB, k, false);
            r.R[in.dst] = s;
        } break;
        case Op::Normalize: {
            float v[4], l2 = 0.0f;
            for (int k = 0; k < in.width; ++k) { v[k] = r.ld(in.a, in.flags & kUniformA, k, false); l2 += v[k] * v[k]; }
            const float s = l2 > 0.0f ? 1.0f / std::sqrt(l2) : 0.0f;
            for (int k = 0; k < in.width; ++k) r.R[in.dst + k] = v[k] * s;
        } break;
        case Op::Extract: r.R[in.dst] = r.ld(in.a, in.flags & kUniformA, int(in.imm), false); break;
        case Op::Combine3:
            r.R[in.dst + 0] = r.ld(in.a, in.flags & kUniformA, 0, false);
            r.R[in.dst + 1] = r.ld(in.b, in.flags & kUniformB, 0, false);
            r.R[in.dst + 2] = r.ld(in.c, in.flags & kUniformC, 0, false);
            break;
        default: return;
        }
    }
}

// ---------------------------------------------------------------- batch (CPU)
// Register row `row` of lane l at R[row * N + l]; a vecW value occupies W
// adjacent rows, so a component-wise op is W lane loops of length N.
template <int N>
struct BatchRegs {
    float *NOVA_RESTRICT R;
    const float *NOVA_RESTRICT U;
};

template <Op O, int N, bool UA, bool UB, bool UC>
inline void batchCwT(const Instr &in, BatchRegs<N> r) {
    for (int k = 0; k < in.width; ++k) {
        const int ka = (in.flags & kSplatA) ? 0 : k, kb = (in.flags & kSplatB) ? 0 : k, kc = (in.flags & kSplatC) ? 0 : k;
        const float *NOVA_RESTRICT a = UA ? r.U + in.a + ka : r.R + (in.a + ka) * N;
        const float *NOVA_RESTRICT b = UB ? r.U + in.b + kb : r.R + (in.b + kb) * N;
        const float *NOVA_RESTRICT c = UC ? r.U + in.c + kc : r.R + (in.c + kc) * N;
        float *NOVA_RESTRICT d = r.R + (in.dst + k) * N;
        const float ua = a[0], ub = b[0], uc = c[0]; // loop-invariant broadcasts
        for (int l = 0; l < N; ++l)
            d[l] = Fn<O>::eval(UA ? ua : a[l], UB ? ub : b[l], UC ? uc : c[l]);
    }
}

template <Op O, int N>
inline void batchCw(const Instr &in, BatchRegs<N> r) {
    // Unused operand slots are flagged uniform by the emitter, so unary and
    // binary ops only instantiate the variants they need.
    switch (in.flags & 7) {
    case 0: batchCwT<O, N, false, false, false>(in, r); break;
    case 1: batchCwT<O, N, true, false, false>(in, r); break;
    case 2: batchCwT<O, N, false, true, false>(in, r); break;
    case 3: batchCwT<O, N, true, true, false>(in, r); break;
    case 4: batchCwT<O, N, false, false, true>(in, r); break;
    case 5: batchCwT<O, N, true, false, true>(in, r); break;
    case 6: batchCwT<O, N, false, true, true>(in, r); break;
    default: batchCwT<O, N, true, true, true>(in, r); break;
    }
}

template <int N>
inline const float *rowOrUniform(BatchRegs<N> r, uint16_t idx, bool uniform, int k, float *scratch) {
    if (!uniform) return r.R + (idx + k) * N;
    for (int l = 0; l < N; ++l) scratch[l] = r.U[idx + k];
    return scratch;
}

template <int N>
void runBatch(const Instr *code, float *NOVA_RESTRICT R, const float *NOVA_RESTRICT U) {
    BatchRegs<N> r{R, U};
    alignas(64) float sa[N], sb[N];
    for (const Instr *ip = code;; ++ip) {
        const Instr &in = *ip;
        switch (in.op) {
        case Op::End: return;
#define NOVA_CW(o) case Op::o: batchCw<Op::o, N>(in, r); break;
        NOVA_CW(Mov) NOVA_CW(Add) NOVA_CW(Sub) NOVA_CW(Mul) NOVA_CW(Div) NOVA_CW(Mad) NOVA_CW(Mix)
        NOVA_CW(Clamp) NOVA_CW(Min) NOVA_CW(Max) NOVA_CW(Floor) NOVA_CW(Fract) NOVA_CW(Abs) NOVA_CW(Sqrt)
        NOVA_CW(Sin) NOVA_CW(Gt) NOVA_CW(Select)
#undef NOVA_CW
        case Op::Dot: {
            float *NOVA_RESTRICT d = R + in.dst * N;
            for (int l = 0; l < N; ++l) d[l] = 0.0f;
            for (int k = 0; k < int(in.imm); ++k) {
                const float *a = rowOrUniform<N>(r, in.a, in.flags & kUniformA, k, sa);
                const float *b = rowOrUniform<N>(r, in.b, in.flags & kUniformB, k, sb);
                for (int l = 0; l < N; ++l) d[l] += a[l] * b[l];
            }
        } break;
        case Op::Normalize: {
            alignas(64) float l2[N] = {};
            for (int k = 0; k < in.width; ++k) {
                const float *a = rowOrUniform<N>(r, in.a, in.flags & kUniformA, k, sa);
                for (int l = 0; l < N; ++l) l2[l] += a[l] * a[l];
            }
            for (int l = 0; l < N; ++l) l2[l] = l2[l] > 0.0f ? 1.0f / std::sqrt(l2[l]) : 0.0f;
            for (int k = 0; k < in.width; ++k) {
                const float *a = rowOrUniform<N>(r, in.a, in.flags & kUniformA, k, sa);
                float *d = R + (in.dst + k) * N;
                for (int l = 0; l < N; ++l) d[l] = a[l] * l2[l];
            }
        } break;
        case Op::Extract: {
            const float *a = rowOrUniform<N>(r, in.a, in.flags & kUniformA, int(in.imm), sa);
            float *d = R + in.dst * N;
            for (int l = 0; l < N; ++l) d[l] = a[l];
        } break;
        case Op::Combine3: {
            const uint16_t src[3] = {in.a, in.b, in.c};
            for (int k = 0; k < 3; ++k) {
                const float *a = rowOrUniform<N>(r, src[k], in.flags & (kUniformA << k), 0, sa);
                float *d = R + (in.dst + k) * N;
                for (int l = 0; l < N; ++l) d[l] = a[l];
            }
        } break;
        default: return;
        }
    }
}

} // namespace nova::shading
