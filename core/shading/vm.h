#pragma once

// Bytecode executor (docs/design/M3.md K5). One template over the lane count:
// execute<1> is the per-lane executor (filters, prologues, the GPU later),
// execute<8/16/32> the batch executor of the shade stage. Every operation
// runs as one loop over the lanes per component, which the compiler
// vectorizes; the C library's transcendental functions stay in scalar loops
// (vectorized variants round differently), so all lane counts give
// bit-identical results.
//
// R: rowCount rows of N floats (row r, lane l at R[r * N + l]).
// U: the material's uniform block.
// tex: the texture sampler and the material's slots (Tex2D; without it, or
// on the device until M10, lookups return their fallback).

#include "core/shading/program.h"
#include "core/shading/query.h"
#if defined(__CUDACC__)
#include "core/gpu/device_texture.h"
#endif

#if defined(_MSC_VER) && !defined(__CUDACC__)
#define NOVA_NO_VECTOR __pragma(loop(no_vector))
#elif defined(__clang__)
#define NOVA_NO_VECTOR _Pragma("clang loop vectorize(disable)")
#else
#define NOVA_NO_VECTOR
#endif

namespace nova::shading {

namespace vm_detail {

template <int N>
NOVA_INLINE const float *operand(const float *R, const float *U, uint16_t index, bool uniform, bool splat, int k,
                                 float *tmp) {
    const int kk = splat ? 0 : k;
    if (uniform) {
        const float u = U[index + kk];
        for (int l = 0; l < N; ++l)
            tmp[l] = u;
        return tmp;
    }
    return R + size_t(index + kk) * N;
}

#define NOVA_VM_LOOP(EXPR)                                                                                             \
    for (int l = 0; l < N; ++l)                                                                                        \
        D[l] = (EXPR);
#define NOVA_VM_SCALAR_LOOP(EXPR)                                                                                      \
    NOVA_NO_VECTOR for (int l = 0; l < N; ++l) D[l] = (EXPR);

template <int N>
NOVA_INLINE void unary(Op op, float *D, const float *A) {
    switch (op) {
    case Op::Copy:
        NOVA_VM_LOOP(A[l]) break;
    case Op::Neg:
        NOVA_VM_LOOP(-A[l]) break;
    case Op::Abs:
        NOVA_VM_LOOP(std::fabs(A[l])) break;
    case Op::Floor:
        NOVA_VM_LOOP(std::floor(A[l])) break;
    case Op::Ceil:
        NOVA_VM_LOOP(std::ceil(A[l])) break;
    case Op::Round:
        NOVA_VM_LOOP(vexRound(A[l])) break;
    case Op::Trunc:
        NOVA_VM_LOOP(std::trunc(A[l])) break;
    case Op::Sign:
        NOVA_VM_LOOP(vexSign(A[l])) break;
    case Op::Fract:
        NOVA_VM_LOOP(A[l] - std::floor(A[l])) break;
    case Op::Sqrt:
        NOVA_VM_LOOP(vexSqrt(A[l])) break;
    case Op::Not:
        NOVA_VM_LOOP(A[l] == 0.0f ? 1.0f : 0.0f) break;
    default: // transcendental
        NOVA_VM_SCALAR_LOOP(evalUnary(op, A[l])) break;
    }
}

template <int N>
NOVA_INLINE void binary(Op op, float *D, const float *A, const float *B) {
    switch (op) {
    case Op::Add:
        NOVA_VM_LOOP(A[l] + B[l]) break;
    case Op::Sub:
        NOVA_VM_LOOP(A[l] - B[l]) break;
    case Op::Mul:
        NOVA_VM_LOOP(A[l] * B[l]) break;
    case Op::Div:
        NOVA_VM_LOOP(vexDiv(A[l], B[l])) break;
    case Op::Mod:
        NOVA_VM_LOOP(vexMod(A[l], B[l])) break;
    case Op::Min:
        NOVA_VM_LOOP(B[l] < A[l] ? B[l] : A[l]) break;
    case Op::Max:
        NOVA_VM_LOOP(B[l] > A[l] ? B[l] : A[l]) break;
    case Op::Greater:
        NOVA_VM_LOOP(A[l] > B[l] ? 1.0f : 0.0f) break;
    case Op::GreaterEq:
        NOVA_VM_LOOP(A[l] >= B[l] ? 1.0f : 0.0f) break;
    case Op::Equal:
        NOVA_VM_LOOP(A[l] == B[l] ? 1.0f : 0.0f) break;
    case Op::And:
        NOVA_VM_LOOP(A[l] != 0.0f && B[l] != 0.0f ? 1.0f : 0.0f) break;
    case Op::Or:
        NOVA_VM_LOOP(A[l] != 0.0f || B[l] != 0.0f ? 1.0f : 0.0f) break;
    default: // Pow, Atan2
        NOVA_VM_SCALAR_LOOP(evalBinary(op, A[l], B[l])) break;
    }
}

template <int N>
NOVA_INLINE void ternary(Op op, float *D, const float *A, const float *B, const float *C) {
    switch (op) {
    case Op::Clamp:
        NOVA_VM_LOOP(vexClamp(A[l], B[l], C[l])) break;
    case Op::Mix:
        NOVA_VM_LOOP(A[l] + (B[l] - A[l]) * C[l]) break;
    case Op::Smoothstep:
        NOVA_VM_LOOP(vexSmoothstep(A[l], B[l], C[l])) break;
    default: // Select
        NOVA_VM_LOOP(A[l] != 0.0f ? B[l] : C[l]) break;
    }
}

// Rare, large operations stay out of the executor's loop, so its hot part
// stays small (NOVA_OUTLINE).

// Procedurals: per lane, gather the operands, run the scalar procedural, scatter.
template <int N>
NOVA_OUTLINE void procedural(const Instr &in, float *R, const float *U) {
    const int w = in.width;
    const bool ua = in.flags & kUniformA, ub = in.flags & kUniformB, uc = in.flags & kUniformC;
    const int wa = in.op == Op::RgbToHsv || in.op == Op::HsvToRgb || in.op == Op::Flake ? 3 : int(in.imm & 15u);
    const int wb = in.op == Op::Fractal || in.op == Op::Worley || in.op == Op::Flake || in.op == Op::Voronoi ? 1 : 0;
    const int wc = in.op == Op::Fractal ? 2 : 0;
    const float *A[4], *B[1] = {nullptr}, *C[2] = {nullptr, nullptr};
    float tA[4][N], tB[N], tC[2][N];
    for (int k = 0; k < wa; ++k)
        A[k] = operand<N>(R, U, in.a, ua, false, k, tA[k]);
    if (wb)
        B[0] = operand<N>(R, U, in.b, ub, false, 0, tB);
    for (int k = 0; k < wc; ++k)
        C[k] = operand<N>(R, U, in.c, uc, false, k, tC[k]);
    for (int l = 0; l < N; ++l) {
        float a[4] = {0.0f, 0.0f, 0.0f, 0.0f}, b[1] = {0.0f}, c[2] = {0.0f, 0.0f}, o[4];
        for (int k = 0; k < wa; ++k)
            a[k] = A[k][l];
        if (wb)
            b[0] = B[0][l];
        for (int k = 0; k < wc; ++k)
            c[k] = C[k][l];
        evalProcedural(in.op, in.imm, a, b, c, o);
        for (int k = 0; k < w; ++k)
            R[size_t(in.dst + k) * N + l] = o[k];
    }
}

// Query: one call for the lanes (core/shading/query.h); the defaults without
// an implementation.
template <int N>
NOVA_OUTLINE void query(const Instr &in, float *R, const float *U, const TextureContext *tex) {
    const bool ua = in.flags & kUniformA, ub = in.flags & kUniformB;
    float t[N];
    float dir[N * 3], params[N * 4], out[N * 4];
    for (int k = 0; k < 3; ++k) {
        const float *a = operand<N>(R, U, in.a, ua, false, k, t);
        for (int l = 0; l < N; ++l)
            dir[l * 3 + k] = a[l];
    }
    for (int k = 0; k < 4; ++k) {
        const float *b = operand<N>(R, U, in.b, ub, false, k, t);
        for (int l = 0; l < N; ++l)
            params[l * 4 + k] = b[l];
    }
    const QueryKind kind = QueryKind(in.imm);
#if !defined(__CUDA_ARCH__)
    if (tex && tex->queries && tex->lanes) {
        tex->queries->run(kind, N, tex->lanes, dir, params, out);
    } else
#endif
    {
        for (int l = 0; l < N; ++l)
            queryDefault(kind, dir + l * 3, out + l * 4);
    }
    for (int k = 0; k < in.width; ++k)
        for (int l = 0; l < N; ++l)
            R[size_t(in.dst + k) * N + l] = out[l * 4 + k];
}

// Tex2D: one sampler call for the lanes; the fallback where a lane has no texel.
template <int N>
NOVA_OUTLINE void texture2D(const Instr &in, float *R, const float *U, const TextureContext *tex) {
    const int w = in.width;
    const bool ua = in.flags & kUniformA, ub = in.flags & kUniformB, uc = in.flags & kUniformC;
    float ta[N], tb[N], tc[N];
    uint8_t status[N];
    float texels[4 * N];
    for (int l = 0; l < N; ++l)
        status[l] = 1;
#if !defined(__CUDA_ARCH__)
    if (tex && tex->sampler && in.imm < tex->count && tex->samplers[in.imm].texture != kNoTexture) {
        float s[N], t[N], d[4][N];
        const float *S = operand<N>(R, U, in.a, ua, false, 0, ta);
        const float *T = operand<N>(R, U, in.a, ua, false, 1, tb);
        for (int l = 0; l < N; ++l) {
            s[l] = S[l];
            t[l] = T[l];
        }
        TexQuery q;
        q.count = N;
        q.s = s;
        q.t = t;
        float rnd[N];
        if (tex->rnd) {
            // One sample per hit, decorrelated between slots.
            const float shift = float(in.imm) * 0.618034f;
            for (int l = 0; l < N; ++l) {
                const float r = tex->rnd[l] + shift;
                rnd[l] = r - std::floor(r);
            }
            q.rnd = rnd;
        }
        if (in.flags & kHasB) {
            for (int k = 0; k < 4; ++k) {
                const float *B = operand<N>(R, U, in.b, ub, false, k, tc);
                for (int l = 0; l < N; ++l)
                    d[k][l] = B[l];
            }
            q.dsdx = d[0];
            q.dtdx = d[1];
            q.dsdy = d[2];
            q.dtdy = d[3];
        }
        tex->sampler->sample2D(tex->samplers[in.imm], q, texels, status);
    }
#else
    // The GPU (docs/design/M10.md D10): the slot's device texture, lane by lane.
    if (tex && tex->device && in.imm < tex->count) {
        const float *S = operand<N>(R, U, in.a, ua, false, 0, ta);
        const float *T = operand<N>(R, U, in.a, ua, false, 1, tb);
        float d[4][N];
        if (in.flags & kHasB)
            for (int k = 0; k < 4; ++k) {
                const float *B = operand<N>(R, U, in.b, ub, false, k, tc);
                for (int l = 0; l < N; ++l)
                    d[k][l] = B[l];
            }
        const bool derivatives = (in.flags & kHasB) != 0;
        for (int l = 0; l < N; ++l) {
            float r[4], dv[4];
            if (derivatives)
                for (int k = 0; k < 4; ++k)
                    dv[k] = d[k][l];
            if (sampleDeviceTexture(tex->device[in.imm], S[l], T[l], derivatives ? dv : nullptr, r)) {
                status[l] = 0;
                for (int k = 0; k < w; ++k)
                    texels[k * N + l] = r[k];
            }
        }
    }
#endif
    for (int k = 0; k < w; ++k) {
        const float *F = operand<N>(R, U, in.c, uc, false, k, tc);
        float *D = R + size_t(in.dst + k) * N;
        for (int l = 0; l < N; ++l)
            D[l] = status[l] ? F[l] : texels[k * N + l];
    }
}

#undef NOVA_VM_LOOP
#undef NOVA_VM_SCALAR_LOOP

} // namespace vm_detail

template <int N>
NOVA_HOST_DEVICE void execute(const Instr *code, size_t count, float *R, const float *U,
                              const TextureContext *tex = nullptr) {
    using namespace vm_detail;
    float ta[N], tb[N], tc[N];
    for (size_t i = 0; i < count; ++i) {
        const Instr &in = code[i];
        const int w = in.width;
        const bool ua = in.flags & kUniformA, ub = in.flags & kUniformB, uc = in.flags & kUniformC;
        const bool sa = in.flags & kSplatA, sb = in.flags & kSplatB, sc = in.flags & kSplatC;
        switch (opClass(in.op)) {
        case OpClass::Unary:
            for (int k = 0; k < w; ++k)
                unary<N>(in.op, R + size_t(in.dst + k) * N, operand<N>(R, U, in.a, ua, sa, k, ta));
            break;
        case OpClass::Binary:
            for (int k = 0; k < w; ++k)
                binary<N>(in.op, R + size_t(in.dst + k) * N, operand<N>(R, U, in.a, ua, sa, k, ta),
                          operand<N>(R, U, in.b, ub, sb, k, tb));
            break;
        case OpClass::Ternary:
            for (int k = 0; k < w; ++k)
                ternary<N>(in.op, R + size_t(in.dst + k) * N, operand<N>(R, U, in.a, ua, sa, k, ta),
                           operand<N>(R, U, in.b, ub, sb, k, tb), operand<N>(R, U, in.c, uc, sc, k, tc));
            break;
        case OpClass::Reduce: {
            const int wa = int(in.imm);
            float *D = R + size_t(in.dst) * N;
            float acc[N];
            for (int l = 0; l < N; ++l)
                acc[l] = 0.0f;
            for (int k = 0; k < wa; ++k) {
                const float *A = operand<N>(R, U, in.a, ua, false, k, ta);
                if (in.op == Op::Length) {
                    for (int l = 0; l < N; ++l)
                        acc[l] += A[l] * A[l];
                } else {
                    const float *B = operand<N>(R, U, in.b, ub, false, k, tb);
                    if (in.op == Op::Dot) {
                        for (int l = 0; l < N; ++l)
                            acc[l] += A[l] * B[l];
                    } else {
                        for (int l = 0; l < N; ++l)
                            acc[l] += (A[l] - B[l]) * (A[l] - B[l]);
                    }
                }
            }
            if (in.op == Op::Dot) {
                for (int l = 0; l < N; ++l)
                    D[l] = acc[l];
            } else {
                for (int l = 0; l < N; ++l)
                    D[l] = std::sqrt(acc[l]);
            }
            break;
        }
        default:
            switch (in.op) {
            case Op::Normalize: {
                float acc[N];
                for (int l = 0; l < N; ++l)
                    acc[l] = 0.0f;
                for (int k = 0; k < w; ++k) {
                    const float *A = operand<N>(R, U, in.a, ua, false, k, ta);
                    for (int l = 0; l < N; ++l)
                        acc[l] += A[l] * A[l];
                }
                for (int l = 0; l < N; ++l)
                    acc[l] = acc[l] > 0.0f ? 1.0f / std::sqrt(acc[l]) : 0.0f;
                for (int k = 0; k < w; ++k) {
                    const float *A = operand<N>(R, U, in.a, ua, false, k, ta);
                    float *D = R + size_t(in.dst + k) * N;
                    for (int l = 0; l < N; ++l)
                        D[l] = A[l] * acc[l];
                }
                break;
            }
            case Op::Cross: {
                float a[3][N], b[3][N];
                for (int k = 0; k < 3; ++k) {
                    const float *A = operand<N>(R, U, in.a, ua, false, k, ta);
                    const float *B = operand<N>(R, U, in.b, ub, false, k, tb);
                    for (int l = 0; l < N; ++l) {
                        a[k][l] = A[l];
                        b[k][l] = B[l];
                    }
                }
                float *D0 = R + size_t(in.dst) * N, *D1 = D0 + N, *D2 = D1 + N;
                for (int l = 0; l < N; ++l) {
                    D0[l] = a[1][l] * b[2][l] - a[2][l] * b[1][l];
                    D1[l] = a[2][l] * b[0][l] - a[0][l] * b[2][l];
                    D2[l] = a[0][l] * b[1][l] - a[1][l] * b[0][l];
                }
                break;
            }
            case Op::Swizzle: {
                // Components may repeat or reorder: read all first.
                float s[4][N];
                for (int k = 0; k < w; ++k) {
                    const float *A = operand<N>(R, U, in.a, ua, false, swizzleComponent(in.imm, k), ta);
                    for (int l = 0; l < N; ++l)
                        s[k][l] = A[l];
                }
                for (int k = 0; k < w; ++k) {
                    float *D = R + size_t(in.dst + k) * N;
                    for (int l = 0; l < N; ++l)
                        D[l] = s[k][l];
                }
                break;
            }
            case Op::Noise:
            case Op::Fractal:
            case Op::CellNoise:
            case Op::Worley:
            case Op::RgbToHsv:
            case Op::HsvToRgb:
            case Op::Flake:
            case Op::Voronoi:
            case Op::Blackbody:
                procedural<N>(in, R, U);
                break;
            case Op::Tex2D:
                texture2D<N>(in, R, U, tex);
                break;
            case Op::Query:
                query<N>(in, R, U, tex);
                break;
            case Op::Concat: {
                const int wa = int(in.imm);
                for (int k = 0; k < w; ++k) {
                    const float *S = k < wa ? operand<N>(R, U, in.a, ua, false, k, ta)
                                            : operand<N>(R, U, in.b, ub, false, k - wa, tb);
                    float *D = R + size_t(in.dst + k) * N;
                    for (int l = 0; l < N; ++l)
                        D[l] = S[l];
                }
                break;
            }
            default:
                break;
            }
            break;
        }
    }
}

// Runs the prologue: every operand and destination is a slot of U.
inline void executePrologue(const Program &p, float *U) { execute<1>(p.prologue.data(), p.prologue.size(), U, U); }

} // namespace nova::shading
