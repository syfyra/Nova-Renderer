// Throwaway microbenchmark: per-lane (Cycles-SVM-style) vs. SoA batch
// interpretation of the same flat bytecode, plus a native upper bound.
// Build: cl /O2 /arch:AVX2 /std:c++20 /EHsc /fp:precise bench_vm.cpp
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <random>

#if defined(_MSC_VER)
#define RESTRICT __restrict
#define FORCEINLINE __forceinline
#else
#define RESTRICT __restrict__
#define FORCEINLINE inline __attribute__((always_inline))
#endif

enum Op : uint8_t {
    OP_END, OP_CONST, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MAD, OP_MIX, OP_CLAMP, OP_MIN, OP_MAX,
    OP_FLOOR, OP_FRACT, OP_SIN, OP_POW, OP_DOT3, OP_NORMALIZE3, OP_SMOOTHSTEP, OP_SELECT_GT, OP_COUNT
};

// 8-byte instruction. Operands are scalar register rows; a vecN value
// occupies N consecutive rows. flags bit k: operand k is a scalar broadcast
// over the components (vec3 * float).
struct Instr {
    uint8_t op, width, dst, a, b, c, flags, imm; // imm: constant-pool index for OP_CONST
};
static_assert(sizeof(Instr) == 8);

struct Program {
    std::vector<Instr> code;
    std::vector<float> consts;
    int numRegs = 0;
};

// ---- scalar op semantics, shared by both executors (NOVA_HOST_DEVICE in Nova)
FORCEINLINE float fract_(float x) { return x - std::floor(x); }
FORCEINLINE float mix_(float a, float b, float t) { return a + (b - a) * t; }
#ifdef TERNARY
FORCEINLINE float fmin_(float a, float b) { return a < b ? a : b; }
FORCEINLINE float fmax_(float a, float b) { return a > b ? a : b; }
#else
FORCEINLINE float fmin_(float a, float b) { return std::fmin(a, b); }
FORCEINLINE float fmax_(float a, float b) { return std::fmax(a, b); }
#endif
FORCEINLINE float clamp_(float x, float lo, float hi) { return fmin_(fmax_(x, lo), hi); }
FORCEINLINE float smooth_(float lo, float hi, float x) {
    float t = clamp_((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
FORCEINLINE float pow_(float a, float b) { return a > 0.0f ? std::pow(a, b) : 0.0f; }

// ------------------------------------------------------------ per-lane VM
// Registers: float regs[256] per lane (Cycles: float stack[SVM_STACK_SIZE]).
void runScalar(const Program &p, float *RESTRICT regs) {
    const Instr *ip = p.code.data();
    for (;; ++ip) {
        const Instr in = *ip;
        float *d = regs + in.dst;
        const float *a = regs + in.a, *b = regs + in.b, *c = regs + in.c;
        const int w = in.width;
        const int sa = (in.flags & 1) ? 0 : 1, sb = (in.flags & 2) ? 0 : 1, sc = (in.flags & 4) ? 0 : 1;
        switch (in.op) {
        case OP_END: return;
        case OP_CONST: for (int k = 0; k < w; ++k) d[k] = p.consts[in.imm + k]; break;
        case OP_ADD: for (int k = 0; k < w; ++k) d[k] = a[k * sa] + b[k * sb]; break;
        case OP_SUB: for (int k = 0; k < w; ++k) d[k] = a[k * sa] - b[k * sb]; break;
        case OP_MUL: for (int k = 0; k < w; ++k) d[k] = a[k * sa] * b[k * sb]; break;
        case OP_DIV: for (int k = 0; k < w; ++k) d[k] = b[k * sb] != 0.0f ? a[k * sa] / b[k * sb] : 0.0f; break;
        case OP_MAD: for (int k = 0; k < w; ++k) d[k] = a[k * sa] * b[k * sb] + c[k * sc]; break;
        case OP_MIX: for (int k = 0; k < w; ++k) d[k] = mix_(a[k * sa], b[k * sb], c[k * sc]); break;
        case OP_CLAMP: for (int k = 0; k < w; ++k) d[k] = clamp_(a[k * sa], b[k * sb], c[k * sc]); break;
        case OP_MIN: for (int k = 0; k < w; ++k) d[k] = std::fmin(a[k * sa], b[k * sb]); break;
        case OP_MAX: for (int k = 0; k < w; ++k) d[k] = std::fmax(a[k * sa], b[k * sb]); break;
        case OP_FLOOR: for (int k = 0; k < w; ++k) d[k] = std::floor(a[k]); break;
        case OP_FRACT: for (int k = 0; k < w; ++k) d[k] = fract_(a[k]); break;
        case OP_SIN: for (int k = 0; k < w; ++k) d[k] = std::sin(a[k]); break;
        case OP_POW: for (int k = 0; k < w; ++k) d[k] = pow_(a[k * sa], b[k * sb]); break;
        case OP_DOT3: d[0] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
        case OP_NORMALIZE3: {
            float l2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
            float s = l2 > 0.0f ? 1.0f / std::sqrt(l2) : 0.0f;
            d[0] = a[0] * s; d[1] = a[1] * s; d[2] = a[2] * s;
        } break;
        case OP_SMOOTHSTEP: for (int k = 0; k < w; ++k) d[k] = smooth_(a[k * sa], b[k * sb], c[k * sc]); break;
        case OP_SELECT_GT: for (int k = 0; k < w; ++k) d[k] = a[k * sa] > b[k * sb] ? c[k * sc] : 0.0f; break;
        }
    }
}

// ------------------------------------------------------------ batch VM
// Registers: row r of lane l at regs[r * N + l]. Rows of a vecN are
// adjacent, so component-wise ops run over w*N contiguous floats.
template <int N>
void runBatch(const Program &p, float *RESTRICT regs) {
    const Instr *ip = p.code.data();
    for (;; ++ip) {
        const Instr in = *ip;
        float *RESTRICT d = regs + in.dst * N;
        const float *RESTRICT a = regs + in.a * N;
        const float *RESTRICT b = regs + in.b * N;
        const float *RESTRICT c = regs + in.c * N;
        const int w = in.width;
        switch (in.op) {
        case OP_END: return;
        case OP_CONST:
            for (int k = 0; k < w; ++k) {
                const float v = p.consts[in.imm + k];
                for (int l = 0; l < N; ++l) d[k * N + l] = v;
            }
            break;
#define BIN(OPC, EXPR)                                                                          \
    case OPC:                                                                                   \
        if (in.flags == 0) {                                                                    \
            for (int i = 0; i < w * N; ++i) { const float x = a[i], y = b[i]; d[i] = (EXPR); }  \
        } else {                                                                                \
            const int sa = (in.flags & 1) ? 0 : N, sb = (in.flags & 2) ? 0 : N;                 \
            for (int k = 0; k < w; ++k) {                                                       \
                const float *RESTRICT ak = a + k * sa, *RESTRICT bk = b + k * sb;               \
                float *RESTRICT dk = d + k * N;                                                 \
                for (int l = 0; l < N; ++l) { const float x = ak[l], y = bk[l]; dk[l] = (EXPR); } \
            }                                                                                   \
        }                                                                                       \
        break;
#define TER(OPC, EXPR)                                                                          \
    case OPC: {                                                                                 \
        const int sa = (in.flags & 1) ? 0 : N, sb = (in.flags & 2) ? 0 : N, sc = (in.flags & 4) ? 0 : N; \
        for (int k = 0; k < w; ++k) {                                                           \
            const float *RESTRICT ak = a + k * sa, *RESTRICT bk = b + k * sb, *RESTRICT ck = c + k * sc; \
            float *RESTRICT dk = d + k * N;                                                     \
            for (int l = 0; l < N; ++l) { const float x = ak[l], y = bk[l], z = ck[l]; dk[l] = (EXPR); } \
        }                                                                                       \
    } break;
#define UN(OPC, EXPR) \
    case OPC: for (int i = 0; i < w * N; ++i) { const float x = a[i]; d[i] = (EXPR); } break;
            BIN(OP_ADD, x + y)
            BIN(OP_SUB, x - y)
            BIN(OP_MUL, x * y)
            BIN(OP_DIV, y != 0.0f ? x / y : 0.0f)
            BIN(OP_MIN, fmin_(x, y))
            BIN(OP_MAX, fmax_(x, y))
            BIN(OP_POW, pow_(x, y))
            TER(OP_MAD, x * y + z)
            TER(OP_MIX, mix_(x, y, z))
            TER(OP_CLAMP, clamp_(x, y, z))
            TER(OP_SMOOTHSTEP, smooth_(x, y, z))
            TER(OP_SELECT_GT, x > y ? z : 0.0f)
            UN(OP_FLOOR, std::floor(x))
            UN(OP_FRACT, fract_(x))
            UN(OP_SIN, std::sin(x))
        case OP_DOT3:
            for (int l = 0; l < N; ++l)
                d[l] = a[l] * b[l] + a[N + l] * b[N + l] + a[2 * N + l] * b[2 * N + l];
            break;
        case OP_NORMALIZE3:
            for (int l = 0; l < N; ++l) {
                const float x = a[l], y = a[N + l], z = a[2 * N + l];
                const float l2 = x * x + y * y + z * z;
                const float s = l2 > 0.0f ? 1.0f / std::sqrt(l2) : 0.0f;
                d[l] = x * s; d[N + l] = y * s; d[2 * N + l] = z * s;
            }
            break;
        }
    }
}

// ------------------------------------------------------------ program
// rows 0..7 are inputs: u, v, P.xyz, N.xyz
void Program_pushConst(Program &p, int dst, std::initializer_list<float> v) {
    Instr in{OP_CONST, uint8_t(v.size()), uint8_t(dst), 0, 0, 0, 0, uint8_t(p.consts.size())};
    p.consts.insert(p.consts.end(), v);
    p.code.push_back(in);
}

struct B {
    Program p;
    int next = 8;
    int reg(int w) { int r = next; next += w; p.numRegs = std::max(p.numRegs, next); return r; }
    int k(std::initializer_list<float> v) { int r = reg(int(v.size())); Program_pushConst(p, r, v); return r; }
    int op(Op o, int w, int a, int b = 0, int c = 0, int flags = 0, int dw = -1) {
        int r = reg(dw < 0 ? w : dw);
        p.code.push_back(Instr{uint8_t(o), uint8_t(w), uint8_t(r), uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(flags), 0});
        return r;
    }
};

// A textured-looking material: checker from uv, sine stripes from P,
// facing ratio from N, a few mixes. Constants first (uniform prologue).
Program makeProgram(int repeats) {
    B b;
    const int U = 0, V = 1, P = 2, Nn = 5;
    const int c4 = b.k({4.0f}), cHalf = b.k({0.5f}), c2 = b.k({2.0f}), c10 = b.k({10.0f});
    const int colA = b.k({0.8f, 0.1f, 0.1f}), colB = b.k({0.1f, 0.1f, 0.8f});
    const int c04 = b.k({0.4f}), c01 = b.k({0.1f}), c0 = b.k({0.0f}), c1 = b.k({1.0f}), c22 = b.k({2.2f});
    int out = colA;
    for (int r = 0; r < repeats; ++r) {
        int s = b.op(OP_MUL, 1, U, c4), t = b.op(OP_MUL, 1, V, c4);
        int ft = b.op(OP_FRACT, 1, t);
        int fls = b.op(OP_FLOOR, 1, s), flt = b.op(OP_FLOOR, 1, t);
        int sum = b.op(OP_ADD, 1, fls, flt);
        int m = b.op(OP_MUL, 1, sum, cHalf);
        int fm = b.op(OP_FRACT, 1, m);
        int chk = b.op(OP_MUL, 1, fm, c2);
        int col = b.op(OP_MIX, 3, colA, colB, chk, 4);
        int px = b.op(OP_MUL, 1, P, c10);
#ifdef NO_TRANSC
        int w = b.op(OP_FRACT, 1, px);
#else
        int w = b.op(OP_SIN, 1, px);
#endif
        int w2 = b.op(OP_MAD, 1, w, cHalf, cHalf);
        int rr = b.op(OP_MAD, 1, ft, c04, c01);
        int rough = b.op(OP_CLAMP, 1, rr, c0, c1);
#ifdef NO_TRANSC
        int spec = b.op(OP_MUL, 1, w2, c22);
#else
        int spec = b.op(OP_POW, 1, w2, c22);
#endif
        int pn = b.op(OP_MUL, 3, P, c01, 0, 2);
        int nsum = b.op(OP_ADD, 3, Nn, pn);
        int nn = b.op(OP_NORMALIZE3, 3, nsum);
        int d = b.op(OP_DOT3, 3, nn, P, 0, 0, 1);
        int facing = b.op(OP_SMOOTHSTEP, 1, c0, c1, d);
        int col2 = b.op(OP_MUL, 3, col, facing, 0, 2);
        int col3 = b.op(OP_MIX, 3, col, col2, rough, 4);
        int col4 = b.op(OP_MUL, 3, col3, spec, 0, 2);
        out = b.op(OP_ADD, 3, out, col4);
    }
    b.p.code.push_back(Instr{OP_END, 0, 0, 0, 0, 0, 0, 0});
    // the output color lives in rows out..out+2
    b.p.consts.push_back(float(out)); // stash
    return b.p;
}

// Native equivalent for one repeat block (upper bound), vectorizable over lanes.
template <int N>
void runNative(const float *RESTRICT in, float *RESTRICT outc, int repeats) {
    const float *U = in, *V = in + N, *Px = in + 2 * N, *Py = in + 3 * N, *Pz = in + 4 * N;
    const float *Nx = in + 5 * N, *Ny = in + 6 * N, *Nz = in + 7 * N;
    for (int l = 0; l < N; ++l) {
        float o0 = 0.8f, o1 = 0.1f, o2 = 0.1f;
        for (int r = 0; r < repeats; ++r) {
            float s = U[l] * 4.0f, t = V[l] * 4.0f;
            float ft = fract_(t);
            float chk = fract_((std::floor(s) + std::floor(t)) * 0.5f) * 2.0f;
            float c0 = mix_(0.8f, 0.1f, chk), c1 = mix_(0.1f, 0.1f, chk), c2 = mix_(0.1f, 0.8f, chk);
#ifdef NO_TRANSC
            float w2 = fract_(Px[l] * 10.0f) * 0.5f + 0.5f;
#else
            float w2 = std::sin(Px[l] * 10.0f) * 0.5f + 0.5f;
#endif
            float rough = clamp_(ft * 0.4f + 0.1f, 0.0f, 1.0f);
#ifdef NO_TRANSC
            float spec = w2 * 2.2f;
#else
            float spec = pow_(w2, 2.2f);
#endif
            float nx = Nx[l] + Px[l] * 0.1f, ny = Ny[l] + Py[l] * 0.1f, nz = Nz[l] + Pz[l] * 0.1f;
            float l2 = nx * nx + ny * ny + nz * nz;
            float is = l2 > 0.0f ? 1.0f / std::sqrt(l2) : 0.0f;
            float d = (nx * Px[l] + ny * Py[l] + nz * Pz[l]) * is;
            float f = smooth_(0.0f, 1.0f, d);
            o0 += mix_(c0, c0 * f, rough) * spec;
            o1 += mix_(c1, c1 * f, rough) * spec;
            o2 += mix_(c2, c2 * f, rough) * spec;
        }
        outc[l] = o0; outc[N + l] = o1; outc[2 * N + l] = o2;
    }
}

using Clock = std::chrono::high_resolution_clock;

template <int N>
void benchBatch(const Program &p, const std::vector<float> &inputs, int totalLanes, int instrCount, int outRow) {
    alignas(64) static float regs[256 * N];
    double sink = 0;
    const int chunks = totalLanes / N;
    auto t0 = Clock::now();
    for (int iter = 0; iter < 20; ++iter)
        for (int ch = 0; ch < chunks; ++ch) {
            // load the 8 input rows (geometry) for this chunk: SoA copy
            for (int r = 0; r < 8; ++r)
                std::memcpy(regs + r * N, inputs.data() + size_t(r) * totalLanes + size_t(ch) * N, N * sizeof(float));
            runBatch<N>(p, regs);
            sink += regs[outRow * N];
        }
    auto t1 = Clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (20.0 * chunks * N);
    std::printf("batch N=%3d : %7.2f ns/lane  %6.3f ns/(instr*lane)   (sink %g)\n", N, ns, ns / instrCount, sink);
}

template <int N>
void benchNative(const std::vector<float> &inputs, int totalLanes, int repeats, int instrCount) {
    alignas(64) static float in[8 * N];
    alignas(64) static float out[3 * N];
    double sink = 0;
    const int chunks = totalLanes / N;
    auto t0 = Clock::now();
    for (int iter = 0; iter < 20; ++iter)
        for (int ch = 0; ch < chunks; ++ch) {
            for (int r = 0; r < 8; ++r)
                std::memcpy(in + r * N, inputs.data() + size_t(r) * totalLanes + size_t(ch) * N, N * sizeof(float));
            runNative<N>(in, out, repeats);
            sink += out[0];
        }
    auto t1 = Clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (20.0 * chunks * N);
    std::printf("native N=%3d: %7.2f ns/lane  %6.3f ns/(instr*lane)   (sink %g)\n", N, ns, ns / instrCount, sink);
}

int main() {
    for (int repeats : {1, 4}) {
        Program p = makeProgram(repeats);
        const int outRow = int(p.consts.back());
        p.consts.pop_back();
        const int instrCount = int(p.code.size()) - 1;
        int scalarOps = 0;
        for (auto &i : p.code) scalarOps += (i.op == OP_DOT3 || i.op == OP_NORMALIZE3) ? 1 : i.width;
        std::printf("\n=== program: %d instructions (%d component-ops), %d registers, %zu bytes code\n", instrCount,
                    scalarOps, p.numRegs, p.code.size() * sizeof(Instr));
        const int totalLanes = 1 << 16;
        std::vector<float> inputs(size_t(8) * totalLanes);
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> U(-1.0f, 1.0f);
        for (auto &v : inputs) v = U(rng);

        // per-lane interpreter
        {
            alignas(64) float regs[256];
            double sink = 0;
            auto t0 = Clock::now();
            for (int iter = 0; iter < 20; ++iter)
                for (int l = 0; l < totalLanes; ++l) {
                    for (int r = 0; r < 8; ++r) regs[r] = inputs[size_t(r) * totalLanes + l];
                    runScalar(p, regs);
                    sink += regs[outRow];
                }
            auto t1 = Clock::now();
            double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (20.0 * totalLanes);
            std::printf("per-lane    : %7.2f ns/lane  %6.3f ns/(instr*lane)   (sink %g)\n", ns, ns / instrCount, sink);
        }
        benchBatch<8>(p, inputs, totalLanes, instrCount, outRow);
        benchBatch<16>(p, inputs, totalLanes, instrCount, outRow);
        benchBatch<32>(p, inputs, totalLanes, instrCount, outRow);
        benchBatch<64>(p, inputs, totalLanes, instrCount, outRow);
        benchBatch<128>(p, inputs, totalLanes, instrCount, outRow);
        benchNative<32>(inputs, totalLanes, repeats, instrCount);
        benchNative<64>(inputs, totalLanes, repeats, instrCount);

        // correctness: batch vs per-lane on the first 64 lanes
        alignas(64) static float regsB[256 * 64];
        for (int r = 0; r < 8; ++r) std::memcpy(regsB + r * 64, inputs.data() + size_t(r) * totalLanes, 64 * 4);
        runBatch<64>(p, regsB);
        double maxErr = 0;
        for (int l = 0; l < 64; ++l) {
            float regs[256];
            for (int r = 0; r < 8; ++r) regs[r] = inputs[size_t(r) * totalLanes + l];
            runScalar(p, regs);
            for (int k = 0; k < 3; ++k) maxErr = std::max(maxErr, double(std::fabs(regs[outRow + k] - regsB[(outRow + k) * 64 + l])));
        }
        std::printf("max |batch - per-lane| = %g\n", maxErr);
    }

    // Counting sort of 1024 hits by material id (shade-stage sort cost).
    {
        const int n = 1024, materials = 16;
        std::vector<uint16_t> mat(n);
        std::vector<uint32_t> order(n), counts(materials + 1);
        std::mt19937 rng(2);
        for (auto &m : mat) m = uint16_t(rng() % materials);
        auto t0 = Clock::now();
        uint64_t sink = 0;
        const int iters = 100000;
        for (int it = 0; it < iters; ++it) {
            std::fill(counts.begin(), counts.end(), 0u);
            for (int i = 0; i < n; ++i) counts[mat[i] + 1]++;
            for (int m = 0; m < materials; ++m) counts[m + 1] += counts[m];
            for (int i = 0; i < n; ++i) order[counts[mat[i]]++] = uint32_t(i);
            sink += order[it & (n - 1)];
            mat[it & (n - 1)] ^= 1;
        }
        auto t1 = Clock::now();
        std::printf("\ncounting sort 1024 hits / 16 materials: %.2f ns per hit (sink %llu)\n",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / (double(iters) * n),
                    (unsigned long long)sink);
    }
    return 0;
}
