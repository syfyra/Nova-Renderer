// M7 research spike: the OptiX 9.1 denoiser without an OptiX SDK install and
// without linking cuda.lib.
//
//  - nvcuda.dll is loaded with LoadLibrary and its entry points are resolved
//    by name (GetProcAddress), so the program starts on machines without an
//    NVIDIA driver and can report "no CUDA" instead of failing to load;
//  - optixInit() (optix_stubs.h) loads nvoptix.dll from the driver;
//  - the CUDA context is the device's primary context (cuDevicePrimaryCtxRetain),
//    pushed/popped around every use, with one non-blocking stream;
//  - images come from husk's multi-part EXRs through Houdini's OpenImageIO.
//
// Modes:
//   denoise   optix_denoise_spike --in noisy.exr --out out.exr
//                 [--beauty C] [--albedo albedo] [--normal N]   EXR part names
//                 [--guides none|albedo|albedo+normal]
//                 [--nspace world|camera|camera-flipz] [--w2c m00,m01,...,m22]
//                 [--normal-from other.exr]                      normal guide of another render
//                 [--alpha copy|denoise] [--intensity auto|compute] [--blend 0]
//                 [--half] [--model aov|temporal|upscale2x] [--tile N] [--overlap N]
//                 [--iters N] [--device N] [--optix-log 0..4]
//   bench     optix_denoise_spike --in noisy.exr --bench [--iters N]
//                 runs the timing/memory table (resolutions, formats, layers,
//                 tiling, upscale, temporal) on images resampled from --in.
// Times are wall-clock milliseconds around each call plus a stream
// synchronize; "gpu" is the invoke measured with CUDA events; steady-state
// values are medians over --iters frames after the first.
//
// Built by build_spike.bat (VS 2022 Build Tools, CUDA 12.9 headers only,
// OptiX 9.1 headers from the Nova build tree, Houdini's OIIO); with
// "build_spike.bat <dir> dll" as a DLL exporting spike_main for
// probes/inproc_probe.py.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cuda.h>
#include <optix.h>
#include <optix_denoiser_tiling.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#define IMATH_HALF_NO_LOOKUP_TABLE 1 // Imath_sidefx.lib does not export the lookup table
#include <Imath/half.h>
#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// CUDA driver API, loaded at run time.
// ---------------------------------------------------------------------------
#define NOVA_STR2(x) #x
#define NOVA_STR(x) NOVA_STR2(x)
// The cuda.h macros map e.g. cuMemAlloc -> cuMemAlloc_v2; NOVA_STR expands
// them, so the versioned export is looked up.
#define NOVA_CU_FUNCS(X) \
    X(cuInit) X(cuDriverGetVersion) X(cuDeviceGetCount) X(cuDeviceGet) X(cuDeviceGetName) \
    X(cuDeviceGetAttribute) X(cuDevicePrimaryCtxRetain) X(cuDevicePrimaryCtxRelease) \
    X(cuDevicePrimaryCtxGetState) X(cuCtxPushCurrent) X(cuCtxPopCurrent) X(cuCtxGetCurrent) \
    X(cuStreamCreate) X(cuStreamDestroy) X(cuStreamSynchronize) X(cuMemAlloc) X(cuMemFree) \
    X(cuMemAllocHost) X(cuMemFreeHost) X(cuMemcpyHtoDAsync) X(cuMemcpyDtoHAsync) X(cuMemsetD8Async) \
    X(cuMemGetInfo) X(cuEventCreate) X(cuEventDestroy) X(cuEventRecord) X(cuEventSynchronize) \
    X(cuEventElapsedTime) X(cuGetErrorName)

struct CudaApi
{
    HMODULE dll = nullptr;
#define NOVA_CU_DECL(fn) decltype(&fn) p_##fn = nullptr;
    NOVA_CU_FUNCS(NOVA_CU_DECL)
#undef NOVA_CU_DECL

    bool load()
    {
        dll = LoadLibraryA("nvcuda.dll");
        if (!dll)
            return false;
#define NOVA_CU_LOAD(fn) \
    p_##fn = reinterpret_cast<decltype(&fn)>(GetProcAddress(dll, NOVA_STR(fn))); \
    if (!p_##fn) { std::fprintf(stderr, "nvcuda.dll lacks %s\n", NOVA_STR(fn)); return false; }
        NOVA_CU_FUNCS(NOVA_CU_LOAD)
#undef NOVA_CU_LOAD
        return true;
    }
};
static CudaApi cu;

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

#define CU_CHECK(call)                                                                   \
    do {                                                                                 \
        CUresult r_ = (call);                                                            \
        if (r_ != CUDA_SUCCESS) {                                                        \
            const char* n_ = "?";                                                        \
            if (cu.p_cuGetErrorName) cu.p_cuGetErrorName(r_, &n_);                       \
            std::fprintf(stderr, "CUDA error %s (%d) at %s:%d: %s\n", n_, int(r_), __FILE__, \
                         __LINE__, #call);                                               \
            std::exit(3);                                                                \
        }                                                                                \
    } while (0)
#define OX_CHECK(call)                                                                   \
    do {                                                                                 \
        OptixResult r_ = (call);                                                         \
        if (r_ != OPTIX_SUCCESS) {                                                       \
            std::fprintf(stderr, "OptiX error %s (%d) at %s:%d: %s\n",                    \
                         OPTIX_FUNCTION_TABLE_SYMBOL.optixGetErrorName ? optixGetErrorName(r_) : "?", \
                         int(r_), __FILE__,                                              \
                         __LINE__, #call);                                               \
            std::exit(4);                                                                \
        }                                                                                \
    } while (0)

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------
struct Plane
{
    int w = 0, h = 0, c = 0;
    std::vector<float> px; // w*h*c
};

struct Input
{
    Plane beauty, albedo, normal; // RGBA, RGB, XYZ
    bool haveW2C = false;
    float w2c[16] = {};           // row-major Imath convention (p_cam = p_world * M)
};

static bool readPlane(OIIO::ImageInput& in, int sub, Plane& p, int wantChannels)
{
    if (!in.seek_subimage(sub, 0))
        return false;
    const OIIO::ImageSpec& s = in.spec();
    p.w = s.width;
    p.h = s.height;
    p.c = wantChannels;
    std::vector<float> all(size_t(s.width) * s.height * s.nchannels);
    if (!in.read_image(sub, 0, 0, s.nchannels, OIIO::TypeDesc::FLOAT, all.data()))
        return false;
    p.px.assign(size_t(p.w) * p.h * p.c, 0.0f);
    for (size_t i = 0; i < size_t(p.w) * p.h; ++i)
        for (int k = 0; k < p.c; ++k)
            p.px[i * p.c + k] = k < s.nchannels ? all[i * s.nchannels + k] : 1.0f;
    return true;
}

static bool loadInput(const std::string& path, const std::string& beauty, const std::string& albedo,
                      const std::string& normal, Input& out)
{
    auto in = OIIO::ImageInput::open(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), OIIO::geterror().c_str());
        return false;
    }
    for (int sub = 0; in->seek_subimage(sub, 0); ++sub) {
        const OIIO::ImageSpec& s = in->spec();
        std::string name = s.get_string_attribute("name", sub == 0 ? beauty : "");
        if (sub == 0) {
            const OIIO::ParamValue* m = s.find_attribute("worldToCamera");
            if (m && m->type() == OIIO::TypeMatrix44) {
                std::memcpy(out.w2c, m->data(), sizeof(out.w2c));
                out.haveW2C = true;
            }
        }
        if (name == beauty) readPlane(*in, sub, out.beauty, 4);
        else if (name == albedo) readPlane(*in, sub, out.albedo, 3);
        else if (name == normal) readPlane(*in, sub, out.normal, 3);
    }
    if (out.beauty.px.empty()) {
        std::fprintf(stderr, "no beauty part '%s' in %s\n", beauty.c_str(), path.c_str());
        return false;
    }
    return true;
}

static bool writeRGBA(const std::string& path, const Plane& p)
{
    auto out = OIIO::ImageOutput::create(path);
    if (!out)
        return false;
    OIIO::ImageSpec spec(p.w, p.h, p.c, OIIO::TypeDesc::FLOAT);
    spec.attribute("compression", "zip");
    if (!out->open(path, spec))
        return false;
    bool ok = out->write_image(OIIO::TypeDesc::FLOAT, p.px.data());
    out->close();
    return ok;
}

static Plane resample(const Plane& src, int w, int h)
{
    Plane p;
    p.w = w;
    p.h = h;
    p.c = src.c;
    p.px.resize(size_t(w) * h * p.c);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sx = std::min(src.w - 1, x * src.w / w), sy = std::min(src.h - 1, y * src.h / h);
            std::memcpy(&p.px[(size_t(y) * w + x) * p.c], &src.px[(size_t(sy) * src.w + sx) * src.c],
                        sizeof(float) * p.c);
        }
    return p;
}

// ---------------------------------------------------------------------------
// Device helpers
// ---------------------------------------------------------------------------
struct DevBuf
{
    CUdeviceptr ptr = 0;
    size_t size = 0;
    void alloc(size_t n)
    {
        if (n == 0) return;
        CU_CHECK(cu.p_cuMemAlloc(&ptr, n));
        size = n;
    }
    void free()
    {
        if (ptr) cu.p_cuMemFree(ptr);
        ptr = 0;
        size = 0;
    }
};

static OptixImage2D image2D(CUdeviceptr p, int w, int h, OptixPixelFormat f, unsigned pixelBytes)
{
    OptixImage2D im{};
    im.data = p;
    im.width = unsigned(w);
    im.height = unsigned(h);
    im.pixelStrideInBytes = pixelBytes;
    im.rowStrideInBytes = unsigned(w) * pixelBytes;
    im.format = f;
    return im;
}

static void optixLog(unsigned int level, const char* tag, const char* msg, void*)
{
    std::fprintf(stderr, "  [optix %u][%s] %s\n", level, tag, msg);
}

// Host-side packing to the pixel format of a case (float or half, n channels).
static void pack(const Plane& p, int channels, bool half, std::vector<unsigned char>& out)
{
    size_t n = size_t(p.w) * p.h;
    out.resize(n * channels * (half ? 2 : 4));
    if (half) {
        auto* h = reinterpret_cast<Imath::half*>(out.data());
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < channels; ++k)
                h[i * channels + k] = Imath::half(k < p.c ? p.px[i * p.c + k] : 1.0f);
    } else {
        auto* f = reinterpret_cast<float*>(out.data());
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < channels; ++k)
                f[i * channels + k] = k < p.c ? p.px[i * p.c + k] : 1.0f;
    }
}

// ---------------------------------------------------------------------------
// One denoiser configuration
// ---------------------------------------------------------------------------
struct Case
{
    std::string name;
    int w = 0, h = 0;
    OptixDenoiserModelKind model = OPTIX_DENOISER_MODEL_KIND_AOV;
    bool albedo = true, normal = true;
    OptixDenoiserAlphaMode alpha = OPTIX_DENOISER_ALPHA_MODE_COPY;
    bool half = false;   // half4 beauty/AOVs and half3 guides instead of float
    int layers = 1;      // beauty + (layers - 1) extra AOV layers (kernel prediction)
    int tile = 0;        // 0: whole image; else tile edge (optixUtilDenoiserInvokeTiled)
    int overlap = -1;    // tile overlap; -1: OptixDenoiserSizes::overlapWindowSizeInPixels
    bool intensity = true; // optixDenoiserComputeIntensity/AverageColor every frame
    float blend = 0.0f;
    int iters = 20;
    bool pinned = true;  // page-locked host staging buffers
};

struct Result
{
    double create = 0, memres = 0, alloc = 0, setup = 0;
    double firstFrame = 0;        // upload + intensity + invoke + download, first call
    double upload = 0, intensity = 0, invoke = 0, download = 0, frame = 0; // steady-state medians (wall)
    double invokeGpu = 0;         // steady-state median (CUDA events)
    double frameMin = 0;
    OptixDenoiserSizes sizes{};
    size_t deviceBytes = 0;
    long long vramDelta = 0;
};

static double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static Result runCase(OptixDeviceContext octx, CUstream stream, const Case& c, const Plane& beauty,
                      const Plane* albedo, const Plane* normal, Plane* outBeauty)
{
    Result r;
    size_t free0 = 0, total = 0;
    CU_CHECK(cu.p_cuMemGetInfo(&free0, &total));

    const bool upscale = c.model == OPTIX_DENOISER_MODEL_KIND_UPSCALE2X ||
                         c.model == OPTIX_DENOISER_MODEL_KIND_TEMPORAL_UPSCALE2X;
    const bool temporal = c.model == OPTIX_DENOISER_MODEL_KIND_TEMPORAL_AOV ||
                          c.model == OPTIX_DENOISER_MODEL_KIND_TEMPORAL_UPSCALE2X;
    const int ow = upscale ? 2 * c.w : c.w, oh = upscale ? 2 * c.h : c.h;

    OptixDenoiserOptions opts{};
    opts.guideAlbedo = c.albedo ? 1u : 0u;
    opts.guideNormal = c.normal ? 1u : 0u;
    opts.denoiseAlpha = c.alpha;

    auto t = Clock::now();
    OptixDenoiser dn = nullptr;
    OX_CHECK(optixDenoiserCreate(octx, c.model, &opts, &dn));
    r.create = msSince(t);

    t = Clock::now();
    const unsigned tw = c.tile ? unsigned(c.tile) : unsigned(c.w);
    const unsigned th = c.tile ? unsigned(c.tile) : unsigned(c.h);
    OX_CHECK(optixDenoiserComputeMemoryResources(dn, tw, th, &r.sizes));
    r.memres = msSince(t);
    const unsigned overlap = !c.tile ? 0u : c.overlap >= 0 ? unsigned(c.overlap) : r.sizes.overlapWindowSizeInPixels;
    const size_t scratchBytes = c.tile ? r.sizes.withOverlapScratchSizeInBytes
                                       : r.sizes.withoutOverlapScratchSizeInBytes;
    const size_t scratchTotal = std::max({scratchBytes, r.sizes.computeIntensitySizeInBytes,
                                          r.sizes.computeAverageColorSizeInBytes});

    const OptixPixelFormat f4 = c.half ? OPTIX_PIXEL_FORMAT_HALF4 : OPTIX_PIXEL_FORMAT_FLOAT4;
    const OptixPixelFormat f3 = c.half ? OPTIX_PIXEL_FORMAT_HALF3 : OPTIX_PIXEL_FORMAT_FLOAT3;
    const unsigned px4 = c.half ? 8u : 16u, px3 = c.half ? 6u : 12u;
    const size_t inPx = size_t(c.w) * c.h, outPx = size_t(ow) * oh;

    t = Clock::now();
    DevBuf state, scratch, hdr, avg, alb, nrm, flow, prevGuide, curGuide;
    std::vector<DevBuf> in(c.layers), out(c.layers);
    state.alloc(r.sizes.stateSizeInBytes);
    scratch.alloc(scratchTotal);
    hdr.alloc(sizeof(float));
    avg.alloc(3 * sizeof(float));
    for (int l = 0; l < c.layers; ++l) {
        in[l].alloc(inPx * px4);
        out[l].alloc(outPx * px4);
    }
    if (c.albedo) alb.alloc(inPx * px3);
    if (c.normal) nrm.alloc(inPx * px3);
    if (temporal) {
        flow.alloc(inPx * (c.half ? 4 : 8));
        CU_CHECK(cu.p_cuMemsetD8Async(flow.ptr, 0, flow.size, stream));
        size_t g = outPx * r.sizes.internalGuideLayerPixelSizeInBytes;
        prevGuide.alloc(g);
        curGuide.alloc(g);
        CU_CHECK(cu.p_cuMemsetD8Async(prevGuide.ptr, 0, g, stream));
    }
    CU_CHECK(cu.p_cuStreamSynchronize(stream));
    r.alloc = msSince(t);
    r.deviceBytes = state.size + scratch.size + alb.size + nrm.size + flow.size + prevGuide.size + curGuide.size;
    for (int l = 0; l < c.layers; ++l) r.deviceBytes += in[l].size + out[l].size;

    t = Clock::now();
    OX_CHECK(optixDenoiserSetup(dn, stream, tw + 2 * overlap, th + 2 * overlap, state.ptr, state.size,
                                scratch.ptr, scratch.size));
    CU_CHECK(cu.p_cuStreamSynchronize(stream));
    r.setup = msSince(t);

    size_t free1 = 0;
    CU_CHECK(cu.p_cuMemGetInfo(&free1, &total));
    r.vramDelta = (long long)free0 - (long long)free1;

    // Host staging
    std::vector<unsigned char> hb, ha, hn;
    pack(beauty, 4, c.half, hb);
    if (c.albedo) pack(*albedo, 3, c.half, ha);
    if (c.normal) pack(*normal, 3, c.half, hn);
    void* pinB = nullptr; void* pinA = nullptr; void* pinN = nullptr; void* pinOut = nullptr;
    const size_t outBytes = outPx * px4;
    if (c.pinned) {
        CU_CHECK(cu.p_cuMemAllocHost(&pinB, hb.size()));
        std::memcpy(pinB, hb.data(), hb.size());
        if (c.albedo) { CU_CHECK(cu.p_cuMemAllocHost(&pinA, ha.size())); std::memcpy(pinA, ha.data(), ha.size()); }
        if (c.normal) { CU_CHECK(cu.p_cuMemAllocHost(&pinN, hn.size())); std::memcpy(pinN, hn.data(), hn.size()); }
        CU_CHECK(cu.p_cuMemAllocHost(&pinOut, outBytes));
    }
    std::vector<unsigned char> hostOut(outBytes);
    const void* srcB = c.pinned ? pinB : hb.data();
    const void* srcA = c.pinned ? pinA : ha.data();
    const void* srcN = c.pinned ? pinN : hn.data();
    void* dstO = c.pinned ? pinOut : hostOut.data();

    OptixDenoiserGuideLayer guide{};
    if (c.albedo) guide.albedo = image2D(alb.ptr, c.w, c.h, f3, px3);
    if (c.normal) guide.normal = image2D(nrm.ptr, c.w, c.h, f3, px3);
    if (temporal) {
        guide.flow = image2D(flow.ptr, c.w, c.h, c.half ? OPTIX_PIXEL_FORMAT_HALF2 : OPTIX_PIXEL_FORMAT_FLOAT2,
                             c.half ? 4u : 8u);
        unsigned gp = unsigned(r.sizes.internalGuideLayerPixelSizeInBytes);
        guide.previousOutputInternalGuideLayer = image2D(prevGuide.ptr, ow, oh, OPTIX_PIXEL_FORMAT_INTERNAL_GUIDE_LAYER, gp);
        guide.outputInternalGuideLayer = image2D(curGuide.ptr, ow, oh, OPTIX_PIXEL_FORMAT_INTERNAL_GUIDE_LAYER, gp);
    }
    std::vector<OptixDenoiserLayer> layers(c.layers);
    for (int l = 0; l < c.layers; ++l) {
        layers[l].input = image2D(in[l].ptr, c.w, c.h, f4, px4);
        layers[l].output = image2D(out[l].ptr, ow, oh, f4, px4);
        layers[l].type = l == 0 ? OPTIX_DENOISER_AOV_TYPE_BEAUTY : OPTIX_DENOISER_AOV_TYPE_DIFFUSE;
        if (temporal) layers[l].previousOutput = layers[l].input; // first frame: noisy input (header note)
    }

    CUevent e0, e1;
    CU_CHECK(cu.p_cuEventCreate(&e0, CU_EVENT_DEFAULT));
    CU_CHECK(cu.p_cuEventCreate(&e1, CU_EVENT_DEFAULT));

    std::vector<double> vUp, vInt, vInv, vDown, vFrame, vGpu;
    for (int it = 0; it <= c.iters; ++it) {
        auto tf = Clock::now();
        auto tt = Clock::now();
        for (int l = 0; l < c.layers; ++l)
            CU_CHECK(cu.p_cuMemcpyHtoDAsync(in[l].ptr, srcB, hb.size(), stream));
        if (c.albedo) CU_CHECK(cu.p_cuMemcpyHtoDAsync(alb.ptr, srcA, ha.size(), stream));
        if (c.normal) CU_CHECK(cu.p_cuMemcpyHtoDAsync(nrm.ptr, srcN, hn.size(), stream));
        CU_CHECK(cu.p_cuStreamSynchronize(stream));
        double up = msSince(tt);

        OptixDenoiserParams params{};
        params.blendFactor = c.blend;
        params.temporalModeUsePreviousLayers = (temporal && it > 0) ? 1u : 0u;
        tt = Clock::now();
        if (c.intensity) {
            OX_CHECK(optixDenoiserComputeIntensity(dn, stream, &layers[0].input, hdr.ptr, scratch.ptr, scratch.size));
            OX_CHECK(optixDenoiserComputeAverageColor(dn, stream, &layers[0].input, avg.ptr, scratch.ptr, scratch.size));
            params.hdrIntensity = hdr.ptr;
            params.hdrAverageColor = avg.ptr;
            CU_CHECK(cu.p_cuStreamSynchronize(stream));
        }
        double inten = msSince(tt);

        if (temporal && it > 0) {
            for (int l = 0; l < c.layers; ++l) layers[l].previousOutput = layers[l].output;
            std::swap(guide.previousOutputInternalGuideLayer.data, guide.outputInternalGuideLayer.data);
        }
        tt = Clock::now();
        CU_CHECK(cu.p_cuEventRecord(e0, stream));
        if (c.tile) {
            OX_CHECK(optixUtilDenoiserInvokeTiled(dn, stream, &params, state.ptr, state.size, &guide, layers.data(),
                                                  unsigned(c.layers), scratch.ptr, scratch.size, overlap,
                                                  unsigned(c.tile), unsigned(c.tile)));
        } else {
            OX_CHECK(optixDenoiserInvoke(dn, stream, &params, state.ptr, state.size, &guide, layers.data(),
                                         unsigned(c.layers), 0, 0, scratch.ptr, scratch.size));
        }
        CU_CHECK(cu.p_cuEventRecord(e1, stream));
        CU_CHECK(cu.p_cuEventSynchronize(e1));
        double inv = msSince(tt);
        float gpuMs = 0;
        CU_CHECK(cu.p_cuEventElapsedTime(&gpuMs, e0, e1));

        tt = Clock::now();
        CU_CHECK(cu.p_cuMemcpyDtoHAsync(dstO, out[0].ptr, outBytes, stream));
        CU_CHECK(cu.p_cuStreamSynchronize(stream));
        double down = msSince(tt);
        double frame = msSince(tf);
        if (it == 0) {
            r.firstFrame = frame;
        } else {
            vUp.push_back(up); vInt.push_back(inten); vInv.push_back(inv); vDown.push_back(down);
            vFrame.push_back(frame); vGpu.push_back(gpuMs);
        }
    }
    r.upload = median(vUp); r.intensity = median(vInt); r.invoke = median(vInv); r.download = median(vDown);
    r.frame = median(vFrame); r.invokeGpu = median(vGpu);
    r.frameMin = vFrame.empty() ? 0 : *std::min_element(vFrame.begin(), vFrame.end());

    if (outBeauty) {
        outBeauty->w = ow; outBeauty->h = oh; outBeauty->c = 4;
        outBeauty->px.resize(outPx * 4);
        const unsigned char* src = static_cast<const unsigned char*>(dstO);
        for (size_t i = 0; i < outPx * 4; ++i)
            outBeauty->px[i] = c.half ? float(reinterpret_cast<const Imath::half*>(src)[i])
                                      : reinterpret_cast<const float*>(src)[i];
    }

    cu.p_cuEventDestroy(e0); cu.p_cuEventDestroy(e1);
    if (pinB) cu.p_cuMemFreeHost(pinB);
    if (pinA) cu.p_cuMemFreeHost(pinA);
    if (pinN) cu.p_cuMemFreeHost(pinN);
    if (pinOut) cu.p_cuMemFreeHost(pinOut);
    for (auto& b : in) b.free();
    for (auto& b : out) b.free();
    state.free(); scratch.free(); hdr.free(); avg.free(); alb.free(); nrm.free(); flow.free();
    prevGuide.free(); curGuide.free();
    OX_CHECK(optixDenoiserDestroy(dn));
    return r;
}

static void printResult(const Case& c, const Result& r)
{
    std::printf("%-34s %5dx%-5d create %7.2f memres %6.3f alloc %6.2f setup %8.2f | first %8.2f | "
                "up %6.2f int %5.2f inv %7.2f (gpu %7.2f) down %6.2f frame %7.2f (min %7.2f) | "
                "state %6.1f MB scratch %6.1f/%6.1f MB overlap %u guidepx %zu vram %+.0f MB\n",
                c.name.c_str(), c.w, c.h, r.create, r.memres, r.alloc, r.setup, r.firstFrame, r.upload,
                r.intensity, r.invoke, r.invokeGpu, r.download, r.frame, r.frameMin,
                r.sizes.stateSizeInBytes / 1048576.0, r.sizes.withoutOverlapScratchSizeInBytes / 1048576.0,
                r.sizes.withOverlapScratchSizeInBytes / 1048576.0, r.sizes.overlapWindowSizeInPixels,
                r.sizes.internalGuideLayerPixelSizeInBytes, r.vramDelta / 1048576.0);
    std::fflush(stdout);
}

#ifdef SPIKE_DLL
// DLL variant (build_spike.bat dll): hython loads it with ctypes and calls
// spike_main inside a Houdini process that already uses CUDA/OptiX itself.
extern "C" __declspec(dllexport) int spike_main(int argc, char** argv)
#else
int main(int argc, char** argv)
#endif
{
    std::map<std::string, std::string> a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) == 0) {
            if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) a[k] = argv[++i];
            else a[k] = "1";
        }
    }
    auto get = [&](const char* k, const char* d) { auto it = a.find(k); return it == a.end() ? std::string(d) : it->second; };

    const auto tProcess = Clock::now();
    Input input;
    if (!a.count("--in")) {
        std::fprintf(stderr, "usage: optix_denoise_spike --in noisy.exr (--out out.exr | --bench) [options]\n");
        return 2;
    }
    if (!loadInput(get("--in", ""), get("--beauty", "C"), get("--albedo", "albedo"), get("--normal", "N"), input))
        return 2;
    std::printf("input %dx%d, albedo %s, normal %s, worldToCamera %s (read %.1f ms)\n", input.beauty.w,
                input.beauty.h, input.albedo.px.empty() ? "no" : "yes", input.normal.px.empty() ? "no" : "yes",
                input.haveW2C ? "yes" : "no", msSince(tProcess));

    // --- CUDA + OptiX initialisation, timed step by step -------------------
    auto t = Clock::now();
    if (!cu.load()) {
        std::fprintf(stderr, "nvcuda.dll not available: no NVIDIA driver -> denoiser disabled\n");
        return 5;
    }
    double tLoad = msSince(t);
    t = Clock::now();
    CU_CHECK(cu.p_cuInit(0));
    double tInit = msSince(t);
    int drv = 0, ndev = 0;
    CU_CHECK(cu.p_cuDriverGetVersion(&drv));
    CU_CHECK(cu.p_cuDeviceGetCount(&ndev));
    const int ordinal = std::atoi(get("--device", "0").c_str());
    CUdevice dev;
    CU_CHECK(cu.p_cuDeviceGet(&dev, ordinal));
    char name[256] = {};
    CU_CHECK(cu.p_cuDeviceGetName(name, sizeof(name), dev));
    int ccMaj = 0, ccMin = 0;
    cu.p_cuDeviceGetAttribute(&ccMaj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
    cu.p_cuDeviceGetAttribute(&ccMin, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
    unsigned int pflags = 0;
    int pactive = 0;
    cu.p_cuDevicePrimaryCtxGetState(dev, &pflags, &pactive);

    t = Clock::now();
    CUcontext ctx = nullptr;
    CU_CHECK(cu.p_cuDevicePrimaryCtxRetain(&ctx, dev));
    CU_CHECK(cu.p_cuCtxPushCurrent(ctx));
    size_t freeCtx = 0, totalMem = 0;
    CU_CHECK(cu.p_cuMemGetInfo(&freeCtx, &totalMem)); // forces the lazy primary context creation
    double tCtx = msSince(t);
    t = Clock::now();
    CUstream stream = nullptr;
    CU_CHECK(cu.p_cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING));
    double tStream = msSince(t);

    t = Clock::now();
    void* optixLib = nullptr;
    OX_CHECK(optixInitWithHandle(&optixLib));
    double tOptixInit = msSince(t);
    char optixPath[MAX_PATH] = {};
    GetModuleFileNameA(static_cast<HMODULE>(optixLib), optixPath, MAX_PATH);

    t = Clock::now();
    OptixDeviceContextOptions copts{};
    copts.logCallbackFunction = optixLog;
    copts.logCallbackLevel = unsigned(std::atoi(get("--optix-log", "2").c_str()));
    OptixDeviceContext octx = nullptr;
    OX_CHECK(optixDeviceContextCreate(ctx, &copts, &octx));
    double tOctx = msSince(t);
    size_t freeAfter = 0;
    CU_CHECK(cu.p_cuMemGetInfo(&freeAfter, &totalMem));

    std::printf("device %d '%s' sm_%d%d, %d device(s), driver API %d, primary ctx before: active %d flags 0x%x\n",
                ordinal, name, ccMaj, ccMin, ndev, drv, pactive, pflags);
    std::printf("nvoptix: %s (OptiX headers %d, ABI %d)\n", optixPath, OPTIX_VERSION, OPTIX_ABI_VERSION);
    std::printf("init ms: LoadLibrary(nvcuda) %.2f, cuInit %.2f, primary ctx retain+first use %.2f, stream %.2f, "
                "optixInit %.2f, optixDeviceContextCreate %.2f\n",
                tLoad, tInit, tCtx, tStream, tOptixInit, tOctx);
    std::printf("VRAM total %.0f MB, free after ctx %.0f MB, after OptiX ctx %.0f MB\n", totalMem / 1048576.0,
                freeCtx / 1048576.0, freeAfter / 1048576.0);
    std::printf("process start -> OptiX ready: %.1f ms\n", msSince(tProcess));

    if (a.count("--bench")) {
        std::vector<Case> cases;
        auto add = [&](const char* nm, int w, int h, auto&& tweak) {
            Case c; c.name = nm; c.w = w; c.h = h; tweak(c); cases.push_back(c);
        };
        auto none = [](Case&) {};
        const int iters = std::atoi(get("--iters", "20").c_str());
        // first case doubles as "first denoiser in the process"
        add("aov alb+nrm f32 (first in process)", 960, 540, none);
        add("aov alb+nrm f32", 960, 540, none);
        add("aov alb+nrm f32", 1920, 1080, none);
        add("aov alb+nrm f32", 3840, 2160, none);
        add("aov alb+nrm f16", 960, 540, [](Case& c) { c.half = true; });
        add("aov alb+nrm f16", 3840, 2160, [](Case& c) { c.half = true; });
        add("aov alb+nrm f32 pageable", 960, 540, [](Case& c) { c.pinned = false; });
        add("aov alb+nrm f32 pageable", 3840, 2160, [](Case& c) { c.pinned = false; });
        add("aov no guides", 960, 540, [](Case& c) { c.albedo = c.normal = false; });
        add("aov albedo only", 960, 540, [](Case& c) { c.normal = false; });
        add("aov alb+nrm alpha denoise", 960, 540, [](Case& c) { c.alpha = OPTIX_DENOISER_ALPHA_MODE_DENOISE; });
        add("aov alb+nrm no intensity", 960, 540, [](Case& c) { c.intensity = false; });
        add("aov alb+nrm 2 layers", 960, 540, [](Case& c) { c.layers = 2; });
        add("aov alb+nrm 4 layers", 960, 540, [](Case& c) { c.layers = 4; });
        add("aov alb+nrm 8 layers", 960, 540, [](Case& c) { c.layers = 8; });
        add("aov alb+nrm 4 layers", 3840, 2160, [](Case& c) { c.layers = 4; });
        add("aov alb+nrm tiled 1024", 3840, 2160, [](Case& c) { c.tile = 1024; });
        add("aov alb+nrm tiled 512", 3840, 2160, [](Case& c) { c.tile = 512; });
        add("hdr (deprecated->aov) alb+nrm", 960, 540, [](Case& c) { c.model = OPTIX_DENOISER_MODEL_KIND_HDR; });
        add("temporal_aov alb+nrm", 960, 540, [](Case& c) { c.model = OPTIX_DENOISER_MODEL_KIND_TEMPORAL_AOV; });
        add("temporal_aov alb+nrm", 3840, 2160, [](Case& c) { c.model = OPTIX_DENOISER_MODEL_KIND_TEMPORAL_AOV; });
        add("upscale2x alb+nrm (out 2x)", 960, 540, [](Case& c) { c.model = OPTIX_DENOISER_MODEL_KIND_UPSCALE2X; });
        add("upscale2x alb+nrm (out 2x)", 1920, 1080, [](Case& c) { c.model = OPTIX_DENOISER_MODEL_KIND_UPSCALE2X; });
        add("aov alb+nrm f32", 480, 270, none);
        add("aov alb+nrm f32", 240, 135, none);
        for (auto& c : cases) {
            c.iters = iters;
            Plane b = resample(input.beauty, c.w, c.h);
            Plane al = input.albedo.px.empty() ? Plane{} : resample(input.albedo, c.w, c.h);
            Plane nr = input.normal.px.empty() ? Plane{} : resample(input.normal, c.w, c.h);
            if (al.px.empty()) c.albedo = false;
            if (nr.px.empty()) c.normal = false;
            Result r = runCase(octx, stream, c, b, &al, &nr, nullptr);
            printResult(c, r);
        }
    } else {
        Case c;
        c.name = "denoise";
        c.w = input.beauty.w;
        c.h = input.beauty.h;
        c.iters = std::atoi(get("--iters", "3").c_str());
        std::string g = get("--guides", "albedo+normal");
        c.albedo = g.find("albedo") != std::string::npos && !input.albedo.px.empty();
        c.normal = g.find("normal") != std::string::npos && !input.normal.px.empty();
        c.alpha = get("--alpha", "copy") == "denoise" ? OPTIX_DENOISER_ALPHA_MODE_DENOISE : OPTIX_DENOISER_ALPHA_MODE_COPY;
        c.intensity = get("--intensity", "compute") == "compute";
        c.blend = float(std::atof(get("--blend", "0").c_str()));
        c.half = a.count("--half") > 0;
        c.tile = std::atoi(get("--tile", "0").c_str());
        c.overlap = std::atoi(get("--overlap", "-1").c_str());
        std::string m = get("--model", "aov");
        if (m == "upscale2x") c.model = OPTIX_DENOISER_MODEL_KIND_UPSCALE2X;
        else if (m == "temporal") c.model = OPTIX_DENOISER_MODEL_KIND_TEMPORAL_AOV;
        if (a.count("--normal-from")) {
            // take the normal guide from another render of the same view
            // (e.g. Karma's pixel-filtered N) to test guide anti-aliasing
            Input other;
            if (!loadInput(get("--normal-from", ""), get("--beauty", "C"), "-", get("--normal", "N"), other) ||
                other.normal.w != input.beauty.w || other.normal.h != input.beauty.h) {
                std::fprintf(stderr, "--normal-from: no matching normal plane\n");
                return 2;
            }
            input.normal = other.normal;
            c.normal = true;
        }
        Plane nrm = input.normal;
        std::string ns = get("--nspace", "world");
        if (a.count("--w2c")) {
            // 9 comma-separated numbers: the world-to-camera rotation, row-major,
            // row-vector convention (p_cam = p_world * M), as EXR's worldToCamera.
            float m[9] = {};
            if (std::sscanf(get("--w2c", "").c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f", &m[0], &m[1], &m[2], &m[3],
                            &m[4], &m[5], &m[6], &m[7], &m[8]) != 9) {
                std::fprintf(stderr, "--w2c expects 9 comma-separated numbers\n");
                return 2;
            }
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) input.w2c[r * 4 + k] = m[r * 3 + k];
            input.haveW2C = true;
        }
        if (c.normal && ns != "world") {
            if (!input.haveW2C) {
                std::fprintf(stderr, "no worldToCamera in the EXR header\n");
                return 2;
            }
            const float* M = input.w2c;
            for (size_t i = 0; i < size_t(nrm.w) * nrm.h; ++i) {
                float* n = &nrm.px[i * 3];
                float x = n[0], y = n[1], z = n[2];
                float cx = x * M[0] + y * M[4] + z * M[8];
                float cy = x * M[1] + y * M[5] + z * M[9];
                float cz = x * M[2] + y * M[6] + z * M[10];
                float len = std::sqrt(cx * cx + cy * cy + cz * cz);
                if (len > 0) { cx /= len; cy /= len; cz /= len; }
                n[0] = cx; n[1] = cy; n[2] = ns == "camera-flipz" ? -cz : cz;
            }
        }
        Plane out;
        Result r = runCase(octx, stream, c, input.beauty, &input.albedo, &nrm, &out);
        std::printf("guides albedo %d normal %d (%s) alpha %s intensity %d\n", c.albedo, c.normal, ns.c_str(),
                    c.alpha == OPTIX_DENOISER_ALPHA_MODE_DENOISE ? "denoise" : "copy", int(c.intensity));
        printResult(c, r);
        if (a.count("--out") && !writeRGBA(get("--out", ""), out)) {
            std::fprintf(stderr, "cannot write %s\n", get("--out", "").c_str());
            return 2;
        }
    }

    OX_CHECK(optixDeviceContextDestroy(octx));
    cu.p_cuStreamDestroy(stream);
    CUcontext popped = nullptr;
    cu.p_cuCtxPopCurrent(&popped);
    cu.p_cuDevicePrimaryCtxRelease(dev);
    std::printf("total %.1f ms\n", msSince(tProcess));
    std::fflush(stdout);
    return 0;
}
