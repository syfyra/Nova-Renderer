// OptiX accel probe for docs/research/m10/optix-accel.md: GAS/IAS builds,
// curve and point primitives, raygen-only queries over SoA rays (Nova's
// RayBatch layout), any-hit cost, launch overhead, motion, refit, precision.
//
//   oa_probe.exe [info|tri|trace|anyhit|launch|curves|points|attrs|motion|ias|local|precision|all]
//   (several tests: a comma-separated list, e.g. "attrs,motion")
//
// The device programs (oa_device.optixir / .ptx, oa_cuda_kernel.ptx) are read
// from the executable's directory (oa_build.bat). Like Nova's OptiX denoiser
// (core/denoise/optix_denoiser.cpp), the CUDA driver API is resolved from
// nvcuda.dll at run time and optixInit loads nvoptix.dll from the driver.
// Stand-alone: no Nova headers; the few Nova functions it needs (ray offsets,
// the opacity hash) are re-stated here.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// clang-format off
#include <cuda.h>
#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>
#include <optix_stack_size.h>
// clang-format on

#include "oa_shared.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
    std::fputc('\n', stderr);
    std::exit(1);
}

// ---------------------------------------------------------------------------
// CUDA driver API from nvcuda.dll (the pattern of core/denoise/optix_denoiser.cpp)

#define OA_CU_NAME2(x) #x
#define OA_CU_NAME(x) OA_CU_NAME2(x)
#define OA_CU_FUNCS(X)                                                                                                \
    X(cuInit) X(cuDriverGetVersion) X(cuDeviceGet) X(cuDeviceGetName) X(cuDeviceGetAttribute)                         \
    X(cuDevicePrimaryCtxRetain) X(cuCtxSetCurrent) X(cuStreamCreate) X(cuStreamSynchronize) X(cuMemAlloc)            \
    X(cuMemFree) X(cuMemcpyHtoD) X(cuMemcpyDtoH) X(cuMemcpyHtoDAsync) X(cuEventCreate) X(cuEventRecord)              \
    X(cuEventSynchronize) X(cuEventElapsedTime) X(cuEventDestroy) X(cuModuleLoadData) X(cuModuleGetFunction)         \
    X(cuLaunchKernel) X(cuMemGetInfo) X(cuGetErrorName) X(cuStreamBeginCapture) X(cuStreamEndCapture)                 \
    X(cuGraphInstantiate) X(cuGraphLaunch) X(cuGraphExecDestroy) X(cuGraphDestroy)

struct CudaApi {
#define OA_CU_DECLARE(fn) decltype(&fn) p_##fn = nullptr;
    OA_CU_FUNCS(OA_CU_DECLARE)
#undef OA_CU_DECLARE
    void load() {
        HMODULE dll = LoadLibraryA("nvcuda.dll");
        if (!dll)
            fail("no NVIDIA driver (nvcuda.dll not found)");
#define OA_CU_LOAD(fn)                                                                                                \
    p_##fn = reinterpret_cast<decltype(&fn)>(GetProcAddress(dll, OA_CU_NAME(fn)));                                    \
    if (!p_##fn)                                                                                                      \
        fail("nvcuda.dll lacks %s", OA_CU_NAME(fn));
        OA_CU_FUNCS(OA_CU_LOAD)
#undef OA_CU_LOAD
    }
} cu;

#define CU(call)                                                                                                      \
    do {                                                                                                              \
        const CUresult r_ = (call);                                                                                   \
        if (r_ != CUDA_SUCCESS) {                                                                                     \
            const char *n_ = "?";                                                                                     \
            cu.p_cuGetErrorName(r_, &n_);                                                                             \
            fail("%s:%d CUDA %s in %s", __FILE__, __LINE__, n_, #call);                                               \
        }                                                                                                             \
    } while (0)
#define OX(call)                                                                                                      \
    do {                                                                                                              \
        const OptixResult r_ = (call);                                                                                \
        if (r_ != OPTIX_SUCCESS)                                                                                      \
            fail("%s:%d OptiX %s in %s", __FILE__, __LINE__, optixGetErrorName(r_), #call);                           \
    } while (0)

CUcontext g_cuctx = nullptr;
CUstream g_stream = nullptr;
OptixDeviceContext g_ctx = nullptr;
std::string g_dir; // the executable's directory, with a trailing slash

double now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

float median(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0f : v[v.size() / 2];
}

void logCallback(unsigned level, const char *tag, const char *msg, void *) {
    std::fprintf(stderr, "[optix %u][%s] %s\n", level, tag, msg);
}

void initGpu() {
    cu.load();
    CU(cu.p_cuInit(0));
    CUdevice dev;
    CU(cu.p_cuDeviceGet(&dev, 0));
    CU(cu.p_cuDevicePrimaryCtxRetain(&g_cuctx, dev));
    CU(cu.p_cuCtxSetCurrent(g_cuctx));
    CU(cu.p_cuStreamCreate(&g_stream, CU_STREAM_NON_BLOCKING));
    const OptixResult r = optixInit();
    if (r != OPTIX_SUCCESS)
        fail("optixInit: %s%s", optixGetErrorName(r),
             r == OPTIX_ERROR_UNSUPPORTED_ABI_VERSION ? " (driver too old for OptiX 9.1, ABI 118)" : "");
    OptixDeviceContextOptions o{};
    o.logCallbackFunction = &logCallback;
    o.logCallbackLevel = 2; // errors and warnings
    OX(optixDeviceContextCreate(g_cuctx, &o, &g_ctx));
    // Keep the probe's compile cache next to the executable.
    const std::string cache = g_dir + "optix_cache";
    CreateDirectoryA(cache.c_str(), nullptr);
    OX(optixDeviceContextSetCacheLocation(g_ctx, cache.c_str()));
}

// ---------------------------------------------------------------------------
// Device memory and timing

struct Buf {
    CUdeviceptr p = 0;
    size_t n = 0;
};
Buf dalloc(size_t n) {
    Buf b;
    b.n = n;
    if (n)
        CU(cu.p_cuMemAlloc(&b.p, n));
    return b;
}
void dfree(Buf &b) {
    if (b.p)
        cu.p_cuMemFree(b.p);
    b = Buf{};
}
// Copies go through g_stream and wait for it. g_stream is non-blocking, so a
// plain cuMemcpyHtoD (legacy stream) may still be in flight when work on
// g_stream starts: a pageable HtoD returns once the data is staged, not
// delivered. That race corrupted IAS builds here (CUDA_ERROR_ILLEGAL_ADDRESS
// after earlier tests had left stale data in reused memory).
void h2d(CUdeviceptr dst, const void *src, size_t bytes) {
    CU(cu.p_cuMemcpyHtoDAsync(dst, src, bytes, g_stream));
    CU(cu.p_cuStreamSynchronize(g_stream));
}
void d2h(void *dst, CUdeviceptr src, size_t bytes) {
    CU(cu.p_cuStreamSynchronize(g_stream));
    CU(cu.p_cuMemcpyDtoH(dst, src, bytes));
}
template <class T> Buf dupload(const T *data, size_t count) {
    Buf b = dalloc(count * sizeof(T));
    if (count)
        h2d(b.p, data, count * sizeof(T));
    return b;
}
template <class T> Buf dupload(const std::vector<T> &v) { return dupload(v.data(), v.size()); }
template <class T> void ddownload(std::vector<T> &v, const Buf &b, size_t count) {
    v.resize(count);
    if (count)
        d2h(v.data(), b.p, count * sizeof(T));
}

struct Events {
    CUevent a = nullptr, b = nullptr;
    Events() {
        CU(cu.p_cuEventCreate(&a, CU_EVENT_DEFAULT));
        CU(cu.p_cuEventCreate(&b, CU_EVENT_DEFAULT));
    }
    ~Events() {
        cu.p_cuEventDestroy(a);
        cu.p_cuEventDestroy(b);
    }
    void start() { CU(cu.p_cuEventRecord(a, g_stream)); }
    float stop() { // ms
        CU(cu.p_cuEventRecord(b, g_stream));
        CU(cu.p_cuEventSynchronize(b));
        float ms = 0.0f;
        CU(cu.p_cuEventElapsedTime(&ms, a, b));
        return ms;
    }
};

// ---------------------------------------------------------------------------
// Host math

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a) { return a * (1.0f / length(a)); }
V3 ld3(const std::vector<float> &v, size_t i) { return {v[3 * i], v[3 * i + 1], v[3 * i + 2]}; }
void st3(std::vector<float> &v, size_t i, V3 a) {
    v[3 * i] = a.x;
    v[3 * i + 1] = a.y;
    v[3 * i + 2] = a.z;
}
constexpr float kPi = 3.14159265358979f;

struct Rng { // PCG32
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    uint32_t u32() {
        const uint64_t o = s;
        s = o * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t x = uint32_t(((o >> 18u) ^ o) >> 27u), r = uint32_t(o >> 59u);
        return (x >> r) | (x << ((32 - r) & 31));
    }
    float f() { return float(u32() >> 8) * (1.0f / 16777216.0f); }
    V3 sphere() {
        const float z = 1.0f - 2.0f * f(), r = std::sqrt(std::max(0.0f, 1.0f - z * z)), p = 2.0f * kPi * f();
        return {r * std::cos(p), r * std::sin(p), z};
    }
    V3 hemi(V3 n) {
        const V3 d = sphere();
        return dot(d, n) < 0.0f ? -d : d;
    }
};

// 3x4 row-major object-to-world (OptixInstance::transform).
struct Xf {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    V3 point(V3 p) const {
        return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
    }
    V3 vector(V3 p) const {
        return {m[0] * p.x + m[1] * p.y + m[2] * p.z, m[4] * p.x + m[5] * p.y + m[6] * p.z, m[8] * p.x + m[9] * p.y + m[10] * p.z};
    }
};
// Rotation about a unit axis, uniform scale, then translation.
Xf makeXf(V3 axis, float angle, float scale, V3 t) {
    axis = normalize(axis);
    const float c = std::cos(angle), s = std::sin(angle), C = 1.0f - c;
    const float x = axis.x, y = axis.y, z = axis.z;
    const float r[9] = {c + x * x * C, x * y * C - z * s, x * z * C + y * s, y * x * C + z * s, c + y * y * C,
                        y * z * C - x * s, z * x * C - y * s, z * y * C + x * s, c + z * z * C};
    Xf f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            f.m[4 * i + j] = r[3 * i + j] * scale;
    }
    f.m[3] = t.x;
    f.m[7] = t.y;
    f.m[11] = t.z;
    return f;
}
Xf inverse(const Xf &a) { // general 3x4 inverse (double precision)
    double m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] = a.m[4 * i + j];
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    double inv[3][3] = {{(m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det,
                         (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det},
                        {(m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det,
                         (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det},
                        {(m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det,
                         (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det}};
    Xf r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[4 * i + j] = float(inv[i][j]);
        r.m[4 * i + 3] = float(-(inv[i][0] * a.m[3] + inv[i][1] * a.m[7] + inv[i][2] * a.m[11]));
    }
    return r;
}

// Nova's ray offsets (core/scene/ray.h): offsetRayOrigin, tubeRayOrigin.
V3 offsetRayOrigin(V3 p, V3 n) {
    constexpr float kOrigin = 1.0f / 32.0f, kFloatScale = 1.0f / 65536.0f, kIntScale = 256.0f;
    const int32_t ox = int32_t(kIntScale * n.x), oy = int32_t(kIntScale * n.y), oz = int32_t(kIntScale * n.z);
    auto bump = [](float f, int32_t o) { return std::bit_cast<float>(std::bit_cast<int32_t>(f) + (f < 0.0f ? -o : o)); };
    return {std::fabs(p.x) < kOrigin ? p.x + kFloatScale * n.x : bump(p.x, ox),
            std::fabs(p.y) < kOrigin ? p.y + kFloatScale * n.y : bump(p.y, oy),
            std::fabs(p.z) < kOrigin ? p.z + kFloatScale * n.z : bump(p.z, oz)};
}
V3 tubeRayOrigin(V3 P, V3 N, V3 T, float r, V3 d) {
    const float eps = 1e-3f * r;
    if (dot(d, N) >= 0.0f)
        return offsetRayOrigin(P + N * eps, N);
    if (std::fabs(dot(N, T)) > 1e-3f) {
        const float chord = -2.0f * r * dot(d, N);
        const V3 exitN = normalize(N + d * (chord / r));
        return offsetRayOrigin(P + d * chord + exitN * eps, exitN);
    }
    const V3 across = d - T * dot(d, T);
    const float aa = dot(across, across);
    if (!(aa > 0.0f) || !(r > 0.0f))
        return offsetRayOrigin(P, -N);
    const float chord = 2.0f * r * -dot(across, N) / aa;
    const V3 exitN = normalize(N + across * (chord / r));
    return offsetRayOrigin(P + d * chord + exitN * eps, exitN);
}

// ---------------------------------------------------------------------------
// Geometry

struct Mesh {
    std::vector<float> P; // xyz per vertex
    std::vector<uint32_t> I;
    uint32_t nv() const { return uint32_t(P.size() / 3); }
    uint32_t nt() const { return uint32_t(I.size() / 3); }
};

// A bumpy sphere of 2 * nu * nv triangles; `twist` rotates about y by
// twist * y radians, `bump` adds to the displacement.
Mesh makeBlob(uint32_t nu, uint32_t nv, float twist = 0.0f, float bump = 0.0f, V3 shift = {}) {
    Mesh m;
    m.P.resize(size_t(nu + 1) * (nv + 1) * 3);
    for (uint32_t j = 0; j <= nv; ++j)
        for (uint32_t i = 0; i <= nu; ++i) {
            const float th = kPi * float(j) / float(nv), ph = 2.0f * kPi * float(i) / float(nu);
            const float r = 1.0f + 0.08f * std::sin(7.0f * th) * std::cos(5.0f * ph) +
                            0.03f * std::sin(23.0f * ph) * std::sin(17.0f * th) + bump * std::sin(11.0f * ph + 3.0f * th);
            V3 p{r * std::sin(th) * std::cos(ph), r * std::cos(th), r * std::sin(th) * std::sin(ph)};
            if (twist != 0.0f) {
                const float a = twist * p.y, c = std::cos(a), s = std::sin(a);
                p = {c * p.x + s * p.z, p.y, -s * p.x + c * p.z};
            }
            st3(m.P, size_t(j) * (nu + 1) + i, p + shift);
        }
    m.I.reserve(size_t(nu) * nv * 6);
    for (uint32_t j = 0; j < nv; ++j)
        for (uint32_t i = 0; i < nu; ++i) {
            const uint32_t a = j * (nu + 1) + i, b = a + 1, c = a + nu + 1, d = c + 1;
            m.I.insert(m.I.end(), {a, c, b, b, c, d});
        }
    return m;
}

Mesh makeQuad(V3 c, float half, float y) {
    Mesh m;
    m.P = {c.x - half, y, c.z - half, c.x + half, y, c.z - half, c.x + half, y, c.z + half, c.x - half, y, c.z + half};
    m.I = {0, 2, 1, 0, 3, 2};
    return m;
}

// A build input and the buffers and host arrays it points at.
struct Input {
    OptixBuildInput in{};
    std::vector<Buf> bufs;
    std::vector<CUdeviceptr> a, b; // per motion key
    unsigned flag = OPTIX_GEOMETRY_FLAG_NONE;
    Input() = default;
    Input(const Input &) = delete;
    ~Input() {
        for (Buf &x : bufs)
            dfree(x);
    }
};
using InputP = std::unique_ptr<Input>;

InputP triangles(const std::vector<const std::vector<float> *> &keys, const std::vector<uint32_t> &idx, unsigned flag,
                 unsigned stride = 12) {
    auto g = std::make_unique<Input>();
    g->flag = flag;
    const size_t nv = keys[0]->size() / 3;
    for (const std::vector<float> *k : keys) {
        if (stride == 12) {
            g->bufs.push_back(dupload(*k));
        } else {
            std::vector<float> s(nv * 4, 0.0f);
            for (size_t i = 0; i < nv; ++i)
                std::memcpy(&s[4 * i], &(*k)[3 * i], 12);
            g->bufs.push_back(dupload(s));
        }
        g->a.push_back(g->bufs.back().p);
    }
    g->bufs.push_back(dupload(idx));
    g->in.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    OptixBuildInputTriangleArray &t = g->in.triangleArray;
    t.vertexBuffers = g->a.data();
    t.numVertices = unsigned(nv);
    t.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    t.vertexStrideInBytes = stride;
    t.indexBuffer = g->bufs.back().p;
    t.numIndexTriplets = unsigned(idx.size() / 3);
    t.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
    t.flags = &g->flag;
    t.numSbtRecords = 1;
    return g;
}

InputP curves(OptixPrimitiveType type, const std::vector<const std::vector<float> *> &keys,
              const std::vector<const std::vector<float> *> &widths, const std::vector<uint32_t> &idx, unsigned flag,
              unsigned endcap = 0) {
    auto g = std::make_unique<Input>();
    g->flag = flag;
    for (size_t k = 0; k < keys.size(); ++k) {
        g->bufs.push_back(dupload(*keys[k]));
        g->a.push_back(g->bufs.back().p);
        g->bufs.push_back(dupload(*widths[k]));
        g->b.push_back(g->bufs.back().p);
    }
    g->bufs.push_back(dupload(idx));
    g->in.type = OPTIX_BUILD_INPUT_TYPE_CURVES;
    OptixBuildInputCurveArray &c = g->in.curveArray;
    c.curveType = type;
    c.numPrimitives = unsigned(idx.size());
    c.vertexBuffers = g->a.data();
    c.numVertices = unsigned(keys[0]->size() / 3);
    c.widthBuffers = g->b.data();
    c.indexBuffer = g->bufs.back().p;
    c.flag = flag;
    c.endcapFlags = endcap;
    return g;
}

InputP spheres(const std::vector<float> &centers, const std::vector<float> &radii, unsigned flag) {
    auto g = std::make_unique<Input>();
    g->flag = flag;
    g->bufs.push_back(dupload(centers));
    g->a.push_back(g->bufs.back().p);
    g->bufs.push_back(dupload(radii));
    g->b.push_back(g->bufs.back().p);
    g->in.type = OPTIX_BUILD_INPUT_TYPE_SPHERES;
    OptixBuildInputSphereArray &s = g->in.sphereArray;
    s.vertexBuffers = g->a.data();
    s.numVertices = unsigned(centers.size() / 3);
    s.radiusBuffers = g->b.data();
    s.flags = &g->flag;
    s.numSbtRecords = 1;
    return g;
}

InputP aabbs(const std::vector<OptixAabb> &boxes, unsigned flag) {
    auto g = std::make_unique<Input>();
    g->flag = flag;
    g->bufs.push_back(dupload(boxes));
    g->a.push_back(g->bufs.back().p);
    g->in.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
    OptixBuildInputCustomPrimitiveArray &c = g->in.customPrimitiveArray;
    c.aabbBuffers = g->a.data();
    c.numPrimitives = unsigned(boxes.size());
    c.flags = &g->flag;
    c.numSbtRecords = 1;
    return g;
}

// ---------------------------------------------------------------------------
// Acceleration structures

struct Accel {
    Buf buf;
    OptixTraversableHandle h = 0;
    size_t out = 0, temp = 0, upd = 0, compacted = 0;
    float ms = 0.0f, compactMs = 0.0f;
    void release() { dfree(buf); }
};

OptixAccelBuildOptions buildOptions(unsigned flags, unsigned short keys, OptixBuildOperation op) {
    OptixAccelBuildOptions o{};
    o.buildFlags = flags;
    o.operation = op;
    o.motionOptions.numKeys = keys;
    o.motionOptions.timeBegin = 0.0f;
    o.motionOptions.timeEnd = 1.0f;
    return o;
}

// Builds `reps` times (median GPU time), then compacts if asked.
Accel buildAccel(const OptixBuildInput *in, unsigned n, unsigned flags, unsigned short keys = 1, bool compact = false,
                 int reps = 1) {
    const OptixAccelBuildOptions o =
        buildOptions(flags | (compact ? OPTIX_BUILD_FLAG_ALLOW_COMPACTION : 0u), keys, OPTIX_BUILD_OPERATION_BUILD);
    OptixAccelBufferSizes s{};
    OX(optixAccelComputeMemoryUsage(g_ctx, &o, in, n, &s));
    Accel a;
    a.out = s.outputSizeInBytes;
    a.temp = s.tempSizeInBytes;
    a.upd = s.tempUpdateSizeInBytes;
    Buf temp = dalloc(a.temp), out = dalloc(a.out), prop = dalloc(8);
    OptixAccelEmitDesc e{prop.p, OPTIX_PROPERTY_TYPE_COMPACTED_SIZE};
    Events ev;
    std::vector<float> t;
    for (int r = 0; r < reps; ++r) {
        ev.start();
        OX(optixAccelBuild(g_ctx, g_stream, &o, in, n, temp.p, temp.n, out.p, out.n, &a.h, compact ? &e : nullptr,
                           compact ? 1u : 0u));
        t.push_back(ev.stop());
    }
    a.ms = median(t);
    dfree(temp);
    if (compact) {
        uint64_t size = 0;
        d2h(&size, prop.p, 8);
        a.compacted = size;
        Buf c = dalloc(size);
        ev.start();
        OX(optixAccelCompact(g_ctx, g_stream, a.h, c.p, size, &a.h));
        a.compactMs = ev.stop();
        dfree(out);
        out = c;
    }
    dfree(prop);
    a.buf = out;
    return a;
}

float updateAccel(Accel &a, const OptixBuildInput *in, unsigned n, unsigned flags, unsigned short keys = 1) {
    const OptixAccelBuildOptions o = buildOptions(flags, keys, OPTIX_BUILD_OPERATION_UPDATE);
    Buf temp = dalloc(std::max<size_t>(a.upd, 8));
    Events ev;
    ev.start();
    OX(optixAccelBuild(g_ctx, g_stream, &o, in, n, temp.p, temp.n, a.buf.p, a.buf.n, &a.h, nullptr, 0));
    const float ms = ev.stop();
    dfree(temp);
    return ms;
}

OptixInstance makeInstance(const Xf &xf, unsigned id, OptixTraversableHandle h, unsigned flags = 0, unsigned sbt = 0,
                           unsigned mask = 255) {
    OptixInstance i{};
    std::memcpy(i.transform, xf.m, sizeof(i.transform));
    i.instanceId = id;
    i.sbtOffset = sbt;
    i.visibilityMask = mask;
    i.flags = flags;
    i.traversableHandle = h;
    return i;
}

Accel buildIas(const std::vector<OptixInstance> &inst, Buf &instBuf, unsigned flags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE,
               unsigned short keys = 1, int reps = 1) {
    dfree(instBuf);
    instBuf = dupload(inst);
    OptixBuildInput in{};
    in.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    in.instanceArray.instances = instBuf.p;
    in.instanceArray.numInstances = unsigned(inst.size());
    return buildAccel(&in, 1, flags, keys, false, reps);
}

double mb(size_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

// ---------------------------------------------------------------------------
// Pipelines

struct HitGroupDesc {
    OptixPrimitiveType type = OPTIX_PRIMITIVE_TYPE_TRIANGLE;
    const char *ah = nullptr; // any-hit entry
    const char *is = nullptr; // custom intersection entry
    unsigned endcap = 0;
    unsigned buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
    OaPointData data{};
};

struct PipeCfg {
    int motion = 0;
    unsigned graph = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
    std::vector<HitGroupDesc> groups{HitGroupDesc{}};
    unsigned depth = 2;
    bool ptx = false;
};

enum Raygen { RG_HITOBJ, RG_PAYLOAD, RG_OCCL_TRACE, RG_OCCL_HITOBJ, RG_EMPTY, RG_INSPECT, RG_COUNT };
const char *kRaygenNames[RG_COUNT] = {"__raygen__hitobj", "__raygen__payload",  "__raygen__occl_trace",
                                      "__raygen__occl_hitobj", "__raygen__empty", "__raygen__inspect"};

unsigned primTypeFlag(OptixPrimitiveType t) {
    switch (t) {
    case OPTIX_PRIMITIVE_TYPE_CUSTOM: return OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM;
    case OPTIX_PRIMITIVE_TYPE_ROUND_QUADRATIC_BSPLINE: return OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_QUADRATIC_BSPLINE;
    case OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE: return OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_CUBIC_BSPLINE;
    case OPTIX_PRIMITIVE_TYPE_ROUND_LINEAR: return OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_LINEAR;
    case OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM: return OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_CATMULLROM;
    case OPTIX_PRIMITIVE_TYPE_FLAT_QUADRATIC_BSPLINE: return OPTIX_PRIMITIVE_TYPE_FLAGS_FLAT_QUADRATIC_BSPLINE;
    case OPTIX_PRIMITIVE_TYPE_SPHERE: return OPTIX_PRIMITIVE_TYPE_FLAGS_SPHERE;
    case OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BEZIER: return OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_CUBIC_BEZIER;
    case OPTIX_PRIMITIVE_TYPE_ROUND_QUADRATIC_BSPLINE_ROCAPS: return OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_QUADRATIC_BSPLINE_ROCAPS;
    case OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE_ROCAPS: return OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_CUBIC_BSPLINE_ROCAPS;
    case OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM_ROCAPS: return OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_CATMULLROM_ROCAPS;
    case OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BEZIER_ROCAPS: return OPTIX_PRIMITIVE_TYPE_FLAGS_ROUND_CUBIC_BEZIER_ROCAPS;
    default: return OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;
    }
}

std::vector<char> readFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        fail("cannot read %s", path.c_str());
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

OptixPipelineCompileOptions compileOptions(const PipeCfg &c) {
    OptixPipelineCompileOptions p{};
    p.usesMotionBlur = c.motion;
    p.traversableGraphFlags = c.graph;
    p.numPayloadValues = 5;
    p.numAttributeValues = 2;
    p.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    p.pipelineLaunchParamsVariableName = "params";
    p.pipelineLaunchParamsSizeInBytes = sizeof(OaParams);
    unsigned flags = 0;
    for (const HitGroupDesc &g : c.groups)
        flags |= primTypeFlag(g.type);
    p.usesPrimitiveTypeFlags = flags;
    return p;
}

OptixModuleCompileOptions moduleOptions() {
    OptixModuleCompileOptions m{};
    m.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    m.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
    return m;
}

struct Pipeline {
    static constexpr size_t kRec = 64; // 32-byte header + OaPointData, 16-byte aligned
    OptixModule mod = nullptr;
    std::vector<OptixModule> builtin;
    std::vector<OptixProgramGroup> pgs;
    OptixPipeline pipe = nullptr;
    Buf rec, params;
    OptixShaderBindingTable sbt{};
    float moduleMs = 0.0f, totalMs = 0.0f;

    explicit Pipeline(const PipeCfg &c) {
        const double t0 = now();
        const OptixModuleCompileOptions mco = moduleOptions();
        const OptixPipelineCompileOptions pco = compileOptions(c);
        const std::vector<char> src = readFile(g_dir + (c.ptx ? "oa_device.ptx" : "oa_device.optixir"));
        char log[4096];
        size_t ls = sizeof(log);
        OX(optixModuleCreate(g_ctx, &mco, &pco, src.data(), src.size(), log, &ls, &mod));
        moduleMs = float(now() - t0);
        auto create = [&](const OptixProgramGroupDesc &d) {
            OptixProgramGroupOptions o{};
            OptixProgramGroup g;
            size_t l = sizeof(log);
            OX(optixProgramGroupCreate(g_ctx, &d, 1, &o, log, &l, &g));
            pgs.push_back(g);
        };
        for (const char *name : kRaygenNames) {
            OptixProgramGroupDesc d{};
            d.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
            d.raygen.module = mod;
            d.raygen.entryFunctionName = name;
            create(d);
        }
        {
            OptixProgramGroupDesc d{};
            d.kind = OPTIX_PROGRAM_GROUP_KIND_MISS; // miss 0: no program
            create(d);
            d.miss.module = mod;
            d.miss.entryFunctionName = "__miss__occl";
            create(d);
        }
        for (const HitGroupDesc &g : c.groups) {
            OptixProgramGroupDesc d{};
            d.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
            d.hitgroup.moduleCH = mod;
            d.hitgroup.entryFunctionNameCH = "__closesthit__payload";
            if (g.ah) {
                d.hitgroup.moduleAH = mod;
                d.hitgroup.entryFunctionNameAH = g.ah;
            }
            if (g.type == OPTIX_PRIMITIVE_TYPE_CUSTOM) {
                d.hitgroup.moduleIS = mod;
                d.hitgroup.entryFunctionNameIS = g.is;
            } else if (g.type != OPTIX_PRIMITIVE_TYPE_TRIANGLE) {
                OptixBuiltinISOptions bo{};
                bo.builtinISModuleType = g.type;
                bo.usesMotionBlur = c.motion;
                bo.buildFlags = g.buildFlags;
                bo.curveEndcapFlags = g.endcap;
                OptixModule m;
                OX(optixBuiltinISModuleGet(g_ctx, &mco, &pco, &bo, &m));
                builtin.push_back(m);
                d.hitgroup.moduleIS = m;
            }
            create(d);
        }
        OptixPipelineLinkOptions plo{};
        plo.maxTraceDepth = 1;
        plo.maxTraversableGraphDepth = c.depth;
        ls = sizeof(log);
        OX(optixPipelineCreate(g_ctx, &pco, &plo, pgs.data(), unsigned(pgs.size()), log, &ls, &pipe));
        OptixStackSizes ss{};
        for (OptixProgramGroup g : pgs)
            OX(optixUtilAccumulateStackSizes(g, &ss, pipe));
        unsigned dcTrav = 0, dcState = 0, cont = 0;
        OX(optixUtilComputeStackSizes(&ss, 1, 0, 0, &dcTrav, &dcState, &cont));
        OX(optixPipelineSetStackSize(pipe, dcTrav, dcState, cont, c.depth));

        std::vector<char> host(pgs.size() * kRec, 0);
        for (size_t i = 0; i < pgs.size(); ++i)
            OX(optixSbtRecordPackHeader(pgs[i], host.data() + i * kRec));
        for (size_t i = 0; i < c.groups.size(); ++i)
            std::memcpy(host.data() + (RG_COUNT + 2 + i) * kRec + OPTIX_SBT_RECORD_HEADER_SIZE, &c.groups[i].data,
                        sizeof(OaPointData));
        rec = dupload(host);
        sbt.raygenRecord = rec.p;
        sbt.missRecordBase = rec.p + RG_COUNT * kRec;
        sbt.missRecordStrideInBytes = kRec;
        sbt.missRecordCount = 2;
        sbt.hitgroupRecordBase = rec.p + (RG_COUNT + 2) * kRec;
        sbt.hitgroupRecordStrideInBytes = kRec;
        sbt.hitgroupRecordCount = unsigned(c.groups.size());
        params = dalloc(sizeof(OaParams));
        totalMs = float(now() - t0);
    }
    ~Pipeline() {
        optixPipelineDestroy(pipe);
        for (OptixProgramGroup g : pgs)
            optixProgramGroupDestroy(g);
        optixModuleDestroy(mod); // built-in IS modules belong to the context
        dfree(rec);
        dfree(params);
    }
    void launch(const OaParams &p, Raygen rg, unsigned width, bool upload = true) {
        if (upload)
            CU(cu.p_cuMemcpyHtoDAsync(params.p, &p, sizeof(p), g_stream));
        OptixShaderBindingTable s = sbt;
        s.raygenRecord = rec.p + size_t(rg) * kRec;
        OX(optixLaunch(pipe, g_stream, params.p, sizeof(OaParams), &s, width, 1, 1));
    }
};

// ---------------------------------------------------------------------------
// Rays and hits (SoA, as Nova's RayBatch / HitBatch)

struct Rays {
    uint32_t n = 0;
    std::vector<float> org, dir, tmax, time, seed;
    Buf dOrg, dDir, dTmax, dTime, dSeed;
    explicit Rays(uint32_t count = 0) { resize(count); }
    Rays(const Rays &) = delete;
    ~Rays() { freeDevice(); }
    void resize(uint32_t count) {
        n = count;
        org.assign(size_t(count) * 3, 0.0f);
        dir.assign(size_t(count) * 3, 0.0f);
        tmax.assign(count, 1e30f);
        time.assign(count, 0.0f);
        seed.assign(count, 0.5f);
    }
    void set(uint32_t i, V3 o, V3 d, float tm = 1e30f) {
        st3(org, i, o);
        st3(dir, i, d);
        tmax[i] = tm;
    }
    void freeDevice() {
        for (Buf *b : {&dOrg, &dDir, &dTmax, &dTime, &dSeed})
            dfree(*b);
    }
    void upload() {
        freeDevice();
        dOrg = dupload(org);
        dDir = dupload(dir);
        dTmax = dupload(tmax);
        dTime = dupload(time);
        dSeed = dupload(seed);
    }
};

struct Hits {
    uint32_t n = 0;
    Buf t, prim, inst, uv, occl, insp;
    std::vector<float> ht, huv, hinsp;
    std::vector<uint32_t> hprim, hinst;
    std::vector<uint8_t> hoccl;
    explicit Hits(uint32_t count, bool inspect = false) : n(count) {
        t = dalloc(size_t(count) * 4);
        prim = dalloc(size_t(count) * 4);
        inst = dalloc(size_t(count) * 4);
        uv = dalloc(size_t(count) * 8);
        occl = dalloc(count);
        if (inspect)
            insp = dalloc(size_t(count) * kInspectStride * 4);
    }
    Hits(const Hits &) = delete;
    ~Hits() {
        for (Buf *b : {&t, &prim, &inst, &uv, &occl, &insp})
            dfree(*b);
    }
    void download(uint32_t count) {
        ddownload(ht, t, count);
        ddownload(hprim, prim, count);
        ddownload(hinst, inst, count);
        ddownload(huv, uv, size_t(count) * 2);
        ddownload(hoccl, occl, count);
        if (insp.p)
            ddownload(hinsp, insp, size_t(count) * kInspectStride);
    }
};

OaParams makeParams(OptixTraversableHandle h, const Rays &r, Hits &hh, bool useTime = false) {
    OaParams p{};
    p.handle = h;
    p.org = reinterpret_cast<const float *>(r.dOrg.p);
    p.dir = reinterpret_cast<const float *>(r.dDir.p);
    p.tmax = reinterpret_cast<const float *>(r.dTmax.p);
    p.time = useTime ? reinterpret_cast<const float *>(r.dTime.p) : nullptr;
    p.seed = reinterpret_cast<const float *>(r.dSeed.p);
    p.outT = reinterpret_cast<float *>(hh.t.p);
    p.outPrim = reinterpret_cast<unsigned *>(hh.prim.p);
    p.outInst = reinterpret_cast<unsigned *>(hh.inst.p);
    p.outUV = reinterpret_cast<float *>(hh.uv.p);
    p.outOccl = reinterpret_cast<unsigned char *>(hh.occl.p);
    p.outInspect = reinterpret_cast<float *>(hh.insp.p);
    p.opacity = 1.0f;
    p.mask = 255;
    return p;
}

struct TraceResult {
    double mrays = 0, ms = 0, hitFrac = 0;
};

// Median GPU time of `reps` launches of `count` rays with the given params.
TraceResult traceParams(Pipeline &pl, const OaParams &p, Hits &hh, Raygen rg, uint32_t count, int reps = 9) {
    Events ev;
    for (int i = 0; i < 2; ++i)
        pl.launch(p, rg, count);
    std::vector<float> t;
    for (int i = 0; i < reps; ++i) {
        ev.start();
        pl.launch(p, rg, count);
        t.push_back(ev.stop());
    }
    TraceResult res;
    res.ms = median(t);
    res.mrays = count / (res.ms * 1e-3) * 1e-6;
    uint64_t hits = 0;
    if (rg == RG_OCCL_TRACE || rg == RG_OCCL_HITOBJ) {
        ddownload(hh.hoccl, hh.occl, count);
        for (uint32_t i = 0; i < count; ++i)
            hits += hh.hoccl[i];
    } else {
        ddownload(hh.hprim, hh.prim, count);
        for (uint32_t i = 0; i < count; ++i)
            hits += hh.hprim[i] != ~0u;
    }
    res.hitFrac = double(hits) / count;
    return res;
}

// The same over the first `count` rays of `r` (all if 0).
TraceResult trace(Pipeline &pl, OptixTraversableHandle h, Rays &r, Hits &hh, Raygen rg, unsigned rayFlags = 0,
                  float opacity = 1.0f, bool useTime = false, int reps = 9, uint32_t count = 0) {
    OaParams p = makeParams(h, r, hh, useTime);
    p.rayFlags = rayFlags;
    p.opacity = opacity;
    return traceParams(pl, p, hh, rg, count ? count : r.n, reps);
}

void printTrace(const char *label, const TraceResult &r) {
    std::printf("  %-44s %8.1f Mrays/s  %7.3f ms  hit %5.1f %%\n", label, r.mrays, r.ms, 100.0 * r.hitFrac);
}

// ---------------------------------------------------------------------------
// Ray sets

void cameraRays(Rays &r, V3 eye, V3 at, float fovDeg, uint32_t w, uint32_t h, Rng *rng = nullptr) {
    r.resize(w * h);
    const V3 f = normalize(at - eye), u0{0, 1, 0};
    const V3 rt = normalize(cross(f, u0)), up = cross(rt, f);
    const float tanH = std::tan(0.5f * fovDeg * kPi / 180.0f), aspect = float(w) / float(h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float sx = (2.0f * (x + 0.5f) / w - 1.0f) * tanH * aspect, sy = (1.0f - 2.0f * (y + 0.5f) / h) * tanH;
            const uint32_t i = y * w + x;
            r.set(i, eye, normalize(f + rt * sx + up * sy));
            if (rng) {
                r.time[i] = rng->f();
                r.seed[i] = rng->f();
            }
        }
}

// Diffuse-like bounces from the hits of `prim` (origin just before the hit,
// direction in the hemisphere facing back along the ray); misses become
// random rays from inside the box.
void bounceRays(Rays &out, const Rays &prim, const Hits &hh, V3 lo, V3 hi, Rng &rng, bool sphereDirs = false) {
    out.resize(prim.n);
    for (uint32_t i = 0; i < prim.n; ++i) {
        const V3 o = ld3(prim.org, i), d = ld3(prim.dir, i);
        if (hh.hprim[i] != ~0u) {
            const float t = hh.ht[i];
            const V3 nd = sphereDirs ? rng.sphere() : rng.hemi(-d);
            out.set(i, o + d * (t * (1.0f - 1e-5f) - 1e-4f), nd);
        } else {
            out.set(i, {lo.x + (hi.x - lo.x) * rng.f(), lo.y + (hi.y - lo.y) * rng.f(), lo.z + (hi.z - lo.z) * rng.f()},
                    rng.sphere());
        }
        out.time[i] = prim.time[i];
        out.seed[i] = rng.f();
    }
}

// Shadow rays from the hits towards random points of a square light.
void shadowRays(Rays &out, const Rays &prim, const Hits &hh, V3 light, float half, V3 lo, V3 hi, Rng &rng) {
    out.resize(prim.n);
    for (uint32_t i = 0; i < prim.n; ++i) {
        const V3 o = ld3(prim.org, i), d = ld3(prim.dir, i);
        const V3 from = hh.hprim[i] != ~0u ? o + d * (hh.ht[i] * (1.0f - 1e-5f) - 1e-4f)
                                           : V3{lo.x + (hi.x - lo.x) * rng.f(), lo.y + (hi.y - lo.y) * rng.f(),
                                                lo.z + (hi.z - lo.z) * rng.f()};
        const V3 to{light.x + half * (2.0f * rng.f() - 1.0f), light.y, light.z + half * (2.0f * rng.f() - 1.0f)};
        const V3 v = to - from;
        const float dist = length(v);
        out.set(i, from, v * (1.0f / dist), dist * (1.0f - 1e-4f));
        out.time[i] = prim.time[i];
        out.seed[i] = rng.f();
    }
}

// ---------------------------------------------------------------------------
// The test world: 8 x 8 instances of a 1M-triangle blob on a ground quad.

struct World {
    std::vector<InputP> inputs;
    std::vector<Accel> gas;
    std::vector<OptixInstance> inst;
    Buf instBuf;
    Accel ias;
    World() = default;
    World(const World &) = delete;
    ~World() {
        for (Accel &g : gas)
            g.release();
        ias.release();
        dfree(instBuf);
    }
};

const V3 kGridLo{-1.5f, -1.2f, -1.5f}, kGridHi{19.0f, 1.5f, 19.0f};
const V3 kGridEye{8.75f, 7.0f, -8.0f}, kGridAt{8.75f, 0.0f, 6.0f}, kGridLight{8.75f, 15.0f, 8.75f};

// linkMasks: even blobs get visibility mask 0x81, odd blobs and the ground
// 0x80 (shadow-link set 0 = the even blobs).
std::unique_ptr<World> gridWorld(const Mesh &blob, unsigned geomFlag, unsigned instFlags = 0, int onlyFiltered = -1,
                                 unsigned gasFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, bool linkMasks = false) {
    auto w = std::make_unique<World>();
    const Mesh ground = makeQuad({8.75f, 0, 8.75f}, 40.0f, -1.2f);
    w->inputs.push_back(triangles({&blob.P}, blob.I, geomFlag));
    w->inputs.push_back(triangles({&ground.P}, ground.I, geomFlag));
    for (InputP &in : w->inputs)
        w->gas.push_back(buildAccel(&in->in, 1, gasFlags, 1, true));
    Rng rng(7);
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i) {
            const unsigned id = unsigned(j * 8 + i);
            const unsigned flags = onlyFiltered < 0 ? instFlags : (int(id) == onlyFiltered ? 0u : instFlags);
            w->inst.push_back(makeInstance(makeXf(rng.sphere(), 6.28f * rng.f(), 0.8f + 0.4f * rng.f(), {2.5f * i, 0, 2.5f * j}),
                                           id, w->gas[0].h, flags, 0, linkMasks ? (id % 2 == 0 ? 0x81u : 0x80u) : 255u));
        }
    w->inst.push_back(makeInstance(Xf{}, 64, w->gas[1].h, onlyFiltered < 0 ? instFlags : 0u, 0, linkMasks ? 0x80u : 255u));
    w->ias = buildIas(w->inst, w->instBuf);
    return w;
}

// ===========================================================================
// Tests

void testInfo() {
    std::printf("== info\n");
    int drv = 0;
    CU(cu.p_cuDriverGetVersion(&drv));
    char name[256] = {};
    CU(cu.p_cuDeviceGetName(name, sizeof(name), 0));
    int sms = 0, ccMaj = 0, ccMin = 0;
    CU(cu.p_cuDeviceGetAttribute(&sms, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, 0));
    CU(cu.p_cuDeviceGetAttribute(&ccMaj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, 0));
    CU(cu.p_cuDeviceGetAttribute(&ccMin, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, 0));
    std::printf("device %s, sm_%d%d, %d SMs, CUDA driver API %d, OptiX headers %d (ABI %d)\n", name, ccMaj, ccMin, sms, drv,
                OPTIX_VERSION, OPTIX_ABI_VERSION);
    const struct {
        OptixDeviceProperty p;
        const char *n;
    } props[] = {{OPTIX_DEVICE_PROPERTY_LIMIT_MAX_TRACE_DEPTH, "max trace depth"},
                 {OPTIX_DEVICE_PROPERTY_LIMIT_MAX_TRAVERSABLE_GRAPH_DEPTH, "max traversable graph depth"},
                 {OPTIX_DEVICE_PROPERTY_LIMIT_MAX_PRIMITIVES_PER_GAS, "max primitives per GAS"},
                 {OPTIX_DEVICE_PROPERTY_LIMIT_MAX_INSTANCES_PER_IAS, "max instances per IAS"},
                 {OPTIX_DEVICE_PROPERTY_RTCORE_VERSION, "RT core version"},
                 {OPTIX_DEVICE_PROPERTY_LIMIT_MAX_INSTANCE_ID, "max instance id"},
                 {OPTIX_DEVICE_PROPERTY_LIMIT_NUM_BITS_INSTANCE_VISIBILITY_MASK, "visibility mask bits"},
                 {OPTIX_DEVICE_PROPERTY_LIMIT_MAX_SBT_RECORDS_PER_GAS, "max SBT records per GAS"},
                 {OPTIX_DEVICE_PROPERTY_LIMIT_MAX_SBT_OFFSET, "max SBT offset"},
                 {OPTIX_DEVICE_PROPERTY_SHADER_EXECUTION_REORDERING, "shader execution reordering flags"},
                 {OPTIX_DEVICE_PROPERTY_CLUSTER_ACCEL, "cluster accel flags"}};
    for (const auto &p : props) {
        unsigned v = 0;
        const OptixResult r = optixDeviceContextGetProperty(g_ctx, p.p, &v, sizeof(v));
        if (r == OPTIX_SUCCESS)
            std::printf("  %-36s %u (0x%x)\n", p.n, v, v);
        else
            std::printf("  %-36s error %s\n", p.n, optixGetErrorName(r));
    }

    // Module creation: OptiX-IR and PTX, cache off, cold and warm.
    std::printf("module creation (all probe programs; wall ms):\n");
    PipeCfg cfg;
    const OptixModuleCompileOptions mco = moduleOptions();
    const OptixPipelineCompileOptions pco = compileOptions(cfg);
    for (int ptx = 0; ptx < 2; ++ptx) {
        const std::vector<char> src = readFile(g_dir + (ptx ? "oa_device.ptx" : "oa_device.optixir"));
        const std::string coldDir = g_dir + "optix_cache_cold_" + std::to_string(GetTickCount64()) + (ptx ? "_ptx" : "_ir");
        CreateDirectoryA(coldDir.c_str(), nullptr);
        const char *modes[] = {"cache off", "cache cold", "cache warm"};
        for (int mode = 0; mode < 3; ++mode) {
            OX(optixDeviceContextSetCacheEnabled(g_ctx, mode == 0 ? 0 : 1));
            if (mode == 1)
                OX(optixDeviceContextSetCacheLocation(g_ctx, coldDir.c_str()));
            OptixModule m;
            const double t0 = now();
            OX(optixModuleCreate(g_ctx, &mco, &pco, src.data(), src.size(), nullptr, nullptr, &m));
            const double ms = now() - t0;
            optixModuleDestroy(m);
            std::printf("  %-8s %-11s %8.1f ms  (%zu bytes of input)\n", ptx ? "PTX" : "OptiX-IR", modes[mode], ms, src.size());
        }
    }
    OX(optixDeviceContextSetCacheEnabled(g_ctx, 1));
    OX(optixDeviceContextSetCacheLocation(g_ctx, (g_dir + "optix_cache").c_str()));
    // A whole pipeline: triangles, a curve type, spheres, custom points.
    PipeCfg big;
    big.groups = {HitGroupDesc{}, HitGroupDesc{OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE},
                  HitGroupDesc{OPTIX_PRIMITIVE_TYPE_SPHERE}, HitGroupDesc{OPTIX_PRIMITIVE_TYPE_CUSTOM, nullptr, "__intersection__disc"},
                  HitGroupDesc{OPTIX_PRIMITIVE_TYPE_TRIANGLE, "__anyhit__opacity"}};
    for (int i = 0; i < 2; ++i) {
        Pipeline p(big);
        std::printf("pipeline (5 hit groups, 6 raygens), %s: module %.1f ms, total %.1f ms\n", i ? "second" : "first",
                    p.moduleMs, p.totalMs);
    }
    size_t freeB = 0, totalB = 0;
    CU(cu.p_cuMemGetInfo(&freeB, &totalB));
    std::printf("device memory: %.0f MB free of %.0f MB\n", mb(freeB), mb(totalB));
}

// --- triangles: build, compaction, refit --------------------------------------

void testTriangles() {
    std::printf("== tri: GAS builds of the blob (GPU ms, median of 3)\n");
    const struct {
        const char *n;
        unsigned flags;
        bool compact;
    } variants[] = {{"FAST_TRACE", OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, false},
                    {"FAST_BUILD", OPTIX_BUILD_FLAG_PREFER_FAST_BUILD, false},
                    {"default (no preference)", OPTIX_BUILD_FLAG_NONE, false},
                    {"FAST_TRACE|ALLOW_UPDATE", OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_UPDATE, false},
                    {"FAST_TRACE + compaction", OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, true},
                    {"FAST_BUILD + compaction", OPTIX_BUILD_FLAG_PREFER_FAST_BUILD, true},
                    {"FAST_TRACE|ALLOW_UPDATE + compaction", OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_UPDATE, true}};
    const uint32_t sizes[][2] = {{1000, 500}, {2000, 1000}, {4000, 2000}};
    for (const auto &s : sizes) {
        const double g0 = now();
        const Mesh m = makeBlob(s[0], s[1]);
        const double genMs = now() - g0;
        const double u0 = now();
        InputP in = triangles({&m.P}, m.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        const double upMs = now() - u0;
        std::printf("%u triangles, %u vertices (input %.1f MB; host upload %.1f ms, generation %.0f ms)\n", m.nt(), m.nv(),
                    mb(m.P.size() * 4 + m.I.size() * 4), upMs, genMs);
        for (const auto &v : variants) {
            Accel a = buildAccel(&in->in, 1, v.flags, 1, v.compact, 3);
            std::printf("  %-38s build %8.2f ms  out %7.1f MB  temp %7.1f MB  update-temp %6.1f MB", v.n, a.ms, mb(a.out),
                        mb(a.temp), mb(a.upd));
            if (v.compact)
                std::printf("  compacted %7.1f MB (%.0f %%) in %.2f ms", mb(a.compacted), 100.0 * a.compacted / a.out,
                            a.compactMs);
            std::printf("\n");
            a.release();
        }
        InputP in16 = triangles({&m.P}, m.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT, 16);
        Accel a16 = buildAccel(&in16->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, false, 3);
        std::printf("  %-38s build %8.2f ms  (16-byte vertex stride)\n", "FAST_TRACE, float4 vertices", a16.ms);
        a16.release();
    }

    std::printf("tracing the 1M blob (one instance), primary 1920x1080 and bounce rays:\n");
    Mesh m = makeBlob(1000, 500);
    InputP in = triangles({&m.P}, m.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
    Pipeline pl(PipeCfg{});
    Rays primary, bounce;
    cameraRays(primary, {0.3f, 0.4f, -3.2f}, {0, 0, 0}, 45.0f, 1920, 1080);
    primary.upload();
    Hits hits(primary.n);
    Rng rng(3);
    Buf instBuf;
    auto traceBoth = [&](const char *label, Accel &gas) {
        std::vector<OptixInstance> inst{makeInstance(Xf{}, 0, gas.h)};
        Accel ias = buildIas(inst, instBuf);
        char l[128];
        std::snprintf(l, sizeof(l), "%s primary", label);
        printTrace(l, trace(pl, ias.h, primary, hits, RG_HITOBJ));
        if (bounce.n == 0) {
            hits.download(primary.n);
            bounceRays(bounce, primary, hits, {-1.5f, -1.5f, -1.5f}, {1.5f, 1.5f, 1.5f}, rng);
            bounce.upload();
        }
        std::snprintf(l, sizeof(l), "%s bounce", label);
        printTrace(l, trace(pl, ias.h, bounce, hits, RG_HITOBJ));
        ias.release();
    };
    for (const auto &v : variants) {
        Accel a = buildAccel(&in->in, 1, v.flags, 1, v.compact);
        traceBoth(v.n, a);
        a.release();
    }

    std::printf("refit (OPTIX_BUILD_OPERATION_UPDATE) of the 1M blob, FAST_TRACE|ALLOW_UPDATE:\n");
    const unsigned uflags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_UPDATE;
    Accel a = buildAccel(&in->in, 1, uflags);
    std::printf("  build %.2f ms, update temp %.2f MB\n", a.ms, mb(a.upd));
    const struct {
        const char *n;
        float twist, bump;
    } edits[] = {{"small edit (bump +0.02)", 0.0f, 0.02f}, {"large edit (twist 1.5 rad per unit y)", 1.5f, 0.0f},
                 {"very large edit (twist 4 rad, bump 0.1)", 4.0f, 0.1f}};
    for (const auto &e : edits) {
        Accel b = buildAccel(&in->in, 1, uflags); // fresh build of the original
        const Mesh d = makeBlob(1000, 500, e.twist, e.bump);
        const double h0 = now();
        h2d(in->bufs[0].p, d.P.data(), d.P.size() * 4);
        const double upMs = now() - h0;
        std::vector<float> ut;
        for (int r = 0; r < 3; ++r)
            ut.push_back(updateAccel(b, &in->in, 1, uflags));
        std::printf("  %-40s vertex upload %.2f ms, update %.3f ms (median of 3)\n", e.n, upMs, median(ut));
        traceBoth("    after update:", b);
        Accel c = buildAccel(&in->in, 1, uflags);
        std::printf("    rebuild instead: %.2f ms\n", c.ms);
        traceBoth("    after rebuild:", c);
        b.release();
        c.release();
        h2d(in->bufs[0].p, m.P.data(), m.P.size() * 4); // restore
    }
    a.release();
    dfree(instBuf);
}

// --- the instanced world: wavefront query designs and pipeline flags ---------

void testTrace() {
    std::printf("== trace: 64 instances of a 1M-triangle blob + ground (64M instanced triangles)\n");
    const Mesh blob = makeBlob(1000, 500);
    auto w = gridWorld(blob, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
    std::printf("GAS %.2f ms (%.1f MB compacted), IAS of 65 instances %.3f ms (%.3f MB)\n", w->gas[0].ms,
                mb(w->gas[0].compacted), w->ias.ms, mb(w->ias.out));
    Rays primary, bounce, shadow, primary4k;
    cameraRays(primary, kGridEye, kGridAt, 55.0f, 1920, 1080);
    cameraRays(primary4k, kGridEye, kGridAt, 55.0f, 3840, 2160);
    primary.upload();
    primary4k.upload();
    Hits hits(primary4k.n);
    Rng rng(11);
    {
        Pipeline pl(PipeCfg{});
        trace(pl, w->ias.h, primary, hits, RG_HITOBJ, 0, 1.0f, false, 1);
        hits.download(primary.n);
        bounceRays(bounce, primary, hits, kGridLo, kGridHi, rng);
        shadowRays(shadow, primary, hits, kGridLight, 3.0f, kGridLo, kGridHi, rng);
        bounce.upload();
        shadow.upload();
    }
    const struct {
        const char *n;
        PipeCfg c;
    } cfgs[] = {
        {"SINGLE_LEVEL_INSTANCING, no motion", PipeCfg{}},
        {"SINGLE_GAS|SINGLE_LEVEL_INSTANCING", PipeCfg{0, OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS |
                                                              OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING}},
        {"ALLOW_ANY (depth 2)", PipeCfg{0, OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY}},
        {"SINGLE_LEVEL_INSTANCING, usesMotionBlur", PipeCfg{1}},
        {"ALLOW_ANY (depth 3), usesMotionBlur", PipeCfg{1, OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY, {HitGroupDesc{}}, 3}},
        {"SINGLE_LEVEL_INSTANCING, PTX module", PipeCfg{0, OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING,
                                                        {HitGroupDesc{}}, 2, true}},
    };
    for (const auto &c : cfgs) {
        Pipeline pl(c.c);
        std::printf("pipeline: %s\n", c.n);
        printTrace("closest, hit object: primary 1080p", trace(pl, w->ias.h, primary, hits, RG_HITOBJ));
        printTrace("closest, hit object: bounce", trace(pl, w->ias.h, bounce, hits, RG_HITOBJ));
        printTrace("closest, payload + CH: primary 1080p", trace(pl, w->ias.h, primary, hits, RG_PAYLOAD));
        printTrace("closest, payload + CH: bounce", trace(pl, w->ias.h, bounce, hits, RG_PAYLOAD));
        printTrace("occlusion, trace + miss: shadow", trace(pl, w->ias.h, shadow, hits, RG_OCCL_TRACE));
        printTrace("occlusion, hit object: shadow", trace(pl, w->ias.h, shadow, hits, RG_OCCL_HITOBJ));
        printTrace("occlusion, hit object: bounce (tmax inf)", trace(pl, w->ias.h, bounce, hits, RG_OCCL_HITOBJ));
        if (&c == &cfgs[0])
            printTrace("closest, hit object: primary 2160p", trace(pl, w->ias.h, primary4k, hits, RG_HITOBJ));
    }
    // The same rays against one blob only (no instancing).
    {
        std::vector<OptixInstance> one{makeInstance(Xf{}, 0, w->gas[0].h)};
        Buf ib;
        Accel ias = buildIas(one, ib);
        Pipeline pl(PipeCfg{});
        Rays p1, b1;
        cameraRays(p1, {0.3f, 0.4f, -3.2f}, {0, 0, 0}, 45.0f, 1920, 1080);
        p1.upload();
        trace(pl, ias.h, p1, hits, RG_HITOBJ, 0, 1.0f, false, 1);
        hits.download(p1.n);
        bounceRays(b1, p1, hits, {-1.5f, -1.5f, -1.5f}, {1.5f, 1.5f, 1.5f}, rng);
        b1.upload();
        std::printf("one blob (IAS of 1 instance), baseline pipeline:\n");
        printTrace("closest, hit object: primary 1080p", trace(pl, ias.h, p1, hits, RG_HITOBJ));
        printTrace("closest, hit object: bounce", trace(pl, ias.h, b1, hits, RG_HITOBJ));
        ias.release();
        dfree(ib);
    }
}

// --- any-hit ----------------------------------------------------------------

void testAnyHit() {
    std::printf("== anyhit: 64-instance world, closest (hit object) and occlusion (hit object)\n");
    const Mesh blob = makeBlob(1000, 500);
    Rays primary, bounce, shadow;
    cameraRays(primary, kGridEye, kGridAt, 55.0f, 1920, 1080);
    primary.upload();
    Hits hits(primary.n);
    Rng rng(5);
    {
        auto w = gridWorld(blob, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        Pipeline pl(PipeCfg{});
        trace(pl, w->ias.h, primary, hits, RG_HITOBJ, 0, 1.0f, false, 1);
        hits.download(primary.n);
        bounceRays(bounce, primary, hits, kGridLo, kGridHi, rng);
        shadowRays(shadow, primary, hits, kGridLight, 3.0f, kGridLo, kGridHi, rng);
        bounce.upload();
        shadow.upload();
    }
    const struct {
        const char *n;
        unsigned geom, inst;
        int onlyFiltered;
        const char *ah;
        float opacity;
        unsigned rayFlags;
    } variants[] = {
        {"geometry DISABLE_ANYHIT (no AH program)", OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT, 0, -1, nullptr, 1.0f, 0},
        {"AH empty (accept all)", OPTIX_GEOMETRY_FLAG_NONE, 0, -1, "__anyhit__nop", 1.0f, 0},
        {"AH opacity hash, opacity 1 (accept all)", OPTIX_GEOMETRY_FLAG_NONE, 0, -1, "__anyhit__opacity", 1.0f, 0},
        {"AH opacity hash, opacity 0.5", OPTIX_GEOMETRY_FLAG_NONE, 0, -1, "__anyhit__opacity", 0.5f, 0},
        {"AH opacity, REQUIRE_SINGLE_ANYHIT_CALL", OPTIX_GEOMETRY_FLAG_REQUIRE_SINGLE_ANYHIT_CALL, 0, -1, "__anyhit__opacity",
         1.0f, 0},
        {"AH opacity, instance flag DISABLE_ANYHIT", OPTIX_GEOMETRY_FLAG_NONE, OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT, -1,
         "__anyhit__opacity", 1.0f, 0},
        {"AH opacity, ray flag DISABLE_ANYHIT", OPTIX_GEOMETRY_FLAG_NONE, 0, -1, "__anyhit__opacity", 1.0f,
         OPTIX_RAY_FLAG_DISABLE_ANYHIT},
        {"AH opacity on 1 of 64 instances (others flag)", OPTIX_GEOMETRY_FLAG_NONE, OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT, 27,
         "__anyhit__opacity", 1.0f, 0},
    };
    for (const auto &v : variants) {
        auto w = gridWorld(blob, v.geom, v.inst, v.onlyFiltered);
        PipeCfg c;
        c.groups[0].ah = v.ah;
        Pipeline pl(c);
        std::printf("%s\n", v.n);
        printTrace("closest: primary", trace(pl, w->ias.h, primary, hits, RG_HITOBJ, v.rayFlags, v.opacity));
        printTrace("closest: bounce", trace(pl, w->ias.h, bounce, hits, RG_HITOBJ, v.rayFlags, v.opacity));
        printTrace("occlusion: shadow", trace(pl, w->ias.h, shadow, hits, RG_OCCL_HITOBJ, v.rayFlags, v.opacity));
        printTrace("occlusion: bounce (tmax inf)", trace(pl, w->ias.h, bounce, hits, RG_OCCL_HITOBJ, v.rayFlags, v.opacity));
    }
    // How often does any-hit run? (a counting variant of the opacity program)
    std::printf("any-hit invocations per ray (all 64 instances filtered):\n");
    Buf counter = dalloc(size_t(primary.n) * 4);
    std::vector<uint32_t> perRay;
    for (float opacity : {1.0f, 0.5f}) {
        for (unsigned geom : {unsigned(OPTIX_GEOMETRY_FLAG_NONE), unsigned(OPTIX_GEOMETRY_FLAG_REQUIRE_SINGLE_ANYHIT_CALL)}) {
            auto w = gridWorld(blob, geom);
            PipeCfg c;
            c.groups[0].ah = "__anyhit__opacity_count";
            Pipeline pl(c);
            const struct {
                const char *n;
                Rays *r;
                Raygen rg;
            } sets[] = {{"closest: primary", &primary, RG_HITOBJ}, {"closest: bounce", &bounce, RG_HITOBJ},
                        {"occlusion: shadow", &shadow, RG_OCCL_HITOBJ}, {"occlusion: bounce", &bounce, RG_OCCL_HITOBJ}};
            for (const auto &set : sets) {
                perRay.assign(set.r->n, 0u);
                h2d(counter.p, perRay.data(), perRay.size() * 4);
                OaParams p = makeParams(w->ias.h, *set.r, hits);
                p.opacity = opacity;
                p.counter = reinterpret_cast<unsigned *>(counter.p);
                pl.launch(p, set.rg, set.r->n);
                CU(cu.p_cuStreamSynchronize(g_stream)); // g_stream does not synchronize with the legacy stream
                d2h(perRay.data(), counter.p, perRay.size() * 4);
                uint64_t calls = 0, rays = 0;
                uint32_t most = 0;
                for (uint32_t c : perRay) {
                    calls += c;
                    rays += c > 0;
                    most = std::max(most, c);
                }
                std::printf("  opacity %.1f %-28s %-18s %.3f per ray, %.3f per ray with any, max %u\n", opacity,
                            geom ? "REQUIRE_SINGLE_ANYHIT_CALL" : "default", set.n, double(calls) / set.r->n,
                            rays ? double(calls) / rays : 0.0, most);
            }
        }
    }
    dfree(counter);

    // Shadow linking: only the 32 even blobs block the shadow rays, by
    // visibility mask (ray mask 0x01) or by an any-hit filter on instance id.
    std::printf("shadow linking (32 of 65 instances cast shadows), occlusion queries:\n");
    uint64_t evenMask = 0;
    for (int i = 0; i < 64; i += 2)
        evenMask |= 1ull << i;
    {
        auto w = gridWorld(blob, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT, 0, -1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, true);
        Pipeline pl(PipeCfg{});
        for (Rays *r : {&shadow, &bounce}) {
            OaParams p = makeParams(w->ias.h, *r, hits);
            printTrace(r == &shadow ? "no linking (mask 0xFF): shadow" : "no linking (mask 0xFF): bounce",
                       traceParams(pl, p, hits, RG_OCCL_HITOBJ, r->n));
            p.mask = 0x01;
            printTrace(r == &shadow ? "visibility mask 0x01: shadow" : "visibility mask 0x01: bounce",
                       traceParams(pl, p, hits, RG_OCCL_HITOBJ, r->n));
        }
    }
    {
        auto w = gridWorld(blob, OPTIX_GEOMETRY_FLAG_NONE);
        PipeCfg c;
        c.groups[0].ah = "__anyhit__shadowlink";
        Pipeline pl(c);
        for (Rays *r : {&shadow, &bounce}) {
            OaParams p = makeParams(w->ias.h, *r, hits);
            p.linkMask = evenMask;
            printTrace(r == &shadow ? "any-hit filter on instance id: shadow" : "any-hit filter on instance id: bounce",
                       traceParams(pl, p, hits, RG_OCCL_HITOBJ, r->n));
        }
    }
}

// --- launch overhead ----------------------------------------------------------

void testLaunch() {
    std::printf("== launch: overhead of small launches (64-instance world; per-launch averages)\n");
    const Mesh blob = makeBlob(1000, 500);
    auto w = gridWorld(blob, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
    Pipeline pl(PipeCfg{});
    Rays primary, bounce;
    cameraRays(primary, kGridEye, kGridAt, 55.0f, 1920, 1080);
    primary.upload();
    Hits hits(primary.n);
    Rng rng(9);
    trace(pl, w->ias.h, primary, hits, RG_HITOBJ, 0, 1.0f, false, 1);
    hits.download(primary.n);
    bounceRays(bounce, primary, hits, kGridLo, kGridHi, rng);
    // Shuffle the bounce rays so that prefixes are incoherent samples of the whole image.
    for (uint32_t i = bounce.n - 1; i > 0; --i) {
        const uint32_t j = rng.u32() % (i + 1);
        for (int k = 0; k < 3; ++k) {
            std::swap(bounce.org[3 * i + k], bounce.org[3 * j + k]);
            std::swap(bounce.dir[3 * i + k], bounce.dir[3 * j + k]);
        }
    }
    bounce.upload();

    const std::vector<char> ptx = readFile(g_dir + "oa_cuda_kernel.ptx");
    std::vector<char> ptxz(ptx);
    ptxz.push_back(0);
    CUmodule cmod;
    CUfunction kern;
    CU(cu.p_cuModuleLoadData(&cmod, ptxz.data()));
    CU(cu.p_cuModuleGetFunction(&kern, cmod, "oa_empty"));
    Buf countBuf = dalloc(4);

    // Optional (OA_GRAPH=1, or OA_GRAPH=fresh): optixLaunch captured into a
    // CUDA graph, with the probe's context or a fresh OptixDeviceContext. Both
    // fail on this machine (optix-accel.md 5.4): the capture succeeds and the
    // replay faults (CUDA_ERROR_ILLEGAL_ADDRESS). Earlier variants with more
    // launches failed inside the capture (CUDA 905, 907).
    char *graphEnv = nullptr;
    size_t graphEnvLen = 0;
    _dupenv_s(&graphEnv, &graphEnvLen, "OA_GRAPH");
    const std::string graphMode = graphEnv ? graphEnv : "";
    std::free(graphEnv);
    if (!graphMode.empty()) {
        const OptixDeviceContext main = g_ctx;
        if (graphMode == "fresh") {
            OptixDeviceContextOptions o{};
            o.logCallbackFunction = &logCallback;
            o.logCallbackLevel = 2;
            OX(optixDeviceContextCreate(g_cuctx, &o, &g_ctx));
        }
        {
            Pipeline gp(PipeCfg{});
            OaParams p = makeParams(w->ias.h, bounce, hits);
            h2d(gp.params.p, &p, sizeof(p));
            CU(cu.p_cuStreamSynchronize(g_stream));
            CU(cu.p_cuStreamBeginCapture(g_stream, CU_STREAM_CAPTURE_MODE_RELAXED));
            for (int i = 0; i < 16; ++i)
                gp.launch(p, RG_HITOBJ, 4096, false);
            CUgraph graph;
            CU(cu.p_cuStreamEndCapture(g_stream, &graph));
            CUgraphExec exec;
            CU(cu.p_cuGraphInstantiate(&exec, graph, 0));
            Events e;
            e.start();
            CU(cu.p_cuGraphLaunch(exec, g_stream));
            std::printf("graph of 16 launches of 4096 rays: %.2f us per launch\n", e.stop() * 1e3f / 16);
            CU(cu.p_cuGraphExecDestroy(exec));
            CU(cu.p_cuGraphDestroy(graph));
        }
        if (graphMode == "fresh")
            optixDeviceContextDestroy(g_ctx);
        g_ctx = main;
    }

    std::printf("%9s | %9s %9s %9s | %9s | %9s %9s | %10s %10s | %9s\n", "rays", "empty us", "+params", "submit us",
                "cuda us", "trace us", "Mrays/s", "lat. empty", "lat. trace", "capacity");
    const uint32_t sizes[] = {1, 32, 256, 1024, 4096, 16384, 65536, 262144, 1048576, 2073600};
    Events ev;
    for (uint32_t n : sizes) {
        OaParams p = makeParams(w->ias.h, bounce, hits);
        const int reps = n <= 65536 ? 400 : 40;
        for (int i = 0; i < 5; ++i)
            pl.launch(p, RG_EMPTY, n);
        // (1) back-to-back launches with constant params
        CU(cu.p_cuStreamSynchronize(g_stream));
        double h0 = now();
        ev.start();
        pl.launch(p, RG_EMPTY, n);
        for (int i = 1; i < reps; ++i)
            pl.launch(p, RG_EMPTY, n, false);
        const double submit = (now() - h0) * 1e3 / reps;
        const float emptyUs = ev.stop() * 1e3f / reps;
        // (2) params uploaded before every launch
        ev.start();
        for (int i = 0; i < reps; ++i)
            pl.launch(p, RG_EMPTY, n, true);
        const float paramsUs = ev.stop() * 1e3f / reps;
        // (3) a plain CUDA kernel with the same work
        unsigned width = n;
        CUdeviceptr out = hits.prim.p, cnt = 0;
        void *args[] = {&out, &cnt, &width};
        ev.start();
        for (int i = 0; i < reps; ++i)
            CU(cu.p_cuLaunchKernel(kern, (n + 255) / 256, 1, 1, 256, 1, 1, 0, g_stream, args, nullptr));
        const float cudaUs = ev.stop() * 1e3f / reps;
        // (4) tracing the first n (shuffled) bounce rays
        pl.launch(p, RG_HITOBJ, n);
        ev.start();
        pl.launch(p, RG_HITOBJ, n);
        for (int i = 1; i < reps; ++i)
            pl.launch(p, RG_HITOBJ, n, false);
        const float traceUs = ev.stop() * 1e3f / reps;
        // (5) latency: launch + synchronize, wall clock
        std::vector<float> le, lt;
        for (int i = 0; i < 30; ++i) {
            double t0 = now();
            pl.launch(p, RG_EMPTY, n, false);
            CU(cu.p_cuStreamSynchronize(g_stream));
            le.push_back(float((now() - t0) * 1e3));
            t0 = now();
            pl.launch(p, RG_HITOBJ, n, false);
            CU(cu.p_cuStreamSynchronize(g_stream));
            lt.push_back(float((now() - t0) * 1e3));
        }
        // (6) launched at the full capacity with the count on the device
        h2d(countBuf.p, &n, 4);
        OaParams pc = p;
        pc.count = reinterpret_cast<const unsigned *>(countBuf.p);
        pl.launch(pc, RG_HITOBJ, bounce.n);
        ev.start();
        pl.launch(pc, RG_HITOBJ, bounce.n);
        for (int i = 1; i < 20; ++i)
            pl.launch(pc, RG_HITOBJ, bounce.n, false);
        const float capUs = ev.stop() * 1e3f / 20;
        std::printf("%9u | %9.2f %9.2f %9.2f | %9.2f | %9.2f %9.1f | %10.1f %10.1f | %9.1f\n", n, emptyUs, paramsUs, submit,
                    cudaUs, traceUs, n / traceUs, median(le), median(lt), capUs);
    }
    dfree(countBuf);
}

// --- curves -----------------------------------------------------------------

struct CurveType {
    const char *n;
    OptixPrimitiveType type;
    int degree; // 1 linear, 2 quadratic, 3 cubic; 0 = Bezier (3 per segment + 1)
    unsigned endcap;
};
const CurveType kCurveTypes[] = {
    {"ROUND_LINEAR", OPTIX_PRIMITIVE_TYPE_ROUND_LINEAR, 1, 0},
    {"ROUND_QUADRATIC_BSPLINE", OPTIX_PRIMITIVE_TYPE_ROUND_QUADRATIC_BSPLINE, 2, 0},
    {"ROUND_CUBIC_BSPLINE", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE, 3, 0},
    {"ROUND_CUBIC_BSPLINE + ENDCAP_ON", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE, 3, OPTIX_CURVE_ENDCAP_ON},
    {"ROUND_CATMULLROM", OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM, 3, 0},
    {"ROUND_CUBIC_BEZIER", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BEZIER, 0, 0},
    {"ROUND_QUADRATIC_BSPLINE_ROCAPS", OPTIX_PRIMITIVE_TYPE_ROUND_QUADRATIC_BSPLINE_ROCAPS, 2, 0},
    {"ROUND_CUBIC_BSPLINE_ROCAPS", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE_ROCAPS, 3, 0},
    {"ROUND_CATMULLROM_ROCAPS", OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM_ROCAPS, 3, 0},
    {"ROUND_CUBIC_BEZIER_ROCAPS", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BEZIER_ROCAPS, 0, 0},
    {"FLAT_QUADRATIC_BSPLINE (ribbon)", OPTIX_PRIMITIVE_TYPE_FLAT_QUADRATIC_BSPLINE, 2, 0},
};

struct HairSet {
    std::vector<float> cp, w; // control points (xyz), radii
    std::vector<uint32_t> idx;
};

// nc curves of `segs` segments standing on [0, side]^2, control points sampled
// from a smooth strand so that every basis covers about the same strand.
HairSet makeHair(const CurveType &ct, uint32_t nc, uint32_t segs, float side, float len, float r0, float r1, uint64_t seed) {
    HairSet h;
    Rng rng(seed);
    const uint32_t grid = uint32_t(std::ceil(std::sqrt(double(nc))));
    const uint32_t ncp = ct.degree == 0 ? 3 * segs + 1 : segs + uint32_t(ct.degree);
    h.cp.reserve(size_t(nc) * ncp * 3);
    for (uint32_t c = 0; c < nc; ++c) {
        const float rx = side * ((c % grid) + rng.f()) / grid, rz = side * ((c / grid) + rng.f()) / grid;
        const V3 lean{0.3f * (2 * rng.f() - 1), 1.0f, 0.3f * (2 * rng.f() - 1)};
        const float ph = 6.28f * rng.f(), fr = 1.0f + 2.0f * rng.f();
        auto strand = [&](float s) {
            return V3{rx + len * (lean.x * s + 0.05f * std::sin(6.28f * fr * s + ph)), len * s,
                      rz + len * (lean.z * s + 0.05f * std::cos(6.28f * fr * s + ph))};
        };
        const uint32_t base = uint32_t(h.cp.size() / 3);
        for (uint32_t k = 0; k < ncp; ++k) {
            float s;
            if (ct.degree == 0)
                s = float(k) / float(3 * segs);
            else if (ct.degree == 1)
                s = float(k) / float(segs);
            else if (ct.degree == 2)
                s = (float(k) - 0.5f) / float(segs);
            else
                s = (float(k) - 1.0f) / float(segs);
            const V3 p = strand(s);
            h.cp.insert(h.cp.end(), {p.x, p.y, p.z});
            const float sc = std::clamp(s, 0.0f, 1.0f);
            h.w.push_back(r0 + (r1 - r0) * sc);
        }
        for (uint32_t j = 0; j < segs; ++j)
            h.idx.push_back(base + (ct.degree == 0 ? 3 * j : j));
    }
    return h;
}

void testCurves() {
    const uint32_t nc = 100000, segs = 8;
    std::printf("== curves: %u curves x %u segments (%u primitives) on a 10 x 10 patch, length 1, radius 0.005 -> 0.002\n",
                nc, segs, nc * segs);
    Rays primary, bounce;
    cameraRays(primary, {5.0f, 1.6f, -3.0f}, {5.0f, 0.3f, 4.0f}, 50.0f, 1920, 1080);
    primary.upload();
    Hits hits(primary.n);
    for (const CurveType &ct : kCurveTypes) {
        const HairSet h = makeHair(ct, nc, segs, 10.0f, 1.0f, 0.005f, 0.002f, 21);
        InputP in = curves(ct.type, {&h.cp}, {&h.w}, h.idx, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT, ct.endcap);
        Accel fb = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_BUILD, 1, true, 3);
        Accel a = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, true, 3);
        std::printf("%s: %zu control points (%.1f MB input)\n", ct.n, h.cp.size() / 3, mb((h.cp.size() + h.w.size() + h.idx.size()) * 4));
        std::printf("  FAST_TRACE build %7.2f ms  out %6.1f MB  compacted %6.1f MB (%.0f B/segment)  temp %6.1f MB\n", a.ms,
                    mb(a.out), mb(a.compacted), double(a.compacted) / (nc * segs), mb(a.temp));
        std::printf("  FAST_BUILD build %7.2f ms  out %6.1f MB  compacted %6.1f MB (%.0f B/segment)\n", fb.ms, mb(fb.out),
                    mb(fb.compacted), double(fb.compacted) / (nc * segs));
        for (int v = 0; v < 2; ++v) {
            Accel &g = v == 0 ? a : fb;
            PipeCfg c;
            c.groups[0].type = ct.type;
            c.groups[0].endcap = ct.endcap;
            c.groups[0].buildFlags = (v == 0 ? OPTIX_BUILD_FLAG_PREFER_FAST_TRACE : OPTIX_BUILD_FLAG_PREFER_FAST_BUILD) |
                                     OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
            Pipeline pl(c);
            Buf ib;
            std::vector<OptixInstance> inst{makeInstance(Xf{}, 0, g.h)};
            Accel ias = buildIas(inst, ib);
            const char *tag = v == 0 ? "FAST_TRACE" : "FAST_BUILD";
            char l[96];
            std::snprintf(l, sizeof(l), "%s closest: primary 1080p", tag);
            printTrace(l, trace(pl, ias.h, primary, hits, RG_HITOBJ));
            if (v == 0) {
                hits.download(primary.n);
                Rng rng(13);
                bounceRays(bounce, primary, hits, {0, 0, 0}, {10, 1, 10}, rng, true);
                bounce.upload();
            }
            std::snprintf(l, sizeof(l), "%s closest: bounce (sphere dirs)", tag);
            printTrace(l, trace(pl, ias.h, bounce, hits, RG_HITOBJ));
            std::snprintf(l, sizeof(l), "%s occlusion: bounce", tag);
            printTrace(l, trace(pl, ias.h, bounce, hits, RG_OCCL_HITOBJ));
            ias.release();
            dfree(ib);
        }
        fb.release();
        a.release();
    }
}

// --- points: built-in spheres vs custom primitives ---------------------------

void testPoints() {
    const uint32_t np = 1000000;
    std::printf("== points: %u points in [0,10]^3, radius 0.01-0.03\n", np);
    Rng rng(17);
    std::vector<float> c(size_t(np) * 3), r(np), sph(size_t(np) * 4), nrm(size_t(np) * 3);
    std::vector<OptixAabb> boxes(np);
    for (uint32_t i = 0; i < np; ++i) {
        const V3 p{10 * rng.f(), 10 * rng.f(), 10 * rng.f()};
        const float rad = 0.01f + 0.02f * rng.f();
        st3(c, i, p);
        r[i] = rad;
        sph[4 * i] = p.x;
        sph[4 * i + 1] = p.y;
        sph[4 * i + 2] = p.z;
        sph[4 * i + 3] = rad;
        st3(nrm, i, rng.sphere());
        boxes[i] = {p.x - rad, p.y - rad, p.z - rad, p.x + rad, p.y + rad, p.z + rad};
    }
    Buf dSph = dupload(sph), dNrm = dupload(nrm);
    Rays primary, bounce;
    cameraRays(primary, {5, 5, -6}, {5, 5, 5}, 50.0f, 1920, 1080);
    primary.upload();
    Hits hits(primary.n);
    const struct {
        const char *n;
        OptixPrimitiveType type;
        const char *is;
        bool oriented;
    } variants[] = {{"built-in SPHERE", OPTIX_PRIMITIVE_TYPE_SPHERE, nullptr, false},
                    {"custom sphere (AABB + IS)", OPTIX_PRIMITIVE_TYPE_CUSTOM, "__intersection__sphere", false},
                    {"custom ray-facing disc", OPTIX_PRIMITIVE_TYPE_CUSTOM, "__intersection__disc", false},
                    {"custom oriented disc", OPTIX_PRIMITIVE_TYPE_CUSTOM, "__intersection__disc", true}};
    for (const auto &v : variants) {
        InputP in = v.type == OPTIX_PRIMITIVE_TYPE_SPHERE ? spheres(c, r, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT)
                                                          : aabbs(boxes, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        Accel a = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, true, 3);
        std::printf("%s: build %.2f ms, out %.1f MB, compacted %.1f MB (%.0f B/point)\n", v.n, a.ms, mb(a.out), mb(a.compacted),
                    double(a.compacted) / np);
        PipeCfg cfg;
        cfg.groups[0].type = v.type;
        cfg.groups[0].is = v.is;
        cfg.groups[0].data = {reinterpret_cast<const float *>(dSph.p), v.oriented ? reinterpret_cast<const float *>(dNrm.p) : nullptr};
        Pipeline pl(cfg);
        Buf ib;
        std::vector<OptixInstance> inst{makeInstance(Xf{}, 0, a.h)};
        Accel ias = buildIas(inst, ib);
        printTrace("closest, hit object: primary 1080p", trace(pl, ias.h, primary, hits, RG_HITOBJ));
        if (bounce.n == 0) {
            hits.download(primary.n);
            Rng rb(19);
            bounceRays(bounce, primary, hits, {0, 0, 0}, {10, 10, 10}, rb, true);
            bounce.upload();
        }
        printTrace("closest, hit object: bounce (sphere dirs)", trace(pl, ias.h, bounce, hits, RG_HITOBJ));
        printTrace("occlusion, hit object: bounce", trace(pl, ias.h, bounce, hits, RG_OCCL_HITOBJ));
        ias.release();
        a.release();
        dfree(ib);
    }
    dfree(dSph);
    dfree(dNrm);
}

// --- hit attributes and semantics --------------------------------------------

struct Probe {
    V3 o, d;
    float time = 0.0f;
    const char *what = "";
};

// Traces single rays through `h` and returns the inspect records.
std::vector<std::array<float, kInspectStride>> inspect(const PipeCfg &cfg, OptixTraversableHandle h, const std::vector<Probe> &probes,
                                                       float tmin = 0.0f) {
    Pipeline pl(cfg);
    Rays r(uint32_t(probes.size()));
    for (uint32_t i = 0; i < r.n; ++i) {
        r.set(i, probes[i].o, probes[i].d);
        r.time[i] = probes[i].time;
    }
    r.upload();
    Hits hh(r.n, true);
    OaParams p = makeParams(h, r, hh, true);
    p.tmin = tmin;
    pl.launch(p, RG_INSPECT, r.n);
    CU(cu.p_cuStreamSynchronize(g_stream));
    hh.download(r.n);
    std::vector<std::array<float, kInspectStride>> out(r.n);
    for (uint32_t i = 0; i < r.n; ++i)
        std::memcpy(out[i].data(), &hh.hinsp[size_t(i) * kInspectStride], sizeof(float) * kInspectStride);
    return out;
}

void printInspect(const Probe &p, const std::array<float, kInspectStride> &o) {
    if (o[0] == 0.0f) {
        std::printf("    %-46s miss\n", p.what);
        return;
    }
    std::printf("    %-46s t %.6f prim %.0f u %.6f v %.6f front %.0f kind 0x%02x", p.what, o[1], o[2], o[8], o[9], o[7],
                unsigned(o[5]));
    if (o[10] != 0.0f || o[11] != 0.0f || o[12] != 0.0f)
        std::printf(" ribbonN (%.3f %.3f %.3f)", o[10], o[11], o[12]);
    std::printf("\n");
}

// Uniform cubic/quadratic bases as Embree evaluates them (core/geometry/curves.h).
float evalBasis(OptixPrimitiveType t, const float *p, float u) {
    switch (t) {
    case OPTIX_PRIMITIVE_TYPE_ROUND_LINEAR: return (1 - u) * p[0] + u * p[1];
    case OPTIX_PRIMITIVE_TYPE_ROUND_QUADRATIC_BSPLINE:
    case OPTIX_PRIMITIVE_TYPE_ROUND_QUADRATIC_BSPLINE_ROCAPS:
    case OPTIX_PRIMITIVE_TYPE_FLAT_QUADRATIC_BSPLINE:
        return 0.5f * (1 - u) * (1 - u) * p[0] + (0.5f + u - u * u) * p[1] + 0.5f * u * u * p[2];
    case OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE:
    case OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE_ROCAPS:
        return ((1 - u) * (1 - u) * (1 - u) * p[0] + (3 * u * u * u - 6 * u * u + 4) * p[1] +
                (-3 * u * u * u + 3 * u * u + 3 * u + 1) * p[2] + u * u * u * p[3]) /
               6.0f;
    case OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM:
    case OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM_ROCAPS:
        return 0.5f * ((-u * u * u + 2 * u * u - u) * p[0] + (3 * u * u * u - 5 * u * u + 2) * p[1] +
                       (-3 * u * u * u + 4 * u * u + u) * p[2] + (u * u * u - u * u) * p[3]);
    default: // Bezier
        return (1 - u) * (1 - u) * (1 - u) * p[0] + 3 * u * (1 - u) * (1 - u) * p[1] + 3 * u * u * (1 - u) * p[2] + u * u * u * p[3];
    }
}

// One curve segment along x with control points xs (y = z = 0) and constant
// radius r, traced directly as a GAS.
struct SingleCurve {
    InputP in;
    Accel a;
    std::vector<float> cp, w;
    std::vector<uint32_t> idx;
    SingleCurve(OptixPrimitiveType t, const std::vector<float> &xs, float r, unsigned endcap, uint32_t stride = 1,
                uint32_t segments = 1) {
        for (float x : xs)
            cp.insert(cp.end(), {x, 0.0f, 0.0f});
        w.assign(xs.size(), r);
        for (uint32_t s = 0; s < segments; ++s)
            idx.push_back(s * stride);
        in = curves(t, {&cp}, {&w}, idx, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT, endcap);
        a = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_COMPACTION);
    }
    ~SingleCurve() { a.release(); }
};

PipeCfg gasPipe(OptixPrimitiveType t, unsigned endcap = 0) {
    PipeCfg c;
    c.graph = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    c.depth = 1;
    c.groups[0].type = t;
    c.groups[0].endcap = endcap;
    return c;
}

// The rotation angle about z (degrees) at which a thin target at x = 1,
// rotated about the origin, is hit at `time`: rays from the origin scanned in
// 0.01 degree steps.
double scanAngle(const PipeCfg &cfg, OptixTraversableHandle h, float time) {
    std::vector<Probe> probes;
    for (int k = 0; k <= 18000; ++k) {
        const float a = float(k) * 0.01f * kPi / 180.0f;
        probes.push_back({{0, 0, 0}, {std::cos(a), std::sin(a), 0}, time});
    }
    const auto res = inspect(cfg, h, probes);
    double sum = 0;
    int n = 0;
    for (int k = 0; k <= 18000; ++k)
        if (res[k][0] != 0.0f) {
            sum += k * 0.01;
            ++n;
        }
    return n ? sum / n : -1.0;
}

double slerpAngle(double theta, double t) { return theta * t; }
double nlerpAngle(double theta, double t) { // degrees
    const double h = 0.5 * theta * kPi / 180.0;
    const double w = (1 - t) + t * std::cos(h), z = t * std::sin(h);
    return 2.0 * std::atan2(z, w) * 180.0 / kPi;
}
double matrixAngle(double theta, double t) {
    const double a = theta * kPi / 180.0;
    return std::atan2(t * std::sin(a), (1 - t) + t * std::cos(a)) * 180.0 / kPi;
}

void testAttributes() {
    std::printf("== attrs: hit attributes and semantics (single rays, hit objects)\n");
    // Triangle
    {
        const Mesh m{{0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 1, 2}};
        InputP in = triangles({&m.P}, m.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        Accel a = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE);
        std::vector<Probe> pr{{{0.2f, 0.3f, 1}, {0, 0, -1}, 0, "triangle p0=0 p1=x p2=y, at (0.2,0.3) from +z"},
                              {{0.2f, 0.3f, -1}, {0, 0, 1}, 0, "same point from -z (back face)"}};
        std::printf("  triangle (expect u = 0.2 (weight of p1), v = 0.3; Embree's convention)\n");
        const auto r = inspect(gasPipe(OPTIX_PRIMITIVE_TYPE_TRIANGLE), a.h, pr);
        for (size_t i = 0; i < pr.size(); ++i)
            printInspect(pr[i], r[i]);
        a.release();
    }
    // Round linear: two segments along x, radius 0.1.
    {
        SingleCurve c(OPTIX_PRIMITIVE_TYPE_ROUND_LINEAR, {0, 1, 2}, 0.1f, 0, 1, 2);
        std::vector<Probe> pr{
            {{0.3f, 1, 0}, {0, -1, 0}, 0, "side hit at x=0.3 (expect t 0.9, u 0.3)"},
            {{1.0f, 1, 0}, {0, -1, 0}, 0, "side hit at the joint x=1"},
            {{3.0f, 0, 0}, {-1, 0, 0}, 0, "along the axis onto the end (x=2.1?)"},
            {{2.05f, 1, 0}, {0, -1, 0}, 0, "onto the end cap at x=2.05"},
            {{-1.0f, 0.05f, 0}, {1, 0, 0}, 0, "onto the start cap, 0.05 off axis"},
            {{-0.05f, 1, 0}, {0, -1, 0}, 0, "onto the start cap at x=-0.05, from the side"},
            {{-1.0f, 0.3f, 0.2f}, normalize({0.95f, -0.25f, -0.2f}), 0, "obliquely onto the start cap"},
            {{3.0f, 0.3f, 0.2f}, normalize({-0.95f, -0.25f, -0.2f}), 0, "obliquely onto the end cap"},
            {{0.5f, 0, 0}, {0, 1, 0}, 0, "from inside (axis x=0.5) outwards"},
            {{0.5f, 0.05f, 0}, {1, 0, 0}, 0, "from inside along the axis"},
            {{0.5f, 0.0999f, 0}, {0, 1, 0}, 0, "from just inside the surface, outwards"},
        };
        std::printf("  ROUND_LINEAR, points x = 0, 1, 2, radius 0.1\n");
        const auto r = inspect(gasPipe(OPTIX_PRIMITIVE_TYPE_ROUND_LINEAR), c.a.h, pr);
        for (size_t i = 0; i < pr.size(); ++i)
            printInspect(pr[i], r[i]);
    }
    // Non-linear bases: is u the basis parameter? Caps, inside hits.
    const struct {
        const char *n;
        OptixPrimitiveType t;
        std::vector<float> xs;
        unsigned endcap;
    } bases[] = {
        {"ROUND_QUADRATIC_BSPLINE", OPTIX_PRIMITIVE_TYPE_ROUND_QUADRATIC_BSPLINE, {-1, 0.5f, 3}, 0},
        {"ROUND_CUBIC_BSPLINE", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE, {-1, 0, 2, 3}, 0},
        {"ROUND_CUBIC_BSPLINE + ENDCAP_ON", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE, {-1, 0, 2, 3}, OPTIX_CURVE_ENDCAP_ON},
        {"ROUND_CATMULLROM", OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM, {-1, 0, 2, 3}, 0},
        {"ROUND_CUBIC_BEZIER", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BEZIER, {0, 0.2f, 1.5f, 2}, 0},
        {"ROUND_QUADRATIC_BSPLINE_ROCAPS", OPTIX_PRIMITIVE_TYPE_ROUND_QUADRATIC_BSPLINE_ROCAPS, {-1, 0.5f, 3}, 0},
        {"ROUND_CUBIC_BSPLINE_ROCAPS", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BSPLINE_ROCAPS, {-1, 0, 2, 3}, 0},
        {"ROUND_CATMULLROM_ROCAPS", OPTIX_PRIMITIVE_TYPE_ROUND_CATMULLROM_ROCAPS, {-1, 0, 2, 3}, 0},
        {"ROUND_CUBIC_BEZIER_ROCAPS", OPTIX_PRIMITIVE_TYPE_ROUND_CUBIC_BEZIER_ROCAPS, {0, 0.2f, 1.5f, 2}, 0},
    };
    for (const auto &b : bases) {
        SingleCurve c(b.t, b.xs, 0.1f, b.endcap);
        const float x0 = evalBasis(b.t, b.xs.data(), 0.0f), x1 = evalBasis(b.t, b.xs.data(), 1.0f);
        std::vector<Probe> pr;
        const float us[] = {0.1f, 0.25f, 0.5f, 0.8f};
        std::vector<std::string> labels;
        for (float u : us) {
            labels.push_back("side hit at x(u=" + std::to_string(u).substr(0, 4) + ")");
            pr.push_back({{evalBasis(b.t, b.xs.data(), u), 1, 0}, {0, -1, 0}});
        }
        pr.push_back({{x1 + 1.0f, 0, 0}, {-1, 0, 0}, 0, "along the axis onto the end"});
        pr.push_back({{x0 - 1.0f, 0.05f, 0}, {1, 0, 0}, 0, "onto the start, 0.05 off axis"});
        pr.push_back({{x1 + 0.05f, 1, 0}, {0, -1, 0}, 0, "0.05 beyond the end, from the side"});
        pr.push_back({{x1 + 1.0f, 0.23f, 0.1f}, normalize({-1.0f, -0.2f, -0.1f}), 0, "obliquely onto the end face"});
        pr.push_back({{x0 - 1.0f, 0.23f, 0.1f}, normalize({1.0f, -0.2f, -0.1f}), 0, "obliquely onto the start face"});
        pr.push_back({{0.5f * (x0 + x1), 0, 0}, {0, 1, 0}, 0, "from inside outwards"});
        pr.push_back({{0.5f * (x0 + x1), 0.0999f, 0}, {0, 1, 0}, 0, "from just inside the surface"});
        for (size_t i = 0; i < labels.size(); ++i)
            pr[i].what = labels[i].c_str();
        std::printf("  %s, control x = ", b.n);
        for (float x : b.xs)
            std::printf("%g ", x);
        std::printf("(segment x %.4f .. %.4f), radius 0.1\n", x0, x1);
        const auto r = inspect(gasPipe(b.t, b.endcap), c.a.h, pr);
        for (size_t i = 0; i < pr.size(); ++i) {
            printInspect(pr[i], r[i]);
            if (i < 4 && r[i][0] != 0.0f)
                std::printf("      expected u %.4f; x(u reported) - x(ray) = %+.2e\n", us[i],
                            evalBasis(b.t, b.xs.data(), r[i][8]) - pr[i].o.x);
        }
    }
    // Ribbons: which way does a FLAT_QUADRATIC_BSPLINE face without normals?
    {
        const struct {
            const char *n;
            std::vector<float> p;
        } rib[] = {{"straight ribbon along x", {0, 0, 0, 1, 0, 0, 2, 0, 0}},
                   {"ribbon bent in the xy plane", {0, 0, 0, 1, 1, 0, 2, 0, 0}},
                   {"ribbon bent in the xz plane", {0, 0, 0, 1, 0, 1, 2, 0, 0}}};
        for (const auto &r : rib) {
            std::vector<float> w(3, 0.1f);
            std::vector<uint32_t> idx{0};
            InputP in = curves(OPTIX_PRIMITIVE_TYPE_FLAT_QUADRATIC_BSPLINE, {&r.p}, {&w}, idx, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
            Accel a = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_COMPACTION);
            const float px[3] = {r.p[0], r.p[3], r.p[6]}, py[3] = {r.p[1], r.p[4], r.p[7]}, pz[3] = {r.p[2], r.p[5], r.p[8]};
            const OptixPrimitiveType ft = OPTIX_PRIMITIVE_TYPE_FLAT_QUADRATIC_BSPLINE;
            const V3 mid{evalBasis(ft, px, 0.5f), evalBasis(ft, py, 0.5f), evalBasis(ft, pz, 0.5f)};
            std::vector<Probe> pr{{mid + V3{0, 2, 0}, {0, -1, 0}, 0, "from +y"},
                                  {mid + V3{0, 0, 2}, {0, 0, -1}, 0, "from +z"},
                                  {mid + V3{0, -2, 0}, {0, 1, 0}, 0, "from -y"},
                                  {mid + normalize({0, 1, 1}) * 2.0f, normalize({0, -1, -1}), 0, "from +y+z"},
                                  {mid + normalize({0.3f, 0.2f, 1}) * 2.0f, normalize({-0.3f, -0.2f, -1}), 0, "oblique"}};
            std::printf("  FLAT_QUADRATIC_BSPLINE: %s, width (radius) 0.1, rays at the segment middle (%.2f %.2f %.2f)\n", r.n,
                        mid.x, mid.y, mid.z);
            const auto o = inspect(gasPipe(OPTIX_PRIMITIVE_TYPE_FLAT_QUADRATIC_BSPLINE), a.h, pr);
            for (size_t i = 0; i < pr.size(); ++i)
                printInspect(pr[i], o[i]);
            a.release();
        }
    }
    // Spheres
    {
        std::vector<float> c{0, 0, 0}, rr{1};
        InputP in = spheres(c, rr, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        Accel a = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE);
        std::vector<Probe> pr{{{0, 0, 5}, {0, 0, -1}, 0, "from outside (expect t 4)"},
                              {{0, 0, 0}, {0, 0, 1}, 0, "from the centre (inside)"},
                              {{0, 0, 0.999f}, {0, 0, 1}, 0, "from just inside the surface"},
                              {{0, 0, 1.0f}, {0, 0, 1}, 0, "from the surface point, outwards"}};
        std::printf("  built-in SPHERE, centre 0, radius 1\n");
        const auto r = inspect(gasPipe(OPTIX_PRIMITIVE_TYPE_SPHERE), a.h, pr);
        for (size_t i = 0; i < pr.size(); ++i)
            printInspect(pr[i], r[i]);
        a.release();
    }
    // Motion transforms: how are SRT keys interpolated?
    {
        const Mesh target{{1, -0.005f, -0.5f, 1, 0.005f, -0.5f, 1, 0.005f, 0.5f, 1, -0.005f, 0.5f}, {0, 1, 2, 0, 2, 3}};
        InputP in = triangles({&target.P}, target.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        Accel gas = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE);
        PipeCfg cfg;
        cfg.motion = 1;
        cfg.graph = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY;
        cfg.depth = 3;
        std::printf("  instance motion, rotation about z between two keys (degrees at time t; scan step 0.01)\n");
        std::printf("    %-8s %-6s %10s %10s %10s %10s\n", "type", "t", "measured", "slerp", "nlerp", "matrix lerp");
        for (double theta : {90.0, 170.0}) {
            for (int kind = 0; kind < 2; ++kind) {
                std::vector<char> host;
                OptixTraversableType tt;
                if (kind == 0) {
                    host.assign(sizeof(OptixSRTMotionTransform), 0);
                    auto *m = reinterpret_cast<OptixSRTMotionTransform *>(host.data());
                    m->child = gas.h;
                    m->motionOptions = {2, 0, 0.0f, 1.0f};
                    for (int k = 0; k < 2; ++k) {
                        OptixSRTData &s = m->srtData[k];
                        s = OptixSRTData{};
                        s.sx = s.sy = s.sz = 1.0f;
                        const double h = 0.5 * theta * k * kPi / 180.0;
                        s.qz = float(std::sin(h));
                        s.qw = float(std::cos(h));
                    }
                    tt = OPTIX_TRAVERSABLE_TYPE_SRT_MOTION_TRANSFORM;
                } else {
                    host.assign(sizeof(OptixMatrixMotionTransform), 0);
                    auto *m = reinterpret_cast<OptixMatrixMotionTransform *>(host.data());
                    m->child = gas.h;
                    m->motionOptions = {2, 0, 0.0f, 1.0f};
                    for (int k = 0; k < 2; ++k) {
                        const Xf x = makeXf({0, 0, 1}, float(theta * k * kPi / 180.0), 1.0f, {});
                        std::memcpy(m->transform[k], x.m, sizeof(x.m));
                    }
                    tt = OPTIX_TRAVERSABLE_TYPE_MATRIX_MOTION_TRANSFORM;
                }
                Buf dm = dupload(host);
                OptixTraversableHandle th;
                OX(optixConvertPointerToTraversableHandle(g_ctx, dm.p, tt, &th));
                Buf ib;
                std::vector<OptixInstance> inst{makeInstance(Xf{}, 0, th)};
                Accel ias = buildIas(inst, ib);
                for (float t : {0.25f, 0.5f, 0.75f}) {
                    const double meas = scanAngle(cfg, ias.h, t);
                    std::printf("    %-8s %-6.2f %10.2f %10.2f %10.2f %10.2f   (keys 0 and %.0f degrees)\n",
                                kind == 0 ? "SRT" : "matrix", t, meas, slerpAngle(theta, t), nlerpAngle(theta, t),
                                matrixAngle(theta, t), theta);
                }
                ias.release();
                dfree(ib);
                dfree(dm);
            }
        }
        std::printf("    largest slerp - nlerp difference over t for a key interval of theta degrees:\n");
        for (double theta : {2.0, 5.0, 10.0, 30.0, 60.0, 90.0, 170.0}) {
            double worst = 0;
            for (int k = 0; k <= 1000; ++k) {
                const double t = k / 1000.0;
                worst = std::max(worst, std::fabs(slerpAngle(theta, t) - nlerpAngle(theta, t)));
            }
            std::printf("      theta %6.1f: %.5f degrees (%.2e of the radius at the arc)\n", theta, worst, worst * kPi / 180.0);
        }
        gas.release();
    }
}

// --- motion -----------------------------------------------------------------

void testMotion() {
    std::printf("== motion\n");
    Rng rng(23);
    // (a) deformation motion: the 1M blob with K vertex keys.
    {
        std::printf("deformation (vertex) motion of the 1M blob; IAS of one instance; rays with random times\n");
        Rays primary, bounce;
        cameraRays(primary, {0.3f, 0.4f, -3.2f}, {0, 0, 0}, 45.0f, 1920, 1080, &rng);
        primary.upload();
        Hits hits(primary.n);
        PipeCfg cfg;
        cfg.motion = 1;
        Pipeline pl(cfg);
        for (unsigned keys : {1u, 2u, 4u, 8u, 16u}) {
            std::vector<Mesh> m;
            for (unsigned k = 0; k < keys; ++k) {
                const float s = keys > 1 ? float(k) / float(keys - 1) : 0.0f;
                m.push_back(makeBlob(1000, 500, 0.3f * s, 0.02f * s, {0.3f * s, 0, 0}));
            }
            std::vector<const std::vector<float> *> kp;
            for (const Mesh &x : m)
                kp.push_back(&x.P);
            InputP in = triangles(kp, m[0].I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
            Accel a = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, (unsigned short)keys, true, 3);
            std::printf("  %2u keys: build %7.2f ms, out %6.1f MB, compacted %6.1f MB, temp %6.1f MB\n", keys, a.ms, mb(a.out),
                        mb(a.compacted), mb(a.temp));
            Buf ib;
            std::vector<OptixInstance> inst{makeInstance(Xf{}, 0, a.h)};
            for (unsigned short iasKeys : {(unsigned short)1, (unsigned short)2}) {
                if (keys == 1 && iasKeys == 2)
                    continue;
                Accel ias = buildIas(inst, ib, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, iasKeys);
                char l[96];
                std::snprintf(l, sizeof(l), "IAS keys %u: primary", iasKeys);
                printTrace(l, trace(pl, ias.h, primary, hits, RG_HITOBJ, 0, 1.0f, true));
                if (bounce.n == 0) {
                    hits.download(primary.n);
                    bounceRays(bounce, primary, hits, {-1.5f, -1.5f, -1.5f}, {1.8f, 1.5f, 1.5f}, rng);
                    bounce.upload();
                }
                std::snprintf(l, sizeof(l), "IAS keys %u: bounce", iasKeys);
                printTrace(l, trace(pl, ias.h, bounce, hits, RG_HITOBJ, 0, 1.0f, true));
                ias.release();
            }
            a.release();
            dfree(ib);
        }
    }
    // (b) instance motion: the 64-instance world with motion transforms.
    {
        std::printf("instance motion in the 64-instance world (rotation 20 degrees + translation 0.6 over the shutter)\n");
        const Mesh blob = makeBlob(1000, 500);
        auto w = gridWorld(blob, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        Rays primary, bounce;
        cameraRays(primary, kGridEye, kGridAt, 55.0f, 1920, 1080, &rng);
        primary.upload();
        Hits hits(primary.n);
        PipeCfg cfg;
        cfg.motion = 1;
        cfg.graph = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY;
        cfg.depth = 3;
        Pipeline pl(cfg);
        {
            Pipeline base(PipeCfg{});
            trace(base, w->ias.h, primary, hits, RG_HITOBJ, 0, 1.0f, false, 1);
            hits.download(primary.n);
            bounceRays(bounce, primary, hits, kGridLo, kGridHi, rng);
            bounce.upload();
            std::printf("  static instances, baseline pipeline (SINGLE_LEVEL, no motion)\n");
            printTrace("primary", trace(base, w->ias.h, primary, hits, RG_HITOBJ));
            printTrace("bounce", trace(base, w->ias.h, bounce, hits, RG_HITOBJ));
        }
        std::printf("  static instances, motion pipeline (ALLOW_ANY, depth 3)\n");
        printTrace("primary", trace(pl, w->ias.h, primary, hits, RG_HITOBJ, 0, 1.0f, true));
        printTrace("bounce", trace(pl, w->ias.h, bounce, hits, RG_HITOBJ, 0, 1.0f, true));
        const struct {
            const char *n;
            bool srt;
            unsigned keys;
            unsigned short iasKeys;
            float spin = 0.35f; // rotation over the shutter (radians)
        } variants[] = {{"SRT motion, 2 keys, static IAS", true, 2, 1},   {"SRT motion, 2 keys, IAS 2 keys", true, 2, 2},
                        {"SRT motion, 16 keys, static IAS", true, 16, 1}, {"SRT motion, 16 keys, IAS 16 keys", true, 16, 16},
                        {"SRT motion, 2 keys, static IAS, spin 1 deg", true, 2, 1, 0.0175f},
                        {"SRT motion, 2 keys, static IAS, spin 0", true, 2, 1, 0.0f},
                        {"matrix motion, 2 keys, static IAS", false, 2, 1}, {"matrix motion, 2 keys, IAS 2 keys", false, 2, 2}};
        for (const auto &v : variants) {
            const size_t bytes = v.srt ? sizeof(OptixSRTMotionTransform) + (v.keys - 2) * sizeof(OptixSRTData)
                                       : sizeof(OptixMatrixMotionTransform) + (v.keys - 2) * 12 * sizeof(float);
            const size_t stride = (bytes + 63) / 64 * 64;
            std::vector<char> host(stride * 64, 0);
            for (int i = 0; i < 64; ++i) {
                const OptixInstance &si = w->inst[i];
                // The instance's static transform as rotation (axis, angle) is not
                // kept; rebuild a similar placement: translation only + spin.
                const V3 base{si.transform[3], si.transform[7], si.transform[11]};
                char *p = host.data() + stride * i;
                for (unsigned k = 0; k < v.keys; ++k) {
                    const float s = float(k) / float(v.keys - 1);
                    const float ang = (v.spin * s + 0.1f * i);
                    const V3 t = base + V3{0.6f * s, 0, 0};
                    if (v.srt) {
                        auto *m = reinterpret_cast<OptixSRTMotionTransform *>(p);
                        m->child = w->gas[0].h;
                        m->motionOptions = {(unsigned short)v.keys, 0, 0.0f, 1.0f};
                        OptixSRTData &d = m->srtData[k];
                        d = OptixSRTData{};
                        d.sx = d.sy = d.sz = 1.0f;
                        d.qy = std::sin(0.5f * ang);
                        d.qw = std::cos(0.5f * ang);
                        d.tx = t.x;
                        d.ty = t.y;
                        d.tz = t.z;
                    } else {
                        auto *m = reinterpret_cast<OptixMatrixMotionTransform *>(p);
                        m->child = w->gas[0].h;
                        m->motionOptions = {(unsigned short)v.keys, 0, 0.0f, 1.0f};
                        const Xf x = makeXf({0, 1, 0}, ang, 1.0f, t);
                        std::memcpy(m->transform[k], x.m, sizeof(x.m));
                    }
                }
            }
            Buf dm = dupload(host);
            std::vector<OptixInstance> inst;
            for (int i = 0; i < 64; ++i) {
                OptixTraversableHandle th;
                OX(optixConvertPointerToTraversableHandle(
                    g_ctx, dm.p + stride * i,
                    v.srt ? OPTIX_TRAVERSABLE_TYPE_SRT_MOTION_TRANSFORM : OPTIX_TRAVERSABLE_TYPE_MATRIX_MOTION_TRANSFORM, &th));
                inst.push_back(makeInstance(Xf{}, unsigned(i), th));
            }
            inst.push_back(w->inst[64]); // the ground stays static
            Buf ib;
            Accel ias = buildIas(inst, ib, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, v.iasKeys, 5);
            std::printf("  %s: transform %zu B per instance, IAS build %.3f ms, %.3f MB\n", v.n, bytes, ias.ms, mb(ias.out));
            printTrace("primary", trace(pl, ias.h, primary, hits, RG_HITOBJ, 0, 1.0f, true));
            printTrace("bounce", trace(pl, ias.h, bounce, hits, RG_HITOBJ, 0, 1.0f, true));
            ias.release();
            dfree(ib);
            dfree(dm);
        }
    }
    // (c) how many keys does a GAS accept?
    {
        std::printf("motion key limits (a 2-triangle GAS; SRT transform)\n");
        const Mesh q = makeQuad({0, 0, 0}, 1.0f, 0.0f);
        for (unsigned keys : {16u, 64u, 128u, 129u, 130u, 192u, 255u, 256u}) {
            std::vector<const std::vector<float> *> kp(keys, &q.P);
            InputP in = triangles(kp, q.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
            const OptixAccelBuildOptions o = buildOptions(OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, (unsigned short)keys,
                                                          OPTIX_BUILD_OPERATION_BUILD);
            OptixAccelBufferSizes s{};
            OptixResult r = optixAccelComputeMemoryUsage(g_ctx, &o, &in->in, 1, &s);
            if (r == OPTIX_SUCCESS) {
                Buf t = dalloc(s.tempSizeInBytes), out = dalloc(s.outputSizeInBytes);
                OptixTraversableHandle h;
                r = optixAccelBuild(g_ctx, g_stream, &o, &in->in, 1, t.p, t.n, out.p, out.n, &h, nullptr, 0);
                CU(cu.p_cuStreamSynchronize(g_stream));
                dfree(t);
                dfree(out);
            }
            std::printf("  GAS with %4u vertex keys: %s (output %.1f KB)\n", keys, optixGetErrorName(r), s.outputSizeInBytes / 1024.0);
        }
    }
}

// --- IAS builds -------------------------------------------------------------

void testIas() {
    std::printf("== ias: instance acceleration structures (GAS: a 20k-triangle blob)\n");
    const Mesh blob = makeBlob(100, 100);
    InputP in = triangles({&blob.P}, blob.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
    Accel gas = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, true);
    Pipeline pl(PipeCfg{});
    for (uint32_t n : {1u, 16u, 65u, 256u, 1000u, 10000u, 100000u, 1000000u, 4000000u}) {
        Rng rng(n);
        const float side = 3.0f * std::cbrt(float(n));
        std::vector<OptixInstance> inst(n);
        for (uint32_t i = 0; i < n; ++i)
            inst[i] = makeInstance(makeXf(rng.sphere(), 6.28f * rng.f(), 0.5f + rng.f(), {side * rng.f(), side * rng.f(), side * rng.f()}),
                                   i, gas.h);
        Buf ib;
        const double u0 = now();
        ib = dupload(inst);
        const double upMs = now() - u0;
        OptixBuildInput bi{};
        bi.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
        bi.instanceArray.instances = ib.p;
        bi.instanceArray.numInstances = n;
        Accel ft = buildAccel(&bi, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, false, 3);
        Accel fb = buildAccel(&bi, 1, OPTIX_BUILD_FLAG_PREFER_FAST_BUILD, 1, false, 3);
        Accel up = buildAccel(&bi, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_UPDATE, 1, false, 3);
        // Move every instance a little and update.
        for (OptixInstance &x : inst)
            x.transform[3] += 0.5f;
        h2d(ib.p, inst.data(), inst.size() * sizeof(OptixInstance));
        const float updMs = updateAccel(up, &bi, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_UPDATE);
        std::printf("  %8u instances: upload %7.2f ms (%.1f MB); build FAST_TRACE %7.2f ms (%.1f MB, temp %.1f MB), FAST_BUILD "
                    "%7.2f ms, ALLOW_UPDATE %7.2f ms, update %6.2f ms\n",
                    n, upMs, mb(ib.n), ft.ms, mb(ft.out), mb(ft.temp), fb.ms, up.ms, updMs);
        if (n == 1000000u) {
            Rays primary, bounce;
            cameraRays(primary, {0.5f * side, 0.5f * side, -0.3f * side}, {0.5f * side, 0.5f * side, 0.5f * side}, 60.0f, 1920, 1080);
            primary.upload();
            Hits hits(primary.n);
            printTrace("1M instances, FAST_TRACE IAS: primary", trace(pl, ft.h, primary, hits, RG_HITOBJ));
            hits.download(primary.n);
            Rng rb(3);
            bounceRays(bounce, primary, hits, {0, 0, 0}, {side, side, side}, rb);
            bounce.upload();
            printTrace("1M instances, FAST_TRACE IAS: bounce", trace(pl, ft.h, bounce, hits, RG_HITOBJ));
            printTrace("1M instances, FAST_BUILD IAS: primary", trace(pl, fb.h, primary, hits, RG_HITOBJ));
            printTrace("1M instances, FAST_BUILD IAS: bounce", trace(pl, fb.h, bounce, hits, RG_HITOBJ));
            printTrace("1M instances, updated IAS: bounce", trace(pl, up.h, bounce, hits, RG_HITOBJ));
        }
        ft.release();
        fb.release();
        up.release();
        dfree(ib);
    }
    gas.release();

    // Does the IAS build time depend on the instanced GAS and the rotation?
    std::printf("  64 instances, IAS build (median of 5), by instanced GAS and rotation:\n");
    for (uint32_t res : {100u, 1000u}) {
        const Mesh b = makeBlob(res, res / 2);
        InputP bin = triangles({&b.P}, b.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        Accel g = buildAccel(&bin->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, true);
        for (int rotated = 0; rotated < 2; ++rotated) {
            Rng rng(7);
            std::vector<OptixInstance> inst;
            for (uint32_t i = 0; i < 64; ++i) {
                const V3 axis = rng.sphere();
                const float angle = 6.28f * rng.f();
                inst.push_back(makeInstance(makeXf(axis, rotated ? angle : 0.0f, 1.0f, {2.5f * (i % 8), 0, 2.5f * (i / 8)}), i, g.h));
            }
            Buf ib;
            Accel a = buildIas(inst, ib, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, 5);
            std::printf("    GAS of %7u triangles, %-9s: %.3f ms\n", b.nt(), rotated ? "rotated" : "unrotated", a.ms);
            a.release();
            dfree(ib);
        }
        g.release();
    }
}

// --- intersectLocal: one instance's GAS -----------------------------------------

void testLocal() {
    std::printf("== local: subsurface-walk queries on one instance (Nova's intersectLocal)\n");
    const Mesh blob = makeBlob(1000, 500);
    InputP in = triangles({&blob.P}, blob.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
    Accel gas = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, true);
    const Xf xf = makeXf({1, 1, 0}, 0.52f, 1.3f, {5, 2, 1});
    const Xf inv = inverse(xf);
    Buf ib;
    std::vector<OptixInstance> one{makeInstance(xf, 0, gas.h)};
    Accel ias = buildIas(one, ib);
    PipeCfg cfg;
    cfg.graph = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS | OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
    Pipeline pl(cfg);
    // Walk segments start just inside the surface (object space), random
    // directions, world length 0.3.
    const uint32_t n = 2u << 20;
    Rays world(n), local(n);
    Rng rng(29);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t t = rng.u32() % blob.nt();
        const V3 p0 = ld3(blob.P, blob.I[3 * t]), p1 = ld3(blob.P, blob.I[3 * t + 1]), p2 = ld3(blob.P, blob.I[3 * t + 2]);
        float a = rng.f(), b = rng.f();
        if (a + b > 1) {
            a = 1 - a;
            b = 1 - b;
        }
        const V3 ng = normalize(cross(p1 - p0, p2 - p0)); // outward (winding)
        const V3 po = p0 + (p1 - p0) * a + (p2 - p0) * b - ng * 0.002f;
        const V3 dw = rng.sphere();
        const V3 ow = xf.point(po);
        world.set(i, ow, dw, 0.3f);
        local.set(i, inv.point(ow), inv.vector(dw), 0.3f); // parameter kept: t is the world distance
    }
    world.upload();
    local.upload();
    Hits hw(n), hl(n);
    printTrace("world rays through a 1-instance IAS", trace(pl, ias.h, world, hw, RG_HITOBJ));
    printTrace("object-space rays on the GAS handle", trace(pl, gas.h, local, hl, RG_HITOBJ));
    hw.download(n);
    hl.download(n);
    uint32_t agree = 0, both = 0, onlyOne = 0;
    double maxRel = 0, maxAbsOther = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const bool a = hw.hprim[i] != ~0u, b = hl.hprim[i] != ~0u;
        if (a && b) {
            ++both;
            const double dt = std::fabs(double(hw.ht[i]) - hl.ht[i]);
            if (hw.hprim[i] == hl.hprim[i]) {
                ++agree;
                maxRel = std::max(maxRel, dt);
            } else {
                maxAbsOther = std::max(maxAbsOther, dt);
            }
        } else if (a != b) {
            ++onlyOne;
        }
    }
    std::printf("  both hit %u, same prim %u (max |dt| %.2e), other prim: max |dt| %.2e; hit by only one %u\n",
                both, agree, maxRel, maxAbsOther, onlyOne);
    ias.release();
    gas.release();
    dfree(ib);
}

// --- precision: self-intersection with Nova's ray offsets ---------------------

void testPrecision() {
    std::printf("== precision: secondary rays leaving surfaces (self-hit = the ray hits its own primitive)\n");
    Rng rng(31);
    // Triangles: a 160k-triangle blob, rotated, far from the origin.
    {
        const Mesh blob = makeBlob(400, 200);
        InputP in = triangles({&blob.P}, blob.I, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        Accel gas = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, true);
        Pipeline pl(PipeCfg{});
        for (float T : {0.0f, 1e3f, 1e5f}) {
            const Xf xf = makeXf({0.3f, 1, 0.2f}, 0.7f, 1.0f, {T, 0.5f * T, -0.7f * T});
            Buf ib;
            std::vector<OptixInstance> one{makeInstance(xf, 0, gas.h)};
            Accel ias = buildIas(one, ib);
            {
                // Watertightness: the blob is closed and star-shaped around its
                // centre, so every ray from the centre must hit. Rays aimed
                // exactly at the vertices and at the edge midpoints.
                std::vector<V3> targets;
                for (uint32_t v = 0; v < blob.nv(); ++v)
                    targets.push_back(ld3(blob.P, v));
                for (uint32_t t = 0; t < blob.nt(); ++t)
                    for (int e = 0; e < 3; ++e)
                        targets.push_back((ld3(blob.P, blob.I[3 * t + e]) + ld3(blob.P, blob.I[3 * t + (e + 1) % 3])) * 0.5f);
                Rays leak(uint32_t(targets.size()));
                const V3 c = xf.point({0, 0, 0});
                for (uint32_t i = 0; i < leak.n; ++i)
                    leak.set(i, c, normalize(xf.point(targets[i]) - c));
                leak.upload();
                Hits lh(leak.n);
                const TraceResult r = trace(pl, ias.h, leak, lh, RG_HITOBJ, 0, 1.0f, false, 1);
                std::printf("  translation %.0e: %u rays from the centre at vertices and edge midpoints: %.0f missed\n", T,
                            leak.n, (1.0 - r.hitFrac) * leak.n);
            }
            Rays primary;
            cameraRays(primary, xf.point({0.2f, 0.3f, -3.5f}), xf.point({0, 0, 0}), 40.0f, 1000, 1000);
            primary.upload();
            Hits hits(primary.n);
            trace(pl, ias.h, primary, hits, RG_HITOBJ, 0, 1.0f, false, 1);
            hits.download(primary.n);
            std::vector<uint32_t> src;
            std::vector<V3> P, N;
            for (uint32_t i = 0; i < primary.n; ++i) {
                if (hits.hprim[i] == ~0u)
                    continue;
                const uint32_t t = hits.hprim[i];
                const V3 w0 = xf.point(ld3(blob.P, blob.I[3 * t])), w1 = xf.point(ld3(blob.P, blob.I[3 * t + 1])),
                         w2 = xf.point(ld3(blob.P, blob.I[3 * t + 2]));
                const float u = hits.huv[2 * i], v = hits.huv[2 * i + 1];
                V3 ng = normalize(cross(w1 - w0, w2 - w0));
                if (dot(ng, ld3(primary.dir, i)) > 0)
                    ng = -ng;
                src.push_back(t);
                P.push_back(w0 * (1 - u - v) + w1 * u + w2 * v); // Nova: interpolated world-space corners
                N.push_back(ng);
            }
            const uint32_t m = uint32_t(src.size());
            std::printf("  translation %.0e: %u surface points\n", T, m);
            const char *originNames[] = {"P, tmin 0", "offsetRayOrigin(P, +-Ng), tmin 0", "P, tmin 1e-4"};
            for (int side = 0; side < 2; ++side)
                for (int variant = 0; variant < 3; ++variant) {
                    Rays r(m);
                    Rng rr(37);
                    for (uint32_t i = 0; i < m; ++i) {
                        const V3 n = side == 0 ? N[i] : -N[i];
                        r.set(i, variant == 1 ? offsetRayOrigin(P[i], n) : P[i], rr.hemi(n));
                    }
                    r.upload();
                    Hits h2(m);
                    OaParams p = makeParams(ias.h, r, h2);
                    p.tmin = variant == 2 ? 1e-4f : 0.0f;
                    pl.launch(p, RG_HITOBJ, m);
                    CU(cu.p_cuStreamSynchronize(g_stream));
                    h2.download(m);
                    uint32_t self = 0, nearHits = 0;
                    for (uint32_t i = 0; i < m; ++i) {
                        if (h2.hprim[i] == ~0u)
                            continue;
                        self += h2.hprim[i] == src[i];
                        nearHits += h2.ht[i] < 1e-3f;
                    }
                    std::printf("    %-13s %-34s self-hits %7u (%.4f %%), hits closer than 1e-3: %u\n",
                                side ? "transmission" : "reflection", originNames[variant], self, 100.0 * self / m, nearHits);
                }
            ias.release();
            dfree(ib);
        }
        gas.release();
    }
    // Curves: linear and cubic B-spline hair, rotated and translated.
    for (int basis = 0; basis < 2; ++basis) {
        const CurveType &ct = basis == 0 ? kCurveTypes[0] : kCurveTypes[2];
        const HairSet h = makeHair(ct, 20000, 8, 4.0f, 1.0f, 0.005f, 0.005f, 41);
        InputP in = curves(ct.type, {&h.cp}, {&h.w}, h.idx, OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT);
        Accel gas = buildAccel(&in->in, 1, OPTIX_BUILD_FLAG_PREFER_FAST_TRACE, 1, true);
        PipeCfg cfg;
        cfg.groups[0].type = ct.type;
        Pipeline pl(cfg);
        for (float T : {0.0f, 1e3f}) {
            const Xf xf = makeXf({0, 1, 0}, 0.5f, 1.0f, {T, 0.3f * T, -0.6f * T});
            Buf ib;
            std::vector<OptixInstance> one{makeInstance(xf, 0, gas.h)};
            Accel ias = buildIas(one, ib);
            Rays primary;
            cameraRays(primary, xf.point({2.0f, 1.2f, -1.5f}), xf.point({2.0f, 0.4f, 2.0f}), 45.0f, 1000, 1000);
            primary.upload();
            Hits hits(primary.n);
            trace(pl, ias.h, primary, hits, RG_HITOBJ, 0, 1.0f, false, 1);
            hits.download(primary.n);
            std::vector<uint32_t> src;
            std::vector<V3> P, N, Tg;
            double radialErr = 0, radialMax = 0;
            for (uint32_t i = 0; i < primary.n; ++i) {
                if (hits.hprim[i] == ~0u)
                    continue;
                const uint32_t s = hits.hprim[i], b0 = h.idx[s];
                const float u = std::clamp(hits.huv[2 * i], 0.0f, 1.0f);
                const int ncp = ct.degree == 1 ? 2 : 4;
                float px[4], py[4], pz[4];
                for (int k = 0; k < ncp; ++k) {
                    px[k] = h.cp[3 * (b0 + k)];
                    py[k] = h.cp[3 * (b0 + k) + 1];
                    pz[k] = h.cp[3 * (b0 + k) + 2];
                }
                const float du = 1e-3f;
                auto at = [&](float uu) { return V3{evalBasis(ct.type, px, uu), evalBasis(ct.type, py, uu), evalBasis(ct.type, pz, uu)}; };
                const V3 c = xf.point(at(u));
                const V3 tg = normalize(xf.vector(at(std::min(1.0f, u + du)) - at(std::max(0.0f, u - du))));
                const V3 Pw = ld3(primary.org, i) + ld3(primary.dir, i) * hits.ht[i]; // Nova: org + dir * t
                V3 rad = Pw - c;
                const bool cap = ct.degree == 1 && (hits.huv[2 * i] <= 0.0f || hits.huv[2 * i] >= 1.0f);
                if (!cap)
                    rad = rad - tg * dot(rad, tg);
                const float rl = length(rad);
                radialErr += std::fabs(rl - 0.005f) / 0.005f;
                radialMax = std::max(radialMax, double(std::fabs(rl - 0.005f) / 0.005f));
                src.push_back(s);
                P.push_back(Pw);
                N.push_back(rad * (1.0f / rl));
                Tg.push_back(tg);
            }
            const uint32_t m = uint32_t(src.size());
            std::printf("  %s, translation %.0e: %u hits, |distance to axis - r| / r mean %.2e max %.2e\n", ct.n, T, m,
                        radialErr / m, radialMax);
            const char *originNames[] = {"P", "offsetRayOrigin(P, +-N)", "tubeRayOrigin"};
            for (int side = 0; side < 2; ++side)
                for (int variant = 0; variant < 3; ++variant) {
                    Rays r(m);
                    Rng rr(43);
                    for (uint32_t i = 0; i < m; ++i) {
                        const V3 n = side == 0 ? N[i] : -N[i];
                        const V3 d = rr.hemi(n);
                        const V3 o = variant == 0 ? P[i] : variant == 1 ? offsetRayOrigin(P[i], n) : tubeRayOrigin(P[i], N[i], Tg[i], 0.005f, d);
                        r.set(i, o, d);
                    }
                    r.upload();
                    Hits h2(m);
                    OaParams p = makeParams(ias.h, r, h2);
                    pl.launch(p, RG_HITOBJ, m);
                    CU(cu.p_cuStreamSynchronize(g_stream));
                    h2.download(m);
                    uint32_t same = 0;
                    for (uint32_t i = 0; i < m; ++i)
                        same += h2.hprim[i] == src[i] && h2.ht[i] < 4.0f * 0.005f;
                    std::printf("    %-13s %-26s same segment within 4r: %7u (%.3f %%)\n", side ? "into the fibre" : "away",
                                originNames[variant], same, 100.0 * same / m);
                }
            ias.release();
            dfree(ib);
        }
        gas.release();
    }
}

} // namespace

int main(int argc, char **argv) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/") + 1);
    const std::string what = argc > 1 ? argv[1] : "all";
    initGpu();
    const struct {
        const char *n;
        void (*f)();
    } tests[] = {{"info", testInfo},     {"tri", testTriangles}, {"trace", testTrace},   {"anyhit", testAnyHit},
                 {"launch", testLaunch}, {"curves", testCurves}, {"points", testPoints}, {"attrs", testAttributes},
                 {"motion", testMotion}, {"ias", testIas},       {"local", testLocal},   {"precision", testPrecision}};
    bool ran = false;
    for (const auto &t : tests)
        if (what == "all" || ("," + what + ",").find(std::string(",") + t.n + ",") != std::string::npos) {
            const double t0 = now();
            t.f();
            std::printf("(%s: %.1f s)\n\n", t.n, (now() - t0) * 1e-3);
            std::fflush(stdout);
            ran = true;
        }
    if (!ran)
        fail("unknown test '%s'", what.c_str());
    optixDeviceContextDestroy(g_ctx);
    return 0;
}
