// M10 research probe (docs/research/m10/cuda-wavefront.md, section 3): Nova's
// shading bytecode interpreter (core/shading/vm.h, execute<1>) on the GPU, one
// lane per thread, against a hand-written kernel of the same math and against
// the CPU batch executor (execute<32>) on the same program.
//
// The program is a representative textured surface: 60 instructions (3 Tex2D,
// 1 Perlin noise, 2 HSV conversions, pow/sin/exp/atan2, normal mapping) over
// 72 rows. On the device vm.h's Tex2D returns its fallback (no device sampler
// yet), so the "tex" variants split the program at its Tex2D instructions and
// do the lookups with CUDA texture objects (tex2DGrad) between the segments,
// which is what a device branch in vm.h would do.
//
// Experiments:
//   A  1M lanes: VM (fallback textures) / VM + hardware textures / hand-written,
//      register file in local or shared memory, uniforms and code in global or
//      constant memory; coherent and shuffled lanes.
//   B  divergence: K structurally different random programs (60 instructions),
//      lanes sorted by program or not; K materials sharing one program with
//      different uniforms (global vs constant memory).
//   C  CPU: execute<32> and execute<1> on one thread and on all threads.
//   D  agreement: VM vs hand-written on the GPU, GPU vs CPU (fallback textures).
//
// Build: cw_build.bat (nvcc 12.9, sm_120, MSVC host compiler). Run: cw_vm_probe.exe

#include "core/shading/vm.h"

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace nova;
using namespace nova::shading;

#define CK(x)                                                                                                          \
    do {                                                                                                               \
        cudaError_t e_ = (x);                                                                                          \
        if (e_ != cudaSuccess) {                                                                                       \
            std::printf("CUDA error %s at %s:%d\n", cudaGetErrorString(e_), __FILE__, __LINE__);                    \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// The program
// ---------------------------------------------------------------------------

constexpr int kRows = 72;     // rowCount of the representative program
constexpr int kInputs = 15;   // P(3) N(3) uv(2) I(3) footprint derivatives(4)
constexpr int kOutputs = 15;  // col(3) rough N(3) spec(3) emission(3) coat opacity
constexpr int kRowP = 0, kRowN = 3, kRowUv = 6, kRowI = 8, kRowDeriv = 68;
// The rows of the outputs.
NOVA_INLINE int outRow(int k) {
    constexpr int rows[kOutputs] = {28, 29, 30, 50, 44, 45, 46, 54, 55, 56, 58, 59, 60, 63, 67};
    return rows[k];
}

struct Opnd {
    uint16_t index = 0;
    bool uniform = false;
    bool splat = false;
};
static Opnd R_(int r, bool splat = false) { return {uint16_t(r), false, splat}; }

struct ProgramBuilder {
    std::vector<Instr> code;
    std::vector<float> U;
    uint16_t u(float v) {
        U.push_back(v);
        return uint16_t(U.size() - 1);
    }
    uint16_t u2(float a, float b) {
        const uint16_t i = u(a);
        u(b);
        return i;
    }
    uint16_t u3(float a, float b, float c) {
        const uint16_t i = u(a);
        u(b);
        u(c);
        return i;
    }
    void emit(Op op, int w, int dst, Opnd a, Opnd b = {}, Opnd c = {}, uint32_t imm = 0, uint8_t extra = 0) {
        Instr in{};
        in.op = op;
        in.width = uint8_t(w);
        in.flags = uint8_t((a.uniform ? kUniformA : 0) | (b.uniform ? kUniformB : 0) | (c.uniform ? kUniformC : 0) |
                           (a.splat ? kSplatA : 0) | (b.splat ? kSplatB : 0) | (c.splat ? kSplatC : 0) | extra);
        in.dst = uint16_t(dst);
        in.a = a.index;
        in.b = b.index;
        in.c = c.index;
        in.imm = imm;
        code.push_back(in);
    }
};
static Opnd Uo(uint16_t i, bool splat = false) { return {i, true, splat}; }

// Uniform slots of the representative program (filled by buildProgram).
struct Slots {
    uint16_t uvScale, uvOffset, baseFb, roughFb, nrmFb, freq, half, gamma, tint, gain, zero, one, hueShift, lumW, sat,
        two, up, lo, hi, rmin, rmax, five, f0, specTint, metal, k, emit, coatA, coatB, stripes;
};

static void buildProgram(ProgramBuilder &b, Slots &s) {
    s.uvScale = b.u2(4.0f, 4.0f);
    s.uvOffset = b.u2(0.13f, 0.27f);
    s.baseFb = b.u3(0.8f, 0.5f, 0.3f);
    s.roughFb = b.u(0.4f);
    s.nrmFb = b.u3(0.5f, 0.5f, 1.0f);
    s.freq = b.u(3.7f);
    s.half = b.u(0.5f);
    s.gamma = b.u(2.2f);
    s.tint = b.u3(0.9f, 0.7f, 0.6f);
    s.gain = b.u(1.3f);
    s.zero = b.u(0.0f);
    s.one = b.u(1.0f);
    s.hueShift = b.u(0.07f);
    s.lumW = b.u3(0.2126f, 0.7152f, 0.0722f);
    s.sat = b.u(0.8f);
    s.two = b.u(2.0f);
    s.up = b.u3(0.0f, 1.0f, 0.0f);
    s.lo = b.u(0.1f);
    s.hi = b.u(0.9f);
    s.rmin = b.u(0.05f);
    s.rmax = b.u(0.7f);
    s.five = b.u(5.0f);
    s.f0 = b.u(0.04f);
    s.specTint = b.u3(1.0f, 0.95f, 0.9f);
    s.metal = b.u(0.3f);
    s.k = b.u(6.0f);
    s.emit = b.u3(0.2f, 0.1f, 0.05f);
    s.coatA = b.u(0.25f);
    s.coatB = b.u(0.75f);
    s.stripes = b.u(3.0f);

    b.emit(Op::Mul, 2, 11, R_(kRowUv), Uo(s.uvScale));                                            // 1
    b.emit(Op::Add, 2, 11, R_(11), Uo(s.uvOffset));                                               // 2
    b.emit(Op::Tex2D, 3, 13, R_(11), R_(kRowDeriv), Uo(s.baseFb), 0, kHasB);                      // 3 base
    b.emit(Op::Tex2D, 1, 16, R_(11), R_(kRowDeriv), Uo(s.roughFb), 1, kHasB);                     // 4 rough
    b.emit(Op::Tex2D, 3, 17, R_(11), R_(kRowDeriv), Uo(s.nrmFb), 2, kHasB);                       // 5 normal map
    b.emit(Op::Mul, 3, 20, R_(kRowP), Uo(s.freq, true));                                          // 6
    b.emit(Op::Noise, 1, 23, R_(20), {}, {}, noiseImm(3, 1));                                     // 7
    b.emit(Op::Mul, 1, 24, R_(23), Uo(s.half));                                                   // 8
    b.emit(Op::Add, 1, 24, R_(24), Uo(s.half));                                                   // 9
    b.emit(Op::Pow, 3, 25, R_(13), Uo(s.gamma, true));                                            // 10
    b.emit(Op::Mix, 3, 28, R_(25), Uo(s.tint), R_(24, true));                                     // 11
    b.emit(Op::Mul, 3, 28, R_(28), Uo(s.gain, true));                                             // 12
    b.emit(Op::Clamp, 3, 28, R_(28), Uo(s.zero, true), Uo(s.one, true));                          // 13
    b.emit(Op::RgbToHsv, 3, 31, R_(28));                                                          // 14
    b.emit(Op::Add, 1, 31, R_(31), Uo(s.hueShift));                                               // 15
    b.emit(Op::Fract, 1, 31, R_(31));                                                             // 16
    b.emit(Op::HsvToRgb, 3, 28, R_(31));                                                          // 17
    b.emit(Op::Dot, 1, 34, R_(28), Uo(s.lumW), {}, 3);                                            // 18
    b.emit(Op::Mix, 3, 28, R_(34, true), R_(28), Uo(s.sat, true));                                // 19
    b.emit(Op::Mul, 3, 35, R_(17), Uo(s.two, true));                                              // 20
    b.emit(Op::Sub, 3, 35, R_(35), Uo(s.one, true));                                              // 21
    b.emit(Op::Normalize, 3, 35, R_(35));                                                         // 22
    b.emit(Op::Cross, 3, 38, R_(kRowN), Uo(s.up));                                                // 23
    b.emit(Op::Normalize, 3, 38, R_(38));                                                         // 24
    b.emit(Op::Cross, 3, 41, R_(kRowN), R_(38));                                                  // 25
    b.emit(Op::Mul, 3, 44, R_(38), R_(35, true));                                                 // 26
    b.emit(Op::Mul, 3, 47, R_(41), R_(36, true));                                                 // 27
    b.emit(Op::Add, 3, 44, R_(44), R_(47));                                                       // 28
    b.emit(Op::Mul, 3, 47, R_(kRowN), R_(37, true));                                              // 29
    b.emit(Op::Add, 3, 44, R_(44), R_(47));                                                       // 30
    b.emit(Op::Normalize, 3, 44, R_(44));                                                         // 31
    b.emit(Op::Smoothstep, 1, 50, Uo(s.lo), Uo(s.hi), R_(16));                                    // 32
    b.emit(Op::Mix, 1, 50, Uo(s.rmin), Uo(s.rmax), R_(50));                                       // 33
    b.emit(Op::Mul, 1, 50, R_(50), R_(50));                                                       // 34
    b.emit(Op::Clamp, 1, 50, R_(50), Uo(s.zero), Uo(s.one));                                      // 35
    b.emit(Op::Dot, 1, 51, R_(44), R_(kRowI), {}, 3);                                             // 36
    b.emit(Op::Neg, 1, 51, R_(51));                                                               // 37
    b.emit(Op::Max, 1, 51, R_(51), Uo(s.zero));                                                   // 38
    b.emit(Op::Sub, 1, 52, Uo(s.one), R_(51));                                                    // 39
    b.emit(Op::Pow, 1, 52, R_(52), Uo(s.five));                                                   // 40
    b.emit(Op::Mix, 1, 53, Uo(s.f0), Uo(s.one), R_(52));                                          // 41
    b.emit(Op::Mix, 3, 54, Uo(s.specTint), R_(28), Uo(s.metal, true));                            // 42
    b.emit(Op::Mul, 3, 54, R_(54), R_(53, true));                                                 // 43
    b.emit(Op::Mul, 1, 57, R_(23), Uo(s.k));                                                      // 44
    b.emit(Op::Sin, 1, 57, R_(57));                                                               // 45
    b.emit(Op::Abs, 1, 57, R_(57));                                                               // 46
    b.emit(Op::Mul, 3, 58, Uo(s.emit), R_(57, true));                                             // 47
    b.emit(Op::Length, 1, 61, R_(20), {}, {}, 3);                                                 // 48
    b.emit(Op::Fract, 1, 61, R_(61));                                                             // 49
    b.emit(Op::Greater, 1, 62, R_(61), Uo(s.half));                                               // 50
    b.emit(Op::Select, 1, 63, R_(62), Uo(s.coatA), Uo(s.coatB));                                  // 51
    b.emit(Op::Neg, 1, 64, R_(50));                                                               // 52
    b.emit(Op::Exp, 1, 64, R_(64));                                                               // 53
    b.emit(Op::Atan2, 1, 65, R_(12), R_(11));                                                     // 54
    b.emit(Op::Mul, 1, 65, R_(65), Uo(s.stripes));                                                // 55
    b.emit(Op::Floor, 1, 66, R_(65));                                                             // 56
    b.emit(Op::Mod, 1, 66, R_(66), Uo(s.two));                                                    // 57
    b.emit(Op::Mix, 1, 67, R_(64), R_(24), R_(66));                                               // 58
    b.emit(Op::Mul, 1, 67, R_(67), R_(63));                                                       // 59
    b.emit(Op::Clamp, 1, 67, R_(67), Uo(s.zero), Uo(s.one));                                      // 60
}

// Component operations of a program (an instruction of width w counts w).
static long componentOps(const std::vector<Instr> &code) {
    long n = 0;
    for (const Instr &in : code)
        n += in.width;
    return n;
}

// The program with its transcendental and procedural operations replaced by
// cheap ones of the same shape (calibration against M3's arithmetic numbers).
static std::vector<Instr> arithmeticOnly(std::vector<Instr> code) {
    for (Instr &in : code) {
        switch (in.op) {
        case Op::Pow:
        case Op::Atan2:
            in.op = Op::Mul;
            break;
        case Op::Sin:
        case Op::Exp:
            in.op = Op::Abs;
            break;
        case Op::Noise:
        case Op::RgbToHsv:
        case Op::HsvToRgb:
            in.op = Op::Copy;
            break;
        default:
            break;
        }
    }
    return code;
}

// Random programs for the divergence experiment: 60 instructions from a pool
// of per-component, reduce and transcendental operations over rows [11, 72).
static std::vector<Instr> randomProgram(std::mt19937 &rng, int count, int uniformCount) {
    static const Op pool[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Min,   Op::Max,   Op::Mix,       Op::Clamp,
                              Op::Abs, Op::Fract, Op::Floor, Op::Sqrt, Op::Sin, Op::Exp, Op::Pow, Op::Smoothstep,
                              Op::Select, Op::Greater, Op::Dot, Op::Normalize, Op::Length, Op::Neg, Op::Mod, Op::Cross};
    std::vector<Instr> code;
    int written = 11; // rows [0, written) hold values
    for (int i = 0; i < count; ++i) {
        const Op op = pool[rng() % (sizeof(pool) / sizeof(pool[0]))];
        Instr in{};
        in.op = op;
        int w = (op == Op::Dot || op == Op::Length) ? 1 : (op == Op::Cross || op == Op::Normalize) ? 3 : 1 + int(rng() % 3);
        in.width = uint8_t(w);
        const int dst = 11 + int(rng() % 58); // [11, 69)
        in.dst = uint16_t(dst);
        auto src = [&](uint16_t &idx, uint8_t uflag) {
            if (rng() % 4 == 0) {
                idx = uint16_t(rng() % uint32_t(uniformCount - 4));
                in.flags |= uflag;
            } else {
                idx = uint16_t(rng() % uint32_t(std::max(written - 3, 1)));
            }
        };
        src(in.a, kUniformA);
        src(in.b, kUniformB);
        src(in.c, kUniformC);
        if (op == Op::Dot || op == Op::Length)
            in.imm = 3;
        code.push_back(in);
        written = std::max(written, std::min(dst + w, 69));
    }
    return code;
}

// ---------------------------------------------------------------------------
// Device side
// ---------------------------------------------------------------------------

__constant__ float cU[2048];
__constant__ Instr cCode[64];

struct Seg {
    int begin, count, tex; // a run of non-texture instructions, then Tex2D `tex` (-1: none)
};
__constant__ Seg cSeg[8];

struct DeviceTextures {
    cudaTextureObject_t tex[3];
};

// Tex2D on the device: the lookup vm.h would do with a device sampler
// (uv rows a, derivative rows b, fallback c only for a missing texture).
__device__ __forceinline__ void deviceTex2D(const Instr &in, float *R, const DeviceTextures &t) {
    const float s = R[in.a], tt = R[in.a + 1];
    const float2 ddx = make_float2(R[in.b], R[in.b + 1]), ddy = make_float2(R[in.b + 2], R[in.b + 3]);
    const float4 v = tex2DGrad<float4>(t.tex[in.imm], s, tt, ddx, ddy);
    R[in.dst] = v.x;
    if (in.width > 1) {
        R[in.dst + 1] = v.y;
        R[in.dst + 2] = v.z;
    }
}

enum Flags : int {
    kTex = 1,         // hardware textures (else vm.h's fallback)
    kSharedRows = 2,  // register file in shared memory (else local memory)
    kConstU = 4,      // uniforms in constant memory (else global)
    kConstCode = 8,   // code in constant memory (else global)
};

template <int F>
__global__ void __launch_bounds__(128) vmKernel(int n, const float *__restrict__ in, float *__restrict__ out,
                                                const Instr *gCode, int codeCount, const float *gU, int segCount,
                                                DeviceTextures tex) {
    const int lane = blockIdx.x * blockDim.x + threadIdx.x;
    extern __shared__ float sRows[];
    float local[kRows];
    float *R = (F & kSharedRows) ? sRows + threadIdx.x * (kRows + 1) : local; // odd stride: no bank conflicts
    if (lane >= n)
        return;
    for (int k = 0; k < 11; ++k)
        R[k] = in[size_t(k) * n + lane];
    for (int k = 0; k < 4; ++k)
        R[kRowDeriv + k] = in[size_t(11 + k) * n + lane];
    const Instr *code = (F & kConstCode) ? cCode : gCode;
    if (F & kTex) {
        for (int sgi = 0; sgi < segCount; ++sgi) {
            const Seg sg = cSeg[sgi];
            if (F & kConstU)
                execute<1>(code + sg.begin, size_t(sg.count), R, cU);
            else
                execute<1>(code + sg.begin, size_t(sg.count), R, gU);
            if (sg.tex >= 0)
                deviceTex2D(code[sg.tex], R, tex);
        }
    } else {
        if (F & kConstU)
            execute<1>(code, size_t(codeCount), R, cU);
        else
            execute<1>(code, size_t(codeCount), R, gU);
    }
    for (int k = 0; k < kOutputs; ++k)
        out[size_t(k) * n + lane] = R[outRow(k)];
}

// The same math, written out (uniform values as in buildProgram, read from U
// so the compiler cannot fold them).
template <bool Tex>
__global__ void __launch_bounds__(128) handKernel(int n, const float *__restrict__ in, float *__restrict__ out,
                                                  const float *__restrict__ U, Slots s, DeviceTextures tex) {
    const int lane = blockIdx.x * blockDim.x + threadIdx.x;
    if (lane >= n)
        return;
    auto I_ = [&](int k) { return in[size_t(k) * n + lane]; };
    const Vec3f P{I_(0), I_(1), I_(2)}, N{I_(3), I_(4), I_(5)}, Id{I_(8), I_(9), I_(10)};
    const float u0 = I_(6), v0 = I_(7);
    const float2 ddx = make_float2(I_(11), I_(12)), ddy = make_float2(I_(13), I_(14));
    const float uvx = u0 * U[s.uvScale] + U[s.uvOffset], uvy = v0 * U[s.uvScale + 1] + U[s.uvOffset + 1];
    Vec3f base{U[s.baseFb], U[s.baseFb + 1], U[s.baseFb + 2]}, nm{U[s.nrmFb], U[s.nrmFb + 1], U[s.nrmFb + 2]};
    float rough = U[s.roughFb];
    if (Tex) {
        const float4 a = tex2DGrad<float4>(tex.tex[0], uvx, uvy, ddx, ddy);
        const float4 b = tex2DGrad<float4>(tex.tex[1], uvx, uvy, ddx, ddy);
        const float4 c = tex2DGrad<float4>(tex.tex[2], uvx, uvy, ddx, ddy);
        base = {a.x, a.y, a.z};
        rough = b.x;
        nm = {c.x, c.y, c.z};
    }
    const Vec3f Pf = P * U[s.freq];
    const float nz = noise::perlin(Pf);
    const float n01 = nz * U[s.half] + U[s.half];
    const float g = U[s.gamma];
    Vec3f col{vexPow(base.x, g), vexPow(base.y, g), vexPow(base.z, g)};
    const Vec3f tint{U[s.tint], U[s.tint + 1], U[s.tint + 2]};
    col = col + (tint - col) * n01;
    col = col * U[s.gain];
    col = {vexClamp(col.x, U[s.zero], U[s.one]), vexClamp(col.y, U[s.zero], U[s.one]),
           vexClamp(col.z, U[s.zero], U[s.one])};
    Vec3f hsv = noise::rgbToHsv(col);
    hsv.x = hsv.x + U[s.hueShift];
    hsv.x = hsv.x - std::floor(hsv.x);
    col = noise::hsvToRgb(hsv);
    const float lum = col.x * U[s.lumW] + col.y * U[s.lumW + 1] + col.z * U[s.lumW + 2];
    const float sat = U[s.sat];
    col = {lum + (col.x - lum) * sat, lum + (col.y - lum) * sat, lum + (col.z - lum) * sat};
    Vec3f nt = nm * U[s.two] - Vec3f{U[s.one], U[s.one], U[s.one]};
    auto vmNormalize = [](Vec3f a) {
        float acc = a.x * a.x + a.y * a.y + a.z * a.z;
        acc = acc > 0.0f ? 1.0f / std::sqrt(acc) : 0.0f;
        return a * acc;
    };
    nt = vmNormalize(nt);
    Vec3f T = cross(N, Vec3f{U[s.up], U[s.up + 1], U[s.up + 2]});
    T = vmNormalize(T);
    const Vec3f B = cross(N, T);
    Vec3f Nn = T * nt.x;
    Nn = Nn + B * nt.y;
    Nn = Nn + N * nt.z;
    Nn = vmNormalize(Nn);
    float r = vexSmoothstep(U[s.lo], U[s.hi], rough);
    r = U[s.rmin] + (U[s.rmax] - U[s.rmin]) * r;
    r = r * r;
    r = vexClamp(r, U[s.zero], U[s.one]);
    float c = Nn.x * Id.x + Nn.y * Id.y + Nn.z * Id.z;
    c = -c;
    c = U[s.zero] > c ? U[s.zero] : c;
    const float F = U[s.f0] + (U[s.one] - U[s.f0]) * vexPow(U[s.one] - c, U[s.five]);
    const Vec3f st{U[s.specTint], U[s.specTint + 1], U[s.specTint + 2]};
    const Vec3f spec = (st + (col - st) * U[s.metal]) * F;
    const float e = std::fabs(std::sin(nz * U[s.k]));
    const Vec3f emission = Vec3f{U[s.emit], U[s.emit + 1], U[s.emit + 2]} * e;
    float len = std::sqrt(Pf.x * Pf.x + Pf.y * Pf.y + Pf.z * Pf.z);
    len = len - std::floor(len);
    const float coat = (len > U[s.half] ? 1.0f : 0.0f) != 0.0f ? U[s.coatA] : U[s.coatB];
    const float ex = std::exp(-r);
    float ang = std::atan2(uvy, uvx) * U[s.stripes];
    const float stripe = vexMod(std::floor(ang), U[s.two]);
    float op = ex + (n01 - ex) * stripe;
    op = vexClamp(op * coat, U[s.zero], U[s.one]);
    const float o[kOutputs] = {col.x, col.y, col.z, r, Nn.x, Nn.y, Nn.z, spec.x, spec.y, spec.z,
                               emission.x, emission.y, emission.z, coat, op};
    for (int k = 0; k < kOutputs; ++k)
        out[size_t(k) * n + lane] = o[k];
}

// Divergence: lane l runs program prog[l] (code at codes + prog * stride)
// with uniforms U + uni[l] * uStride.
template <bool ConstU>
__global__ void __launch_bounds__(128) divKernel(int n, const float *__restrict__ in, float *__restrict__ out,
                                                 const Instr *codes, int codeCount, const int *prog,
                                                 const int *uni, const float *gU, int uStride) {
    const int lane = blockIdx.x * blockDim.x + threadIdx.x;
    if (lane >= n)
        return;
    float R[kRows];
    for (int k = 0; k < 11; ++k)
        R[k] = in[size_t(k) * n + lane];
    for (int k = 11; k < kRows; ++k)
        R[k] = 0.5f;
    const Instr *code = codes + size_t(prog[lane]) * size_t(codeCount);
    if (ConstU)
        execute<1>(code, size_t(codeCount), R, cU + uni[lane] * uStride);
    else
        execute<1>(code, size_t(codeCount), R, gU + size_t(uni[lane]) * size_t(uStride));
    float acc = 0.0f;
    for (int k = 11; k < 69; k += 7)
        acc += R[k];
    out[lane] = acc;
}

// ---------------------------------------------------------------------------
// Host helpers
// ---------------------------------------------------------------------------

static float median(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <typename Launch>
static float timeKernel(Launch &&launch, int reps = 15) {
    cudaEvent_t a, b;
    CK(cudaEventCreate(&a));
    CK(cudaEventCreate(&b));
    launch(); // warm-up
    CK(cudaDeviceSynchronize());
    std::vector<float> ms;
    for (int r = 0; r < reps; ++r) {
        CK(cudaEventRecord(a));
        launch();
        CK(cudaEventRecord(b));
        CK(cudaEventSynchronize(b));
        float t = 0.0f;
        CK(cudaEventElapsedTime(&t, a, b));
        ms.push_back(t);
    }
    CK(cudaGetLastError());
    CK(cudaEventDestroy(a));
    CK(cudaEventDestroy(b));
    return median(ms);
}

// A mipmapped 2048^2 texture of `channels` half channels (4 or 1), filled per
// level with a smooth pattern; trilinear, anisotropic 8, wrap.
static cudaTextureObject_t makeTexture(int channels, int seed, std::vector<cudaMipmappedArray_t> &keep) {
    const int size = 2048, levels = 12;
    cudaChannelFormatDesc fmt = channels == 4 ? cudaCreateChannelDescHalf4() : cudaCreateChannelDescHalf();
    cudaMipmappedArray_t mip;
    CK(cudaMallocMipmappedArray(&mip, &fmt, make_cudaExtent(size, size, 0), levels));
    keep.push_back(mip);
    for (int l = 0; l < levels; ++l) {
        const int s = std::max(size >> l, 1);
        std::vector<__half> data(size_t(s) * s * channels);
        for (int y = 0; y < s; ++y)
            for (int x = 0; x < s; ++x)
                for (int c = 0; c < channels; ++c) {
                    const float fx = float(x) / s, fy = float(y) / s;
                    const float v = 0.5f + 0.4f * std::sin(6.2831853f * (fx * (3 + c + seed) + fy * (2 + seed)));
                    data[(size_t(y) * s + x) * channels + c] = __float2half(v);
                }
        cudaArray_t level;
        CK(cudaGetMipmappedArrayLevel(&level, mip, l));
        CK(cudaMemcpy2DToArray(level, 0, 0, data.data(), size_t(s) * channels * sizeof(__half),
                               size_t(s) * channels * sizeof(__half), s, cudaMemcpyHostToDevice));
    }
    cudaResourceDesc res{};
    res.resType = cudaResourceTypeMipmappedArray;
    res.res.mipmap.mipmap = mip;
    cudaTextureDesc td{};
    td.addressMode[0] = td.addressMode[1] = cudaAddressModeWrap;
    td.filterMode = cudaFilterModeLinear;
    td.mipmapFilterMode = cudaFilterModeLinear;
    td.maxAnisotropy = 8;
    td.maxMipmapLevelClamp = float(levels - 1);
    td.readMode = cudaReadModeElementType;
    td.normalizedCoords = 1;
    cudaTextureObject_t t;
    CK(cudaCreateTextureObject(&t, &res, &td, nullptr));
    return t;
}

// Inputs for n lanes on a 1024-wide "screen": P, N, uv, I, footprint
// derivatives (about one texel of the finest level per pixel at uvScale 4).
// `perm` shuffles the lanes (incoherent hits).
static std::vector<float> makeInputs(int n, const std::vector<int> *perm) {
    std::vector<float> h(size_t(kInputs) * n);
    for (int i = 0; i < n; ++i) {
        const int j = perm ? (*perm)[i] : i;
        const float x = float(j % 1024) / 1024.0f, y = float(j / 1024) / 1024.0f;
        const float P[3] = {x * 4.0f - 2.0f, y * 4.0f - 2.0f, 0.3f * std::sin(7.0f * x) * std::cos(5.0f * y)};
        Vec3f N = normalize(Vec3f{0.2f * std::cos(7.0f * x), 0.3f * std::sin(5.0f * y), 1.0f});
        Vec3f D = normalize(Vec3f{x - 0.5f, y - 0.5f, -1.0f});
        const float v[kInputs] = {P[0], P[1], P[2], N.x, N.y, N.z, x, y, D.x, D.y, D.z,
                                  4.0f / 2048.0f, 0.0f, 0.0f, 4.0f / 2048.0f};
        for (int k = 0; k < kInputs; ++k)
            h[size_t(k) * n + i] = v[k];
    }
    return h;
}

// The CPU shade stage's way: execute<N> per chunk of N lanes (rows R[r*N+l]).
template <int N>
static void cpuRun(const std::vector<Instr> &code, const std::vector<float> &U, const std::vector<float> &in, int n,
                   int begin, int end, std::vector<float> &out) {
    std::vector<float> R(size_t(kRows) * N, 0.0f);
    for (int base = begin; base < end; base += N) {
        const int cnt = std::min(N, end - base);
        for (int l = 0; l < N; ++l) {
            const int lane = base + std::min(l, cnt - 1);
            for (int k = 0; k < 11; ++k)
                R[size_t(k) * N + l] = in[size_t(k) * n + lane];
            for (int k = 0; k < 4; ++k)
                R[size_t(kRowDeriv + k) * N + l] = in[size_t(11 + k) * n + lane];
        }
        execute<N>(code.data(), code.size(), R.data(), U.data(), nullptr);
        for (int l = 0; l < cnt; ++l)
            for (int k = 0; k < kOutputs; ++k)
                out[size_t(k) * n + base + l] = R[size_t(outRow(k)) * N + l];
    }
}

template <int N>
static double cpuTime(const std::vector<Instr> &code, const std::vector<float> &U, const std::vector<float> &in, int n,
                      int threads, std::vector<float> &out) {
    std::vector<double> times;
    for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        const int per = (n + threads - 1) / threads;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                const int b = t * per, e = std::min(n, b + per);
                if (b < e)
                    cpuRun<N>(code, U, in, n, b, e, out);
            });
        for (auto &th : pool)
            th.join();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

struct Diff {
    double maxAbs = 0.0;
    long differing = 0; // floats not bit-identical
    long nonFinite = 0;
};
static Diff compare(const std::vector<float> &a, const std::vector<float> &b) {
    Diff d;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) {
            ++d.nonFinite;
            continue;
        }
        if (std::memcmp(&a[i], &b[i], 4) != 0)
            ++d.differing;
        d.maxAbs = std::max(d.maxAbs, double(std::fabs(a[i] - b[i])));
    }
    return d;
}

int main() {
    cudaDeviceProp prop{};
    CK(cudaGetDeviceProperties(&prop, 0));
    std::printf("device: %s, %d SMs, sm_%d%d, %.1f GB\n", prop.name, prop.multiProcessorCount, prop.major, prop.minor,
                double(prop.totalGlobalMem) / 1e9);
#if defined(__CUDACC__)
    std::printf("build: nvcc %d.%d\n", __CUDACC_VER_MAJOR__, __CUDACC_VER_MINOR__);
#endif

    ProgramBuilder pb;
    Slots slots{};
    buildProgram(pb, slots);
    const int codeCount = int(pb.code.size());
    std::printf("program: %d instructions, %ld component ops, %d rows, %zu uniforms\n", codeCount,
                componentOps(pb.code), kRows, pb.U.size());

    // Segments around the Tex2D instructions.
    std::vector<Seg> segs;
    {
        int b = 0;
        for (int i = 0; i < codeCount; ++i)
            if (pb.code[size_t(i)].op == Op::Tex2D) {
                segs.push_back({b, i - b, i});
                b = i + 1;
            }
        segs.push_back({b, codeCount - b, -1});
    }
    CK(cudaMemcpyToSymbol(cSeg, segs.data(), segs.size() * sizeof(Seg)));
    CK(cudaMemcpyToSymbol(cCode, pb.code.data(), pb.code.size() * sizeof(Instr)));
    CK(cudaMemcpyToSymbol(cU, pb.U.data(), pb.U.size() * sizeof(float)));

    const int n = 1 << 20;
    std::vector<int> perm(n);
    for (int i = 0; i < n; ++i)
        perm[i] = i;
    std::mt19937 rng(42);
    std::shuffle(perm.begin(), perm.end(), rng);
    const std::vector<float> hIn = makeInputs(n, nullptr), hInShuf = makeInputs(n, &perm);

    float *dIn, *dInShuf, *dOut, *dU;
    Instr *dCode;
    CK(cudaMalloc(&dIn, hIn.size() * 4));
    CK(cudaMalloc(&dInShuf, hIn.size() * 4));
    CK(cudaMalloc(&dOut, size_t(kOutputs) * n * 4));
    CK(cudaMalloc(&dU, pb.U.size() * 4));
    CK(cudaMalloc(&dCode, pb.code.size() * sizeof(Instr)));
    CK(cudaMemcpy(dIn, hIn.data(), hIn.size() * 4, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(dInShuf, hInShuf.data(), hIn.size() * 4, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(dU, pb.U.data(), pb.U.size() * 4, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(dCode, pb.code.data(), pb.code.size() * sizeof(Instr), cudaMemcpyHostToDevice));

    std::vector<cudaMipmappedArray_t> arrays;
    DeviceTextures tex{};
    tex.tex[0] = makeTexture(4, 0, arrays);
    tex.tex[1] = makeTexture(1, 1, arrays);
    tex.tex[2] = makeTexture(4, 2, arrays);

    const int block = 128, grid = (n + block - 1) / block;
    const size_t shmem = size_t(block) * (kRows + 1) * 4;
    const double instrLanes = double(codeCount) * n, compLanes = double(componentOps(pb.code)) * n;
    auto report = [&](const char *name, float ms) {
        std::printf("  %-44s %8.3f ms  %7.1f G instr/s  %7.1f G comp-ops/s  %6.3f ns/lane\n", name, ms,
                    instrLanes / (ms * 1e6), compLanes / (ms * 1e6), ms * 1e6 / n);
    };

    std::printf("\nA. 1M lanes (2^20), 128 threads per block\n");
    struct Var {
        const char *name;
        void (*k)(int, const float *, float *, const Instr *, int, const float *, int, DeviceTextures);
        bool shared;
    };
    const Var vars[] = {
        {"vm, fallback tex, rows local, U+code global", vmKernel<0>, false},
        {"vm, fallback tex, rows local, U const", vmKernel<kConstU>, false},
        {"vm, fallback tex, rows local, U+code const", vmKernel<kConstU | kConstCode>, false},
        {"vm, fallback tex, rows shared, U+code global", vmKernel<kSharedRows>, true},
        {"vm, fallback tex, rows shared, U+code const", vmKernel<kSharedRows | kConstU | kConstCode>, true},
        {"vm + hw tex, rows local, U+code global", vmKernel<kTex>, false},
        {"vm + hw tex, rows local, U+code const", vmKernel<kTex | kConstU | kConstCode>, false},
        {"vm + hw tex, rows shared, U+code const", vmKernel<kTex | kSharedRows | kConstU | kConstCode>, true},
    };
    for (int coherent = 1; coherent >= 0; --coherent) {
        std::printf(" %s lanes:\n", coherent ? "coherent" : "shuffled");
        const float *src = coherent ? dIn : dInShuf;
        for (const Var &v : vars) {
            const float ms = timeKernel([&] {
                v.k<<<grid, block, v.shared ? shmem : 0>>>(n, src, dOut, dCode, codeCount, dU, int(segs.size()), tex);
            });
            report(v.name, ms);
        }
        report("hand-written, fallback tex", timeKernel([&] {
                   handKernel<false><<<grid, block>>>(n, src, dOut, dU, slots, tex);
               }));
        report("hand-written + hw tex", timeKernel([&] {
                   handKernel<true><<<grid, block>>>(n, src, dOut, dU, slots, tex);
               }));
    }

    // D. Agreement on the GPU (coherent inputs).
    std::vector<float> gVm(size_t(kOutputs) * n), gHand(size_t(kOutputs) * n), gVmFb(size_t(kOutputs) * n),
        gHandFb(size_t(kOutputs) * n);
    vmKernel<kTex><<<grid, block>>>(n, dIn, dOut, dCode, codeCount, dU, int(segs.size()), tex);
    CK(cudaMemcpy(gVm.data(), dOut, gVm.size() * 4, cudaMemcpyDeviceToHost));
    handKernel<true><<<grid, block>>>(n, dIn, dOut, dU, slots, tex);
    CK(cudaMemcpy(gHand.data(), dOut, gHand.size() * 4, cudaMemcpyDeviceToHost));
    vmKernel<0><<<grid, block>>>(n, dIn, dOut, dCode, codeCount, dU, int(segs.size()), tex);
    CK(cudaMemcpy(gVmFb.data(), dOut, gVmFb.size() * 4, cudaMemcpyDeviceToHost));
    handKernel<false><<<grid, block>>>(n, dIn, dOut, dU, slots, tex);
    CK(cudaMemcpy(gHandFb.data(), dOut, gHandFb.size() * 4, cudaMemcpyDeviceToHost));

    // C. CPU.
    std::printf("\nC. CPU, same program, fallback textures (vm.h execute<N>, MSVC /O2 /arch:AVX2)\n");
    std::vector<float> cOut(size_t(kOutputs) * n);
    const int hw = int(std::thread::hardware_concurrency());
    auto cpuReport = [&](const char *name, double ms) {
        std::printf("  %-44s %8.2f ms  %7.2f G instr/s  %6.3f ns/(instr*lane)\n", name, ms, instrLanes / (ms * 1e6),
                    ms * 1e6 / instrLanes);
    };
    cpuReport("execute<32>, 1 thread", cpuTime<32>(pb.code, pb.U, hIn, n, 1, cOut));
    std::vector<float> cOut1(size_t(kOutputs) * n);
    cpuReport("execute<1>, 1 thread", cpuTime<1>(pb.code, pb.U, hIn, n, 1, cOut1));
    char name[64];
    std::snprintf(name, sizeof(name), "execute<32>, %d threads", hw);
    cpuReport(name, cpuTime<32>(pb.code, pb.U, hIn, n, hw, cOut));

    {
        // Arithmetic only: the same shape without transcendental or procedural operations.
        const std::vector<Instr> arith = arithmeticOnly(pb.code);
        Instr *dArith;
        CK(cudaMalloc(&dArith, arith.size() * sizeof(Instr)));
        CK(cudaMemcpy(dArith, arith.data(), arith.size() * sizeof(Instr), cudaMemcpyHostToDevice));
        std::printf("\nA2/C2. Arithmetic-only variant (pow/sin/exp/atan2 -> mul/abs, noise/hsv -> copy)\n");
        report("GPU vm, fallback tex, rows local, U const", timeKernel([&] {
                   vmKernel<kConstU><<<grid, block>>>(n, dIn, dOut, dArith, codeCount, dU, int(segs.size()), tex);
               }));
        std::vector<float> aOut(size_t(kOutputs) * n);
        cpuReport("CPU execute<32>, 1 thread", cpuTime<32>(arith, pb.U, hIn, n, 1, aOut));
        std::snprintf(name, sizeof(name), "CPU execute<32>, %d threads", hw);
        cpuReport(name, cpuTime<32>(arith, pb.U, hIn, n, hw, aOut));
        cudaFree(dArith);
    }

    std::printf("\nD. Agreement (15 outputs x 1M lanes)\n");
    auto pd = [](const char *name, const Diff &d) {
        std::printf("  %-44s max |diff| %.3g, %ld floats not bit-identical, %ld non-finite\n", name, d.maxAbs,
                    d.differing, d.nonFinite);
    };
    pd("GPU vm+tex vs GPU hand-written+tex", compare(gVm, gHand));
    pd("GPU vm fallback vs GPU hand-written fallback", compare(gVmFb, gHandFb));
    pd("GPU vm fallback vs CPU execute<32>", compare(gVmFb, cOut));
    pd("CPU execute<1> vs CPU execute<32>", compare(cOut1, cOut));
    {
        // Which outputs differ between GPU and CPU.
        std::printf("  GPU vs CPU per output (differing lanes):");
        for (int k = 0; k < kOutputs; ++k) {
            long c = 0;
            for (int i = 0; i < n; ++i)
                c += std::memcmp(&gVmFb[size_t(k) * n + i], &cOut[size_t(k) * n + i], 4) != 0;
            std::printf(" %ld", c);
        }
        std::printf("\n");
    }

    // B. Divergence.
    std::printf("\nB. Divergence: 1M lanes, 60-instruction random programs, rows local, U global unless noted\n");
    {
        const int K = 32, uStride = 64;
        std::mt19937 prng(7);
        std::vector<Instr> codes;
        for (int k = 0; k < K; ++k) {
            const std::vector<Instr> c = randomProgram(prng, 60, uStride);
            codes.insert(codes.end(), c.begin(), c.end());
        }
        std::vector<float> Us(size_t(K) * uStride);
        for (size_t i = 0; i < Us.size(); ++i)
            Us[i] = 0.25f + 0.5f * float((i * 2654435761u) % 1000u) / 1000.0f;
        CK(cudaMemcpyToSymbol(cU, Us.data(), Us.size() * 4));
        Instr *dCodes;
        float *dUs;
        int *dProg, *dUni;
        CK(cudaMalloc(&dCodes, codes.size() * sizeof(Instr)));
        CK(cudaMalloc(&dUs, Us.size() * 4));
        CK(cudaMalloc(&dProg, size_t(n) * 4));
        CK(cudaMalloc(&dUni, size_t(n) * 4));
        CK(cudaMemcpy(dCodes, codes.data(), codes.size() * sizeof(Instr), cudaMemcpyHostToDevice));
        CK(cudaMemcpy(dUs, Us.data(), Us.size() * 4, cudaMemcpyHostToDevice));
        long divOps = 0;
        for (int k = 0; k < K; ++k)
            divOps += componentOps(std::vector<Instr>(codes.begin() + k * 60, codes.begin() + (k + 1) * 60));
        std::printf("  (programs average %.1f component ops)\n", double(divOps) / K);
        std::vector<int> hp(n), hu(n);
        std::mt19937 arng(3);
        auto run = [&](const char *label, bool constU) {
            CK(cudaMemcpy(dProg, hp.data(), size_t(n) * 4, cudaMemcpyHostToDevice));
            CK(cudaMemcpy(dUni, hu.data(), size_t(n) * 4, cudaMemcpyHostToDevice));
            const float ms = constU ? timeKernel([&] {
                divKernel<true><<<grid, block>>>(n, dIn, dOut, dCodes, 60, dProg, dUni, dUs, uStride);
            })
                                    : timeKernel([&] {
                                          divKernel<false><<<grid, block>>>(n, dIn, dOut, dCodes, 60, dProg, dUni,
                                                                            dUs, uStride);
                                      });
            std::printf("  %-52s %8.3f ms\n", label, ms);
        };
        for (int k : {1, 2, 4, 8, 32}) {
            char l[96];
            for (int i = 0; i < n; ++i) {
                hp[i] = int(uint64_t(i) * k / n); // sorted
                hu[i] = hp[i];
            }
            std::snprintf(l, sizeof(l), "K=%2d programs, lanes sorted by program", k);
            run(l, false);
            for (int i = 0; i < n; ++i) {
                hp[i] = int(arng() % uint32_t(k));
                hu[i] = hp[i];
            }
            std::snprintf(l, sizeof(l), "K=%2d programs, lanes random", k);
            run(l, false);
        }
        for (int k : {1, 8, 32}) {
            char l[96];
            for (int i = 0; i < n; ++i) {
                hp[i] = 0;
                hu[i] = int(arng() % uint32_t(k));
            }
            std::snprintf(l, sizeof(l), "1 program, K=%2d uniform blocks random, U global", k);
            run(l, false);
            std::snprintf(l, sizeof(l), "1 program, K=%2d uniform blocks random, U const", k);
            run(l, true);
        }
        cudaFree(dCodes);
        cudaFree(dUs);
        cudaFree(dProg);
        cudaFree(dUni);
    }
    std::printf("\ndone\n");
    return 0;
}
