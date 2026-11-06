#include "core/shading/interval.h"

#include "core/math/common.h"

#include <algorithm>
#include <cmath>

namespace nova::shading {

namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

Interval make(float a, float b) {
    if (std::isnan(a) || std::isnan(b))
        return Interval::unbounded();
    return {std::min(a, b), std::max(a, b)};
}

// The hull of values (NaN anywhere: unbounded).
template <typename... T>
Interval hull(float first, T... rest) {
    float lo = first, hi = first;
    bool nan = std::isnan(first);
    for (float v : {rest...}) {
        nan = nan || std::isnan(v);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    return nan ? Interval::unbounded() : Interval{lo, hi};
}

Interval join(Interval a, Interval b) { return {std::min(a.lo, b.lo), std::max(a.hi, b.hi)}; }

// 0 times anything (also infinity) is 0, as for finite values.
float mul(float a, float b) { return a == 0.0f || b == 0.0f ? 0.0f : a * b; }

Interval imul(Interval a, Interval b) {
    return hull(mul(a.lo, b.lo), mul(a.lo, b.hi), mul(a.hi, b.lo), mul(a.hi, b.hi));
}

Interval iadd(Interval a, Interval b) { return make(a.lo + b.lo, a.hi + b.hi); }
Interval isub(Interval a, Interval b) { return make(a.lo - b.hi, a.hi - b.lo); }

// Truth of a condition row: definitely non-zero, definitely zero, or either.
enum class Truth { False, True, Either };
Truth truth(Interval c) {
    if (c.lo == 0.0f && c.hi == 0.0f)
        return Truth::False;
    if (c.lo > 0.0f || c.hi < 0.0f)
        return Truth::True;
    return Truth::Either;
}
Interval boolean(Truth t) {
    return t == Truth::True ? Interval::point(1.0f) : t == Truth::False ? Interval::point(0.0f) : Interval{0.0f, 1.0f};
}

// A non-decreasing function of one argument.
template <typename F>
Interval monotone(Interval a, F f) {
    return make(f(a.lo), f(a.hi));
}

// The largest magnitude of noise.h's Perlin noise in `dim` dimensions (also
// per channel of the vector forms), 0.1 % wider for rounding. A corner's
// gradient term is at most |u| + 2|v| (2D) or |u| + |v| (3D) of its
// offsets, and the fade-weighted mean offset from the corners along an axis,
// (1 - s(f)) f + s(f) (1 - f), is at most 1/2: the interpolated sum stays
// below 2 (2D: 2 x (1/2 + 1/2)) and 3/2 (3D), times the scales 0.6616 and
// 0.9820. Fog Box noise (docs/design/M11-P5.md) needs a bound, not the
// tighter observed range.
float perlinBound(int dim) { return (dim == 2 ? 0.6616f * 2.0f : 0.9820f * 1.5f) * 1.001f; }

Interval unary(Op op, Interval a) {
    switch (op) {
    case Op::Copy:
        return a;
    case Op::Neg:
        return {-a.hi, -a.lo};
    case Op::Abs:
        if (a.lo >= 0.0f)
            return a;
        if (a.hi <= 0.0f)
            return {-a.hi, -a.lo};
        return {0.0f, std::max(-a.lo, a.hi)};
    case Op::Floor:
    case Op::Ceil:
    case Op::Round:
    case Op::Trunc:
    case Op::Sign:
    case Op::Sqrt:
    case Op::Exp:
    case Op::Atan:
    case Op::Asin:
        return monotone(a, [op](float x) { return evalUnary(op, x); });
    case Op::Acos: // non-increasing
        return make(vexAcos(a.hi), vexAcos(a.lo));
    case Op::Fract:
        if (std::floor(a.lo) == std::floor(a.hi) && a.bounded())
            return make(a.lo - std::floor(a.lo), a.hi - std::floor(a.hi));
        return {0.0f, 1.0f};
    case Op::Log: // 0 at x <= 0 (VEX)
        if (a.lo > 0.0f)
            return make(std::log(a.lo), std::log(a.hi));
        if (a.hi <= 0.0f)
            return Interval::point(0.0f);
        return {-kInfinity, std::max(0.0f, std::log(a.hi))};
    case Op::Sin:
    case Op::Cos:
        return {-1.0f, 1.0f};
    case Op::Not: {
        const Truth t = truth(a);
        return boolean(t == Truth::True ? Truth::False : t == Truth::False ? Truth::True : Truth::Either);
    }
    default: // Tan
        return Interval::unbounded();
    }
}

Interval pow(Interval a, Interval b) {
    if (a.lo > 0.0f) // exp(b log a): extremes at the corners
        return hull(vexPow(a.lo, b.lo), vexPow(a.lo, b.hi), vexPow(a.hi, b.lo), vexPow(a.hi, b.hi));
    if (b.lo == 0.0f && b.hi == 0.0f)
        return Interval::point(1.0f);
    if (a.lo == 0.0f && b.lo > 0.0f) // pow(0, b > 0) = 0
        return hull(0.0f, vexPow(a.hi, b.lo), vexPow(a.hi, b.hi));
    return Interval::unbounded();
}

Interval binary(Op op, Interval a, Interval b) {
    switch (op) {
    case Op::Add:
        return iadd(a, b);
    case Op::Sub:
        return isub(a, b);
    case Op::Mul:
        return imul(a, b);
    case Op::Div:
        if (b.lo > 0.0f || b.hi < 0.0f)
            return hull(a.lo / b.lo, a.lo / b.hi, a.hi / b.lo, a.hi / b.hi);
        if (b.lo == 0.0f && b.hi == 0.0f)
            return Interval::point(0.0f); // VEX: x / 0 = 0
        return Interval::unbounded();
    case Op::Mod: { // the sign of b, below |b|
        const float m = std::max(std::fabs(b.lo), std::fabs(b.hi));
        if (!std::isfinite(m))
            return Interval::unbounded();
        return {b.lo < 0.0f ? -m : 0.0f, b.hi > 0.0f ? m : 0.0f};
    }
    case Op::Min:
        return {std::min(a.lo, b.lo), std::min(a.hi, b.hi)};
    case Op::Max:
        return {std::max(a.lo, b.lo), std::max(a.hi, b.hi)};
    case Op::Pow:
        return pow(a, b);
    case Op::Atan2:
        return {-kPi, kPi};
    case Op::Greater:
        return boolean(a.lo > b.hi ? Truth::True : a.hi <= b.lo ? Truth::False : Truth::Either);
    case Op::GreaterEq:
        return boolean(a.lo >= b.hi ? Truth::True : a.hi < b.lo ? Truth::False : Truth::Either);
    case Op::Equal:
        if (a.lo == a.hi && b.lo == b.hi)
            return boolean(a.lo == b.lo ? Truth::True : Truth::False);
        return boolean(a.hi < b.lo || b.hi < a.lo ? Truth::False : Truth::Either);
    case Op::And: {
        const Truth x = truth(a), y = truth(b);
        return boolean(x == Truth::False || y == Truth::False ? Truth::False
                       : x == Truth::True && y == Truth::True ? Truth::True
                                                              : Truth::Either);
    }
    case Op::Or: {
        const Truth x = truth(a), y = truth(b);
        return boolean(x == Truth::True || y == Truth::True ? Truth::True
                       : x == Truth::False && y == Truth::False ? Truth::False
                                                                : Truth::Either);
    }
    default:
        return Interval::unbounded();
    }
}

Interval ternary(Op op, Interval a, Interval b, Interval c) {
    switch (op) {
    case Op::Clamp: // non-decreasing in all three
        return make(vexClamp(a.lo, b.lo, c.lo), vexClamp(a.hi, b.hi, c.hi));
    case Op::Mix: { // a (1 - t) + b t: multilinear, extremes at the corners
        float v[8];
        int n = 0;
        for (float x : {a.lo, a.hi})
            for (float y : {b.lo, b.hi})
                for (float t : {c.lo, c.hi})
                    v[n++] = x + mul(y - x, t);
        return hull(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
    case Op::Smoothstep: // (lo, hi, x)
        if (a.lo == a.hi && b.lo == b.hi && a.lo < b.lo)
            return make(vexSmoothstep(a.lo, b.lo, c.lo), vexSmoothstep(a.lo, b.lo, c.hi));
        return {0.0f, 1.0f};
    case Op::Select: {
        const Truth t = truth(a);
        return t == Truth::True ? b : t == Truth::False ? c : join(b, c);
    }
    default:
        return Interval::unbounded();
    }
}

// Karma's blackbody over a temperature range, per channel into out: the
// values at the ends and at the table's temperatures in between (the
// interpolation between entries stays within them up to rounding), 1 %
// wider.
void blackbodyRange(Interval t, int width, Interval *out) {
    if (!t.bounded()) {
        for (int k = 0; k < width; ++k)
            out[k] = Interval::unbounded();
        return;
    }
    Spectrum lo = blackbody(t.lo), hi = lo;
    const auto widen = [&](Spectrum c) {
        lo = Spectrum{std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
        hi = Spectrum{std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
    };
    widen(blackbody(t.hi));
    for (int i = 0; i < kBlackbodyEntries; ++i) {
        const float k = blackbodyKelvin(i);
        if (k > t.lo && k < t.hi)
            widen(blackbody(k));
    }
    const float l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < width && k < 3; ++k) {
        const float m = 0.01f * std::max(std::fabs(l[k]), std::fabs(h[k]));
        out[k] = {l[k] - m, h[k] + m};
    }
}

Interval operand(const Interval *R, const float *U, uint16_t index, bool uniform, bool splat, int k) {
    const int kk = splat ? 0 : k;
    return uniform ? Interval::point(U[index + kk]) : R[index + kk];
}

} // namespace

void executeInterval(const Instr *code, size_t count, Interval *R, const float *U) {
    for (size_t i = 0; i < count; ++i) {
        const Instr &in = code[i];
        const int w = in.width;
        const bool ua = in.flags & kUniformA, ub = in.flags & kUniformB, uc = in.flags & kUniformC;
        const bool sa = in.flags & kSplatA, sb = in.flags & kSplatB, sc = in.flags & kSplatC;
        Interval out[4];
        switch (opClass(in.op)) {
        case OpClass::Unary:
            for (int k = 0; k < w; ++k)
                out[k] = unary(in.op, operand(R, U, in.a, ua, sa, k));
            break;
        case OpClass::Binary:
            for (int k = 0; k < w; ++k)
                out[k] = binary(in.op, operand(R, U, in.a, ua, sa, k), operand(R, U, in.b, ub, sb, k));
            break;
        case OpClass::Ternary:
            for (int k = 0; k < w; ++k)
                out[k] = ternary(in.op, operand(R, U, in.a, ua, sa, k), operand(R, U, in.b, ub, sb, k),
                                 operand(R, U, in.c, uc, sc, k));
            break;
        case OpClass::Reduce: {
            const int wa = int(in.imm);
            Interval acc = Interval::point(0.0f);
            for (int k = 0; k < wa; ++k) {
                const Interval x = operand(R, U, in.a, ua, false, k);
                if (in.op == Op::Length) {
                    const Interval m = unary(Op::Abs, x); // squares of a non-negative range
                    acc = iadd(acc, imul(m, m));
                } else {
                    const Interval y = operand(R, U, in.b, ub, false, k);
                    if (in.op == Op::Dot) {
                        acc = iadd(acc, imul(x, y));
                    } else {
                        const Interval d = unary(Op::Abs, isub(x, y));
                        acc = iadd(acc, imul(d, d));
                    }
                }
            }
            out[0] = in.op == Op::Dot ? acc : monotone(acc, [](float v) { return vexSqrt(v); });
            break;
        }
        default:
            switch (in.op) {
            case Op::Normalize:
                for (int k = 0; k < w; ++k)
                    out[k] = {-1.0f, 1.0f};
                break;
            case Op::Cross: {
                Interval a[3], b[3];
                for (int k = 0; k < 3; ++k) {
                    a[k] = operand(R, U, in.a, ua, false, k);
                    b[k] = operand(R, U, in.b, ub, false, k);
                }
                out[0] = isub(imul(a[1], b[2]), imul(a[2], b[1]));
                out[1] = isub(imul(a[2], b[0]), imul(a[0], b[2]));
                out[2] = isub(imul(a[0], b[1]), imul(a[1], b[0]));
                break;
            }
            case Op::Swizzle:
                for (int k = 0; k < w; ++k)
                    out[k] = operand(R, U, in.a, ua, false, swizzleComponent(in.imm, k));
                break;
            case Op::Concat: {
                const int wa = int(in.imm);
                for (int k = 0; k < w; ++k)
                    out[k] = k < wa ? operand(R, U, in.a, ua, false, k) : operand(R, U, in.b, ub, false, k - wa);
                break;
            }
            case Op::CellNoise: // MaterialX cellnoise: [0, 1)
                for (int k = 0; k < w; ++k)
                    out[k] = {0.0f, 1.0f};
                break;
            case Op::Noise: { // Perlin, any position
                const float b = perlinBound(int(in.imm & 15u));
                for (int k = 0; k < w; ++k)
                    out[k] = {-b, b};
                break;
            }
            case Op::Fractal: { // the octaves' amplitudes |diminish|^i summed (ops.h: 0 to 32 octaves)
                const Interval octaves = operand(R, U, in.b, ub, false, 0);
                const Interval diminish = operand(R, U, in.c, uc, false, 1);
                if (!octaves.bounded() || !diminish.bounded()) {
                    for (int k = 0; k < w; ++k)
                        out[k] = Interval::unbounded();
                    break;
                }
                const int n = std::clamp(int(octaves.hi), 0, 32);
                const float m = std::max(std::fabs(diminish.lo), std::fabs(diminish.hi));
                float sum = 0.0f, amp = 1.0f;
                for (int octave = 0; octave < n; ++octave) {
                    sum += amp;
                    amp *= m;
                }
                const float b = perlinBound(int(in.imm & 15u)) * sum * 1.001f;
                for (int k = 0; k < w; ++k)
                    out[k] = std::isfinite(b) ? Interval{-b, b} : Interval::unbounded();
                break;
            }
            case Op::Blackbody: // the ends and the table entries between them, 1 % wider
                blackbodyRange(operand(R, U, in.a, ua, false, 0), w, out);
                break;
            case Op::RgbToHsv: { // noise.h: h in [0, 1]; s = (max - min) / max; v = max
                const Interval r = operand(R, U, in.a, ua, false, 0), g = operand(R, U, in.a, ua, false, 1),
                               b = operand(R, U, in.a, ua, false, 2);
                out[0] = {0.0f, 1.0f};
                out[1] = r.lo >= 0.0f && g.lo >= 0.0f && b.lo >= 0.0f ? Interval{0.0f, 1.0f} : Interval{0.0f, kInfinity};
                out[2] = {std::max({r.lo, g.lo, b.lo}), std::max({r.hi, g.hi, b.hi})};
                break;
            }
            case Op::HsvToRgb: { // every channel v (1 - s t) for some t in [0, 1]
                const Interval s = operand(R, U, in.a, ua, false, 1), v = operand(R, U, in.a, ua, false, 2);
                const Interval k = hull(1.0f, 1.0f - s.lo, 1.0f - s.hi);
                out[0] = out[1] = out[2] = imul(v, k);
                break;
            }
            default: // other procedurals, textures, queries
                for (int k = 0; k < w; ++k)
                    out[k] = Interval::unbounded();
                break;
            }
            break;
        }
        for (int k = 0; k < w; ++k)
            R[in.dst + k] = out[k];
    }
}

} // namespace nova::shading
