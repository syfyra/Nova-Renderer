#pragma once

// Shading operations: the semantics every executor shares (docs/design/M3.md
// K3, K5). Pattern math follows VEX, as Karma's MaterialX nodes do, measured
// with vexexec (tests/test_shading_vm.cpp holds the goldens):
//   a / 0 = 0, a % 0 = 0, floored modulo (a - b floor(a / b)),
//   sqrt and log of x <= 0 are 0, asin/acos clamp their argument,
//   pow with a negative base rounds the exponent to an integer,
//   pow(0, y <= 0) is 1 for y = 0 and 0 otherwise,
//   round(x) = floor(x + 0.5), smoothstep(lo, hi, x) blends the clamped
//   VEX-safe ratio (x - lo) / (hi - lo) (Karma's mxstd.h),
//   clamp(x, lo, hi) = x < lo ? lo : x > hi ? hi : x,
//   normalize(0) = 0.
// Every operation is a scalar function of its inputs; vector operations work
// per component or reduce explicitly (Dot, Length, ...).

#include "core/math/common.h"
#include "core/shading/noise.h"
#include "core/volume/blackbody.h"

#include <cmath>
#include <cstdint>

namespace nova::shading {

enum class Op : uint16_t {
    // Leaves of the IR. Constants and parameters become uniform operands;
    // globals and primvars are rows the shade stage fills before the code
    // runs, so none of them is ever executed.
    Const,
    Param,
    Global,
    Primvar,

    // Per component (operands of width 1 are splat to the result width).
    Copy,
    Neg,
    Abs,
    Floor,
    Ceil,
    Round,
    Trunc,
    Sign,
    Fract,
    Sqrt,
    Exp,
    Log,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Not,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Min,
    Max,
    Pow,
    Atan2,
    Greater,   // 1 if a > b, else 0
    GreaterEq, // 1 if a >= b
    Equal,     // 1 if a == b
    And,       // 1 if a != 0 and b != 0
    Or,        // 1 if a != 0 or b != 0
    Clamp,     // (x, lo, hi)
    Mix,       // (a, b, t): a + (b - a) t
    Smoothstep, // (lo, hi, x)
    Select,    // (cond, a, b): cond != 0 ? a : b

    // Vector operations; imm holds the operand width where the result has
    // a different one.
    Dot,       // result 1, imm = operand width
    Length,    // result 1, imm = operand width
    Distance,  // result 1, imm = operand width
    Normalize, // operand and result of the instruction width
    Cross,     // width 3

    // Structure.
    Swizzle,   // imm = operand width | component k at bits 4 + 2k
    Concat,    // result = (a, b), imm = width of a

    // MaterialX procedurals (noise.h); imm = noiseImm(dim, width, style).
    Noise,     // a = position (dim wide): Perlin; width 4 adds a decorrelated float
    Fractal,   // a = position, b = octaves, c = (lacunarity, diminish)
    CellNoise, // a = position; width 1 or 3
    Worley,    // a = position, b = jitter; style 0 distances F1.., 1 solid cells
    RgbToHsv,  // width 3
    HsvToRgb,  // width 3
    Flake,     // a = position (flake units, 3 wide), b = coverage; width 4: priority, xi0, xi1, rand
    Voronoi,   // a = position (dim wide), b = jitter; imm = noiseImm(dim, width, metric) | part << 12:
               // part 0 the distances F1..F3, part 1..3 the seed point (3 wide)
    Blackbody, // a = temperature in kelvin; width 3: Karma's ND_blackbody (core/volume/blackbody.h);
               // imm = noiseImm(1, 3)

    // Textures (core/texture/texture.h): a = uv, b = footprint derivatives
    // (dsdx, dtdx, dsdy, dtdy) or none (finest level), c = fallback (the
    // result's width); imm = texture slot of the program. Always varying,
    // never folded.
    Tex2D,
    // The differences of a (1 wide) at the hit moved by the two footprint
    // vectors (bump heights). The compiler expands it into shifted clones;
    // the VM never sees it.
    Deriv,
    // A ray query (core/shading/query.h): a = direction (3 wide; zero: the
    // shading normal), b = parameters (4 wide); imm = QueryKind. Always
    // varying, never folded or cloned for derivatives.
    Query,

    Count
};

enum class OpClass : uint8_t { Leaf, Unary, Binary, Ternary, Reduce, Special };

NOVA_INLINE OpClass opClass(Op op) {
    if (op <= Op::Primvar)
        return OpClass::Leaf;
    if (op <= Op::Not)
        return OpClass::Unary;
    if (op <= Op::Or)
        return OpClass::Binary;
    if (op <= Op::Select)
        return OpClass::Ternary;
    if (op <= Op::Distance)
        return OpClass::Reduce;
    return OpClass::Special;
}

NOVA_INLINE int operandCount(Op op) {
    switch (opClass(op)) {
    case OpClass::Leaf:
        return op == Op::Primvar ? 1 : 0; // the primvar's fallback value
    case OpClass::Unary:
        return 1;
    case OpClass::Binary:
        return 2;
    case OpClass::Ternary:
        return 3;
    case OpClass::Reduce:
        return op == Op::Distance || op == Op::Dot ? 2 : 1;
    default:
        switch (op) {
        case Op::Normalize:
        case Op::Swizzle:
        case Op::Deriv:
        case Op::Noise:
        case Op::CellNoise:
        case Op::RgbToHsv:
        case Op::HsvToRgb:
        case Op::Blackbody:
            return 1;
        case Op::Fractal:
        case Op::Tex2D:
            return 3;
        default:
            return 2;
        }
    }
}

NOVA_INLINE uint32_t noiseImm(int dim, int width, int style = 0) {
    return uint32_t(dim) | uint32_t(width) << 4 | uint32_t(style) << 8;
}

// Swizzle immediates.
NOVA_INLINE uint32_t swizzleImm(int operandWidth, int c0, int c1 = 0, int c2 = 0, int c3 = 0) {
    return uint32_t(operandWidth) | uint32_t(c0) << 4 | uint32_t(c1) << 6 | uint32_t(c2) << 8 | uint32_t(c3) << 10;
}
NOVA_INLINE int swizzleOperandWidth(uint32_t imm) { return int(imm & 7u); }
NOVA_INLINE int swizzleComponent(uint32_t imm, int k) { return int(imm >> (4 + 2 * k)) & 3; }

// ---------------------------------------------------------------------------
// Scalar semantics
// ---------------------------------------------------------------------------

NOVA_INLINE float vexDiv(float a, float b) { return b != 0.0f ? a / b : (a < 0.0f ? -0.0f : 0.0f); }
NOVA_INLINE float vexMod(float a, float b) { return b != 0.0f ? a - b * std::floor(a / b) : 0.0f; }
NOVA_INLINE float vexSqrt(float a) { return a > 0.0f ? std::sqrt(a) : 0.0f; }
NOVA_INLINE float vexLog(float a) { return a > 0.0f ? std::log(a) : 0.0f; }
NOVA_INLINE float vexRound(float a) { return std::floor(a + 0.5f); }
NOVA_INLINE float vexPow(float a, float b) {
    if (a < 0.0f)
        return std::pow(a, std::floor(b + 0.5f));
    if (a == 0.0f)
        return b == 0.0f ? 1.0f : 0.0f;
    return std::pow(a, b);
}
NOVA_INLINE float vexAsin(float a) { return std::asin(a < -1.0f ? -1.0f : (a > 1.0f ? 1.0f : a)); }
NOVA_INLINE float vexAcos(float a) { return std::acos(a < -1.0f ? -1.0f : (a > 1.0f ? 1.0f : a)); }
NOVA_INLINE float vexSign(float a) { return a > 0.0f ? 1.0f : (a < 0.0f ? -1.0f : 0.0f); }
NOVA_INLINE float vexClamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
// Karma's MaterialX smoothstep (mxstd.h): VEX-safe division, so lo >= hi
// still blends (reversed) and lo == hi gives 0.
NOVA_INLINE float vexSmoothstep(float lo, float hi, float x) {
    const float t = vexClamp(vexDiv(x - lo, hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

NOVA_INLINE float evalUnary(Op op, float a) {
    switch (op) {
    case Op::Copy:
        return a;
    case Op::Neg:
        return -a;
    case Op::Abs:
        return std::fabs(a);
    case Op::Floor:
        return std::floor(a);
    case Op::Ceil:
        return std::ceil(a);
    case Op::Round:
        return vexRound(a);
    case Op::Trunc:
        return std::trunc(a);
    case Op::Sign:
        return vexSign(a);
    case Op::Fract:
        return a - std::floor(a);
    case Op::Sqrt:
        return vexSqrt(a);
    case Op::Exp:
        return std::exp(a);
    case Op::Log:
        return vexLog(a);
    case Op::Sin:
        return std::sin(a);
    case Op::Cos:
        return std::cos(a);
    case Op::Tan:
        return std::tan(a);
    case Op::Asin:
        return vexAsin(a);
    case Op::Acos:
        return vexAcos(a);
    case Op::Atan:
        return std::atan(a);
    case Op::Not:
        return a == 0.0f ? 1.0f : 0.0f;
    default:
        return 0.0f;
    }
}

NOVA_INLINE float evalBinary(Op op, float a, float b) {
    switch (op) {
    case Op::Add:
        return a + b;
    case Op::Sub:
        return a - b;
    case Op::Mul:
        return a * b;
    case Op::Div:
        return vexDiv(a, b);
    case Op::Mod:
        return vexMod(a, b);
    case Op::Min:
        return b < a ? b : a;
    case Op::Max:
        return b > a ? b : a;
    case Op::Pow:
        return vexPow(a, b);
    case Op::Atan2:
        return std::atan2(a, b);
    case Op::Greater:
        return a > b ? 1.0f : 0.0f;
    case Op::GreaterEq:
        return a >= b ? 1.0f : 0.0f;
    case Op::Equal:
        return a == b ? 1.0f : 0.0f;
    case Op::And:
        return a != 0.0f && b != 0.0f ? 1.0f : 0.0f;
    case Op::Or:
        return a != 0.0f || b != 0.0f ? 1.0f : 0.0f;
    default:
        return 0.0f;
    }
}

NOVA_INLINE float evalTernary(Op op, float a, float b, float c) {
    switch (op) {
    case Op::Clamp:
        return vexClamp(a, b, c);
    case Op::Mix:
        return a + (b - a) * c;
    case Op::Smoothstep:
        return vexSmoothstep(a, b, c);
    case Op::Select:
        return a != 0.0f ? b : c;
    default:
        return 0.0f;
    }
}

// The procedurals and color conversions for one lane: operands a (position
// or color), b, c; `out` receives the instruction's width.
NOVA_INLINE void evalProcedural(Op op, uint32_t imm, const float *a, const float *b, const float *c, float *out) {
    using namespace noise;
    const int dim = int(imm & 15u), w = int((imm >> 4) & 15u), style = int((imm >> 8) & 15u);
    const Vec2f p2{a[0], a[1]};
    const Vec3f p3{a[0], a[1], dim == 3 ? a[2] : 0.0f};
    auto put3 = [&](Vec3f v) {
        out[0] = v.x;
        if (w > 1)
            out[1] = v.y;
        if (w > 2)
            out[2] = v.z;
    };
    switch (op) {
    case Op::Noise:
        if (w == 1) {
            out[0] = dim == 2 ? perlin(p2) : perlin(p3);
        } else {
            put3(dim == 2 ? perlinV3(p2) : perlinV3(p3));
            if (w == 4)
                out[3] = dim == 2 ? perlin(p2 + Vec2f{19.0f, 73.0f}) : perlin(p3 + Vec3f{19.0f, 73.0f, 29.0f});
        }
        return;
    case Op::Fractal: {
        const int count = toInt(b[0]); // defined for a NaN or infinite input (INT_MIN: no octave)
        const int octaves = count < 0 ? 0 : (count > 32 ? 32 : count);
        const float lac = c[0], dimish = c[1];
        auto f = [&](bool shifted) {
            return dim == 2 ? fractal(shifted ? p2 + Vec2f{19.0f, 193.0f} : p2, octaves, lac, dimish)
                            : fractal(shifted ? p3 + Vec3f{19.0f, 193.0f, 17.0f} : p3, octaves, lac, dimish);
        };
        if (w == 1) {
            out[0] = f(false);
        } else if (w == 2) {
            out[0] = f(false);
            out[1] = f(true);
        } else {
            put3(dim == 2 ? fractalV3(p2, octaves, lac, dimish) : fractalV3(p3, octaves, lac, dimish));
            if (w == 4)
                out[3] = f(true);
        }
        return;
    }
    case Op::CellNoise:
        if (w == 1)
            out[0] = dim == 2 ? cell(p2) : cell(p3);
        else
            put3(dim == 2 ? cellV3(p2) : cellV3(p3));
        return;
    case Op::Worley: {
        float d[3];
        if (dim == 2) {
            Vec2f minpos;
            worley(p2, b[0], d, minpos);
            if (style == 1) {
                const Vec2f q = minpos + p2;
                if (w == 1)
                    out[0] = cell(q);
                else
                    put3(cellV3(q));
                return;
            }
        } else {
            Vec3f minpos;
            worley(p3, b[0], d, minpos);
            if (style == 1) {
                const Vec3f q = minpos + p3;
                if (w == 1)
                    out[0] = cell(q);
                else
                    put3(cellV3(q));
                return;
            }
        }
        for (int k = 0; k < w && k < 3; ++k)
            out[k] = std::sqrt(d[k]);
        return;
    }
    case Op::Flake:
        flake(Vec3f{a[0], a[1], a[2]}, b[0], out);
        return;
    case Op::Voronoi: {
        float d[3];
        Vec3f pts[3];
        voronoi(p3, b[0], style, dim, d, pts);
        const int part = int((imm >> 12) & 15u);
        if (part == 0) {
            for (int k = 0; k < w && k < 3; ++k)
                out[k] = d[k];
        } else {
            put3(pts[part - 1]);
        }
        return;
    }
    case Op::RgbToHsv:
    case Op::HsvToRgb: { // always width 3 (no immediate)
        const Vec3f v = op == Op::RgbToHsv ? rgbToHsv(Vec3f{a[0], a[1], a[2]}) : hsvToRgb(Vec3f{a[0], a[1], a[2]});
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
        return;
    }
    case Op::Blackbody: {
        const Spectrum bb = blackbody(a[0]);
        out[0] = bb.x;
        out[1] = bb.y;
        out[2] = bb.z;
        return;
    }
    default:
        return;
    }
}

NOVA_INLINE bool isProcedural(Op op) { return op >= Op::Noise && op <= Op::Blackbody; }

// Transcendental operations call the C library; executors keep them out of
// vectorized loops so every lane count gives bit-identical results.
NOVA_INLINE bool isTranscendental(Op op) {
    switch (op) {
    case Op::Exp:
    case Op::Log:
    case Op::Sin:
    case Op::Cos:
    case Op::Tan:
    case Op::Asin:
    case Op::Acos:
    case Op::Atan:
    case Op::Pow:
    case Op::Atan2:
        return true;
    default:
        return false;
    }
}

} // namespace nova::shading
