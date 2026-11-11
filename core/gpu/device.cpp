#include "core/gpu/device.h"

#include <optix.h>
#include <optix_stubs.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nova::gpu {

namespace {
// Device::setAllocationLimit: buffers above it fail (0: none).
size_t g_allocationLimit = 0;
// Device::setCapabilityOverride: the compute capability devices report (0: their own).
int g_capabilityOverride = 0;
} // namespace

void Device::setAllocationLimit(size_t bytes) { g_allocationLimit = bytes; }
void Device::setCapabilityOverride(int capability) { g_capabilityOverride = capability; }

DeviceBuffer &DeviceBuffer::operator=(DeviceBuffer &&o) noexcept {
    if (this != &o) {
        release();
        m_device = o.m_device;
        m_ptr = o.m_ptr;
        m_size = o.m_size;
        o.m_device = nullptr;
        o.m_ptr = 0;
        o.m_size = 0;
    }
    return *this;
}

bool DeviceBuffer::reserve(Device &device, size_t bytes) {
    if (m_ptr && m_size >= bytes && m_device == &device)
        return true;
    release();
    if (bytes == 0)
        return true;
    m_device = &device;
    Device::Scope scope(device);
    const CUresult r = g_allocationLimit != 0 && bytes > g_allocationLimit ? CUDA_ERROR_OUT_OF_MEMORY
                                                                            : device.cu().p_cuMemAlloc(&m_ptr, bytes);
    if (!device.check(r, "cuMemAlloc")) {
        m_ptr = 0;
        return false;
    }
    m_size = bytes;
    return true;
}

void DeviceBuffer::release() {
    if (m_ptr && m_device) {
        Device::Scope scope(*m_device);
        m_device->cu().p_cuMemFree(m_ptr);
    }
    m_ptr = 0;
    m_size = 0;
}

namespace {

// The architectures the kernels are built for, as compute capabilities
// (major * 10 + minor), and the oldest of them (core/CMakeLists.txt,
// NOVA_GPU_ARCHS; 75: Turing, 80: Ampere, 120: the RTX 50 series).
#if !defined(NOVA_GPU_MIN_ARCH)
#define NOVA_GPU_MIN_ARCH 75
#endif
#if !defined(NOVA_GPU_ARCHS)
#define NOVA_GPU_ARCHS "75,80,120"
#endif
// The architectures Nova XPU has run on (NOVA_GPU_ARCHS_TESTED): a GPU that
// runs the code of none of them is experimental.
#if !defined(NOVA_GPU_TESTED_ARCHS)
#define NOVA_GPU_TESTED_ARCHS "120"
#endif

// Whether a GPU of this compute capability runs code built for one of the
// architectures in a comma-separated list: code for X.y runs on every X.z
// with z >= y.
bool runsCodeOf(const char *archs, int capability) {
    for (const char *p = archs; *p;) {
        char *end = nullptr;
        const long arch = std::strtol(p, &end, 10);
        if (end == p)
            break;
        if (arch / 10 == capability / 10 && arch % 10 <= capability % 10)
            return true;
        p = *end ? end + 1 : end;
    }
    return false;
}

void optixLog(unsigned level, const char *tag, const char *message, void *) {
    if (level <= 2) // fatal, errors
        std::fprintf(stderr, "[Nova] OptiX %s: %s\n", tag, message);
}

// An environment variable's text (empty: unset).
std::string environment(const char *name) {
    char *value = nullptr;
    size_t size = 0;
    std::string text;
    if (_dupenv_s(&value, &size, name) == 0 && value)
        text = value;
    std::free(value);
    return text;
}

// A whole number in decimal digits and nothing else.
bool wholeNumber(const std::string &text, unsigned long long &value) {
    if (text.empty() || text.size() > 18 || text.find_first_not_of("0123456789") != std::string::npos)
        return false;
    value = std::strtoull(text.c_str(), nullptr, 10);
    return true;
}

std::string capabilityText(int capability) {
    return std::to_string(capability / 10) + "." + std::to_string(capability % 10);
}

} // namespace

bool Device::check(CUresult r, const char *what) {
    if (r == CUDA_SUCCESS)
        return true;
    if (m_error.empty()) {
        m_error = std::string(what) + ": " + cudaErrorText(r);
        m_outOfMemory = r == CUDA_ERROR_OUT_OF_MEMORY;
    }
    return false;
}

bool Device::check(OptixResult r, const char *what) {
    if (r == OPTIX_SUCCESS)
        return true;
    if (m_error.empty())
        m_error = std::string(what) + ": " + optixGetErrorName(r);
    return false;
}

bool Device::init(std::string &error) {
    // NOVA_XPU_DISABLE (not empty, not 0): as without a usable device, so
    // Nova XPU renders on the CPU (docs/design/M10.md D15).
    {
        const std::string off = environment("NOVA_XPU_DISABLE");
        if (!off.empty() && off != "0") {
            error = "disabled by NOVA_XPU_DISABLE";
            return false;
        }
    }
    m_cu = cudaApi(&error);
    if (!m_cu)
        return false;
    int count = 0;
    if (!check(m_cu->p_cuInit(0), "cuInit") || !check(m_cu->p_cuDeviceGetCount(&count), "cuDeviceGetCount")) {
        error = m_error;
        return false;
    }
    if (count < 1) {
        error = "no CUDA device";
        return false;
    }
    // The driver lists its devices fastest first (unless CUDA_DEVICE_ORDER
    // says otherwise), so the first is the default; NOVA_XPU_DEVICE names
    // another by its index in that list.
    unsigned long long index = 0;
    {
        const std::string wanted = environment("NOVA_XPU_DEVICE");
        if (!wanted.empty() && (!wholeNumber(wanted, index) || index >= static_cast<unsigned long long>(count))) {
            error = "NOVA_XPU_DEVICE=" + wanted + " names none of the driver's " + std::to_string(count) + " device(s)";
            return false;
        }
    }
    char name[256] = {};
    int major = 0, minor = 0;
    if (!check(m_cu->p_cuDeviceGet(&m_dev, int(index)), "cuDeviceGet") ||
        !check(m_cu->p_cuDeviceGetName(name, int(sizeof(name)), m_dev), "cuDeviceGetName") ||
        !check(m_cu->p_cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, m_dev),
               "cuDeviceGetAttribute") ||
        !check(m_cu->p_cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, m_dev),
               "cuDeviceGetAttribute") ||
        !check(m_cu->p_cuDeviceGetAttribute(&m_processors, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, m_dev),
               "cuDeviceGetAttribute") ||
        !check(m_cu->p_cuDeviceTotalMem(&m_totalMemory, m_dev), "cuDeviceTotalMem")) {
        error = m_error;
        return false;
    }
    m_name = name;
    m_capability = g_capabilityOverride ? g_capabilityOverride : major * 10 + minor;
    switch (support(m_capability)) {
    case Support::TooOld:
        // The kernels exist as code for some architectures and as PTX for
        // the oldest, which the driver compiles for any newer one: an older
        // GPU has nothing to run (and the drivers OptiX 9.1 needs do not
        // support one).
        error = description() + " is older than Nova XPU supports: it needs compute capability " +
                capabilityText(NOVA_GPU_MIN_ARCH) + " or newer" +
                (NOVA_GPU_MIN_ARCH == 75 ? " (GeForce RTX 20 and GTX 16 series, Quadro RTX, and later)" : "");
        return false;
    case Support::Experimental: {
        // Built for this architecture but never run on it: only on request
        // (NOVA_XPU_EXPERIMENTAL_GPUS not empty and not 0; the installer's
        // option writes it into Houdini's package file).
        const std::string on = environment("NOVA_XPU_EXPERIMENTAL_GPUS");
        if (on.empty() || on == "0") {
            error = description() +
                    ": Nova XPU is built for this GPU's architecture but has not been tested on it. To try it, turn on "
                    "the experimental GPU support in the Nova installer or set NOVA_XPU_EXPERIMENTAL_GPUS=1";
            return false;
        }
        m_experimental = true;
        break;
    }
    case Support::Tested:
        break;
    }
    {
        unsigned long long megabytes = 0;
        if (wholeNumber(environment("NOVA_XPU_MEMORY_MB"), megabytes) && megabytes > 0)
            m_memoryCap = size_t(megabytes) << 20;
    }
    // Keep the local memory of the largest kernel stack instead of freeing
    // it after each launch (the shade kernel's stack made every pass 20 ms
    // slower). Fails harmlessly when the primary context is active already.
    m_cu->p_cuDevicePrimaryCtxSetFlags(m_dev, CU_CTX_LMEM_RESIZE_TO_MAX);
    if (!check(m_cu->p_cuDevicePrimaryCtxRetain(&m_context, m_dev), "cuDevicePrimaryCtxRetain")) {
        error = m_error;
        return false;
    }
    Scope scope(*this);
    if (!check(m_cu->p_cuStreamCreate(&m_stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate")) {
        error = m_error;
        return false;
    }
    {
        size_t free = 0, total = 0;
        if (m_cu->p_cuMemGetInfo(&free, &total) == CUDA_SUCCESS)
            m_freeAtStart = free;
    }
    const OptixResult r = optixInit();
    if (r != OPTIX_SUCCESS) {
        error = std::string("optixInit: ") + optixGetErrorName(r) +
                (r == OPTIX_ERROR_UNSUPPORTED_ABI_VERSION
                     ? " (the NVIDIA driver is too old: Nova XPU needs driver 590 or newer, for OptiX 9.1)"
                     : "");
        return false;
    }
    OptixDeviceContextOptions o{};
    o.logCallbackFunction = &optixLog;
    o.logCallbackLevel = 2;
    if (!check(optixDeviceContextCreate(m_context, &o, &m_optix), "optixDeviceContextCreate")) {
        error = m_error;
        m_optix = nullptr;
        return false;
    }
    return true;
}

std::string Device::description() const {
    char memory[32];
    std::snprintf(memory, sizeof(memory), "%.0f GB", double(m_totalMemory) / double(size_t(1) << 30));
    return m_name + " (compute capability " + capabilityText(m_capability) + ", " + memory + ")";
}

bool Device::hasKernelCode() const { return runsCodeOf(NOVA_GPU_ARCHS, m_capability); }

Device::Support Device::support(int capability) {
    if (capability < NOVA_GPU_MIN_ARCH)
        return Support::TooOld;
    return runsCodeOf(NOVA_GPU_TESTED_ARCHS, capability) ? Support::Tested : Support::Experimental;
}

size_t Device::freeMemory() const {
    Scope scope(*this);
    size_t free = 0, total = 0;
    if (!m_cu || m_cu->p_cuMemGetInfo(&free, &total) != CUDA_SUCCESS)
        return 0;
    if (m_memoryCap) {
        // As a device that had only the cap free when it was opened: what
        // went since (this process's allocations, and others') comes off it.
        const size_t used = m_freeAtStart > free ? m_freeAtStart - free : 0;
        free = std::min(free, m_memoryCap > used ? m_memoryCap - used : size_t(0));
    }
    return free;
}

Device::~Device() {
    if (!m_cu)
        return;
    if (m_context) {
        Scope scope(*this);
        if (m_optix)
            optixDeviceContextDestroy(m_optix);
        if (m_stream)
            m_cu->p_cuStreamDestroy(m_stream);
    }
    if (m_context)
        m_cu->p_cuDevicePrimaryCtxRelease(m_dev);
}

Device::Scope::Scope(const Device &d) : m_device(d) {
    if (d.m_cu && d.m_context)
        m_pushed = d.m_cu->p_cuCtxPushCurrent(d.m_context) == CUDA_SUCCESS;
}

Device::Scope::~Scope() {
    CUcontext popped = nullptr;
    if (m_pushed)
        m_device.m_cu->p_cuCtxPopCurrent(&popped);
}

bool Device::upload(CUdeviceptr dst, const void *src, size_t bytes) {
    // Pageable host memory: the copy returns once staged; the stream orders it.
    Scope scope(*this);
    return bytes == 0 || check(m_cu->p_cuMemcpyHtoDAsync(dst, src, bytes, m_stream), "cuMemcpyHtoDAsync");
}

bool Device::download(void *dst, CUdeviceptr src, size_t bytes) {
    Scope scope(*this);
    return bytes == 0 || (check(m_cu->p_cuMemcpyDtoHAsync(dst, src, bytes, m_stream), "cuMemcpyDtoHAsync") && sync());
}

bool Device::sync() {
    Scope scope(*this);
    return check(m_cu->p_cuStreamSynchronize(m_stream), "cuStreamSynchronize");
}

} // namespace nova::gpu
