#include "core/denoise/denoiser.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(NOVA_HAS_OPTIX) && defined(_WIN32)

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
#include <optix_denoiser_tiling.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>
// clang-format on

#include <algorithm>
#include <cstdio>

namespace nova {

namespace {

// The CUDA driver API, resolved from nvcuda.dll at run time. cuda.h maps
// some names to versioned exports (cuMemAlloc -> cuMemAlloc_v2); NOVA_CU_NAME
// expands the macro, so the versioned export is looked up.
#define NOVA_CU_NAME2(x) #x
#define NOVA_CU_NAME(x) NOVA_CU_NAME2(x)
#define NOVA_CU_FUNCS(X)                                                                                              \
    X(cuInit) X(cuDeviceGetCount) X(cuDeviceGet) X(cuDevicePrimaryCtxRetain) X(cuDevicePrimaryCtxRelease)            \
    X(cuCtxPushCurrent) X(cuCtxPopCurrent) X(cuStreamCreate) X(cuStreamDestroy) X(cuStreamSynchronize)              \
    X(cuMemAlloc) X(cuMemFree) X(cuMemAllocHost) X(cuMemFreeHost) X(cuMemcpyHtoDAsync) X(cuMemcpyDtoHAsync)          \
    X(cuMemGetInfo) X(cuGetErrorName)

struct CudaApi {
    HMODULE dll = nullptr;
#define NOVA_CU_DECLARE(fn) decltype(&fn) p_##fn = nullptr;
    NOVA_CU_FUNCS(NOVA_CU_DECLARE)
#undef NOVA_CU_DECLARE

    bool load(std::string &error) {
        dll = LoadLibraryA("nvcuda.dll");
        if (!dll) {
            error = "no NVIDIA driver (nvcuda.dll not found)";
            return false;
        }
#define NOVA_CU_LOAD(fn)                                                                                              \
    p_##fn = reinterpret_cast<decltype(&fn)>(GetProcAddress(dll, NOVA_CU_NAME(fn)));                                 \
    if (!p_##fn) {                                                                                                    \
        error = std::string("nvcuda.dll lacks ") + NOVA_CU_NAME(fn);                                                  \
        return false;                                                                                                 \
    }
        NOVA_CU_FUNCS(NOVA_CU_LOAD)
#undef NOVA_CU_LOAD
        return true;
    }
};

struct DeviceBuffer {
    CUdeviceptr ptr = 0;
    size_t size = 0;
};

OptixImage2D image(CUdeviceptr data, int width, int height, OptixPixelFormat format, unsigned pixelBytes) {
    OptixImage2D im{};
    im.data = data;
    im.width = unsigned(width);
    im.height = unsigned(height);
    im.pixelStrideInBytes = pixelBytes;
    im.rowStrideInBytes = unsigned(width) * pixelBytes;
    im.format = format;
    return im;
}

std::string environment(char const *name) {
    char *value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value)
        return {};
    std::string out(value);
    std::free(value);
    return out;
}

} // namespace

struct Denoiser::Impl {
    mutable std::mutex mutex;
    bool tried = false, ok = false;
    std::string message;
    int maxTile = 0;

    CudaApi cu;
    CUdevice device = 0;
    CUcontext context = nullptr;
    CUstream stream = nullptr;
    OptixDeviceContext optix = nullptr;

    // The denoiser of the current configuration and its buffers.
    OptixDenoiser denoiser = nullptr;
    int width = 0, height = 0, layerCount = 0;
    bool albedo = false, normal = false;
    unsigned tileW = 0, tileH = 0, overlap = 0;
    DeviceBuffer state, scratch, intensity, average, albedoBuf, normalBuf;
    std::vector<DeviceBuffer> inputs, outputs;
    void *pinned = nullptr; // host staging: every input, guide and output
    size_t pinnedSize = 0;

    bool check(CUresult r, char const *what) {
        if (r == CUDA_SUCCESS)
            return true;
        const char *name = "?";
        if (cu.p_cuGetErrorName)
            cu.p_cuGetErrorName(r, &name);
        message = std::string("CUDA ") + name + " in " + what;
        return false;
    }
    bool check(OptixResult r, char const *what) {
        if (r == OPTIX_SUCCESS)
            return true;
        message = std::string("OptiX ") + optixGetErrorName(r) + " in " + what;
        return false;
    }

    bool init() {
        if (tried)
            return ok;
        tried = true;
        if (const std::string off = environment("NOVA_DENOISE_DISABLE"); !off.empty() && off != "0") {
            message = "disabled by NOVA_DENOISE_DISABLE";
            return false;
        }
        if (!cu.load(message))
            return false;
        if (!check(cu.p_cuInit(0), "cuInit"))
            return false;
        int devices = 0;
        if (!check(cu.p_cuDeviceGetCount(&devices), "cuDeviceGetCount") || devices == 0) {
            message = "no CUDA device";
            return false;
        }
        int ordinal = 0; // Houdini's choice of the OptiX device
        if (const std::string env = environment("HOUDINI_NVIDIA_OPTIX_DEVICENUMBER"); !env.empty())
            ordinal = std::clamp(std::atoi(env.c_str()), 0, devices - 1);
        if (!check(cu.p_cuDeviceGet(&device, ordinal), "cuDeviceGet") ||
            !check(cu.p_cuDevicePrimaryCtxRetain(&context, device), "cuDevicePrimaryCtxRetain"))
            return false;
        if (!check(cu.p_cuCtxPushCurrent(context), "cuCtxPushCurrent"))
            return false;
        bool good = check(cu.p_cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
        if (good) {
            const OptixResult r = optixInit();
            good = r == OPTIX_SUCCESS;
            if (!good)
                message = r == OPTIX_ERROR_UNSUPPORTED_ABI_VERSION
                              ? "the NVIDIA driver is too old for OptiX 9.1 (ABI 118)"
                              : std::string("OptiX unavailable (") + optixGetErrorName(r) + ")";
        }
        if (good) {
            OptixDeviceContextOptions options{};
            good = check(optixDeviceContextCreate(context, &options, &optix), "optixDeviceContextCreate");
        }
        CUcontext popped = nullptr;
        cu.p_cuCtxPopCurrent(&popped);
        ok = good;
        return ok;
    }

    void release(DeviceBuffer &b) {
        if (b.ptr)
            cu.p_cuMemFree(b.ptr);
        b = DeviceBuffer{};
    }
    bool allocate(DeviceBuffer &b, size_t size) {
        release(b);
        if (size == 0)
            return true;
        if (!check(cu.p_cuMemAlloc(&b.ptr, size), "cuMemAlloc"))
            return false;
        b.size = size;
        return true;
    }
    void dropConfiguration() {
        if (denoiser)
            optixDenoiserDestroy(denoiser);
        denoiser = nullptr;
        for (DeviceBuffer *b : {&state, &scratch, &intensity, &average, &albedoBuf, &normalBuf})
            release(*b);
        for (DeviceBuffer &b : inputs)
            release(b);
        for (DeviceBuffer &b : outputs)
            release(b);
        inputs.clear();
        outputs.clear();
        if (pinned)
            cu.p_cuMemFreeHost(pinned);
        pinned = nullptr;
        pinnedSize = 0;
        width = height = layerCount = 0;
    }

    // The denoiser and buffers for a configuration; whole frames when the
    // free memory allows (with room to spare), else tiles.
    bool configure(int w, int h, int layers, bool withAlbedo, bool withNormal) {
        if (denoiser && w == width && h == height && layers == layerCount && withAlbedo == albedo &&
            withNormal == normal)
            return true;
        dropConfiguration();
        OptixDenoiserOptions options{};
        options.guideAlbedo = withAlbedo ? 1u : 0u;
        options.guideNormal = withNormal ? 1u : 0u;
        options.denoiseAlpha = OPTIX_DENOISER_ALPHA_MODE_COPY;
        if (!check(optixDenoiserCreate(optix, OPTIX_DENOISER_MODEL_KIND_AOV, &options, &denoiser),
                   "optixDenoiserCreate"))
            return false;
        const size_t pixels = size_t(w) * size_t(h);
        const size_t images = pixels * 16 * size_t(2 * layers) + pixels * 12 * size_t(int(withAlbedo) + int(withNormal));
        OptixDenoiserSizes sizes{};
        tileW = unsigned(w);
        tileH = unsigned(h);
        overlap = 0;
        if (!check(optixDenoiserComputeMemoryResources(denoiser, tileW, tileH, &sizes), "memory resources"))
            return false;
        size_t free = 0, total = 0;
        cu.p_cuMemGetInfo(&free, &total);
        const size_t whole = sizes.stateSizeInBytes + sizes.withoutOverlapScratchSizeInBytes + images;
        const int limit = maxTile > 0 ? maxTile : (whole > free / 10 * 8 ? 1024 : 0);
        if (limit > 0 && (w > limit || h > limit)) {
            tileW = unsigned(std::min(w, limit));
            tileH = unsigned(std::min(h, limit));
            if (!check(optixDenoiserComputeMemoryResources(denoiser, tileW, tileH, &sizes), "memory resources"))
                return false;
            overlap = sizes.overlapWindowSizeInPixels;
        }
        const size_t scratchSize = std::max({overlap ? sizes.withOverlapScratchSizeInBytes
                                                     : sizes.withoutOverlapScratchSizeInBytes,
                                             sizes.computeIntensitySizeInBytes, sizes.computeAverageColorSizeInBytes});
        inputs.resize(size_t(layers));
        outputs.resize(size_t(layers));
        bool good = allocate(state, sizes.stateSizeInBytes) && allocate(scratch, scratchSize) &&
                    allocate(intensity, sizeof(float)) && allocate(average, 3 * sizeof(float));
        for (int l = 0; good && l < layers; ++l)
            good = allocate(inputs[size_t(l)], pixels * 16) && allocate(outputs[size_t(l)], pixels * 16);
        if (good && withAlbedo)
            good = allocate(albedoBuf, pixels * 12);
        if (good && withNormal)
            good = allocate(normalBuf, pixels * 12);
        if (good) {
            pinnedSize = images;
            good = check(cu.p_cuMemAllocHost(&pinned, pinnedSize), "cuMemAllocHost");
        }
        if (good)
            good = check(optixDenoiserSetup(denoiser, stream, tileW + 2 * overlap, tileH + 2 * overlap, state.ptr,
                                            state.size, scratch.ptr, scratch.size),
                         "optixDenoiserSetup") &&
                   check(cu.p_cuStreamSynchronize(stream), "setup");
        if (!good) {
            dropConfiguration();
            return false;
        }
        width = w;
        height = h;
        layerCount = layers;
        albedo = withAlbedo;
        normal = withNormal;
        return true;
    }

    bool run(int w, int h, const Layer *layers, int count, const float *albedoIn, const float *normalIn) {
        if (!configure(w, h, count, albedoIn != nullptr, normalIn != nullptr))
            return false;
        const size_t pixels = size_t(w) * size_t(h), rgba = pixels * 16, rgb = pixels * 12;
        // Staging: the inputs, then the guides; the outputs reuse the inputs' part.
        auto *host = static_cast<unsigned char *>(pinned);
        size_t offset = 0;
        for (int l = 0; l < count; ++l, offset += rgba) {
            std::memcpy(host + offset, layers[l].input, rgba);
            if (!check(cu.p_cuMemcpyHtoDAsync(inputs[size_t(l)].ptr, host + offset, rgba, stream), "upload"))
                return false;
        }
        offset = rgba * size_t(2 * count);
        for (const float *guide : {albedoIn, normalIn}) {
            if (!guide)
                continue;
            std::memcpy(host + offset, guide, rgb);
            if (!check(cu.p_cuMemcpyHtoDAsync(guide == albedoIn ? albedoBuf.ptr : normalBuf.ptr, host + offset, rgb,
                                              stream),
                       "upload"))
                return false;
            offset += rgb;
        }

        OptixDenoiserGuideLayer guides{};
        if (albedoIn)
            guides.albedo = image(albedoBuf.ptr, w, h, OPTIX_PIXEL_FORMAT_FLOAT3, 12);
        if (normalIn)
            guides.normal = image(normalBuf.ptr, w, h, OPTIX_PIXEL_FORMAT_FLOAT3, 12);
        std::vector<OptixDenoiserLayer> optixLayers;
        optixLayers.resize(size_t(count));
        for (int l = 0; l < count; ++l) {
            optixLayers[size_t(l)].input = image(inputs[size_t(l)].ptr, w, h, OPTIX_PIXEL_FORMAT_FLOAT4, 16);
            optixLayers[size_t(l)].output = image(outputs[size_t(l)].ptr, w, h, OPTIX_PIXEL_FORMAT_FLOAT4, 16);
            optixLayers[size_t(l)].type = l == 0 ? OPTIX_DENOISER_AOV_TYPE_BEAUTY : OPTIX_DENOISER_AOV_TYPE_NONE;
        }
        // The exposure of the whole image (needed when tiling; cheap otherwise).
        OptixDenoiserParams params{};
        if (!check(optixDenoiserComputeIntensity(denoiser, stream, &optixLayers[0].input, intensity.ptr, scratch.ptr,
                                                 scratch.size),
                   "intensity") ||
            !check(optixDenoiserComputeAverageColor(denoiser, stream, &optixLayers[0].input, average.ptr, scratch.ptr,
                                                    scratch.size),
                   "average colour"))
            return false;
        params.hdrIntensity = intensity.ptr;
        params.hdrAverageColor = average.ptr;
        if (overlap > 0) {
            if (!check(optixUtilDenoiserInvokeTiled(denoiser, stream, &params, state.ptr, state.size, &guides,
                                                    optixLayers.data(), unsigned(count), scratch.ptr, scratch.size,
                                                    overlap, tileW, tileH),
                       "optixDenoiserInvoke (tiled)"))
                return false;
        } else if (!check(optixDenoiserInvoke(denoiser, stream, &params, state.ptr, state.size, &guides,
                                              optixLayers.data(), unsigned(count), 0, 0, scratch.ptr, scratch.size),
                          "optixDenoiserInvoke")) {
            return false;
        }
        offset = 0;
        for (int l = 0; l < count; ++l, offset += rgba)
            if (!check(cu.p_cuMemcpyDtoHAsync(host + rgba * size_t(count) + offset, outputs[size_t(l)].ptr, rgba,
                                              stream),
                       "download"))
                return false;
        if (!check(cu.p_cuStreamSynchronize(stream), "denoise"))
            return false;
        offset = 0;
        for (int l = 0; l < count; ++l, offset += rgba)
            std::memcpy(layers[l].output, host + rgba * size_t(count) + offset, rgba);
        return true;
    }

    ~Impl() {
        if (!ok)
            return;
        // The process may be exiting; leave the driver's own teardown alone
        // except for what Nova created.
        if (cu.p_cuCtxPushCurrent(context) == CUDA_SUCCESS) {
            dropConfiguration();
            if (optix)
                optixDeviceContextDestroy(optix);
            if (stream)
                cu.p_cuStreamDestroy(stream);
            CUcontext popped = nullptr;
            cu.p_cuCtxPopCurrent(&popped);
        }
        cu.p_cuDevicePrimaryCtxRelease(device);
    }
};

Denoiser::Denoiser() : m(std::make_unique<Impl>()) {}
Denoiser::~Denoiser() = default;

bool Denoiser::available() {
    std::lock_guard lock(m->mutex);
    return m->init();
}

bool Denoiser::denoise(int width, int height, const Layer *layers, int layerCount, const float *albedo,
                       const float *normal) {
    std::lock_guard lock(m->mutex);
    if (width <= 0 || height <= 0 || layerCount <= 0 || !m->init())
        return false;
    if (m->cu.p_cuCtxPushCurrent(m->context) != CUDA_SUCCESS) {
        m->message = "cannot make the CUDA context current";
        return false;
    }
    const bool good = m->run(width, height, layers, layerCount, albedo, normal);
    if (!good)
        m->dropConfiguration(); // a failed configuration (out of memory, ...) starts over next time
    CUcontext popped = nullptr;
    m->cu.p_cuCtxPopCurrent(&popped);
    return good;
}

std::string Denoiser::message() const {
    std::lock_guard lock(m->mutex);
    return m->message;
}

void Denoiser::setMaxTile(int pixels) {
    std::lock_guard lock(m->mutex);
    if (pixels != m->maxTile && m->ok && m->cu.p_cuCtxPushCurrent(m->context) == CUDA_SUCCESS) {
        m->dropConfiguration();
        CUcontext popped = nullptr;
        m->cu.p_cuCtxPopCurrent(&popped);
    }
    m->maxTile = pixels;
}

} // namespace nova

#else // no OptiX headers at build time, or not Windows yet

namespace nova {

struct Denoiser::Impl {};

Denoiser::Denoiser() : m(std::make_unique<Impl>()) {}
Denoiser::~Denoiser() = default;
bool Denoiser::available() { return false; }
bool Denoiser::denoise(int, int, const Layer *, int, const float *, const float *) { return false; }
std::string Denoiser::message() const { return "Nova was built without the OptiX headers"; }
void Denoiser::setMaxTile(int) {}

} // namespace nova

#endif

namespace nova {

Denoiser &sharedDenoiser() {
    // Never destroyed: the CUDA driver may already be unloading at exit.
    static Denoiser *denoiser = new Denoiser();
    return *denoiser;
}

} // namespace nova
