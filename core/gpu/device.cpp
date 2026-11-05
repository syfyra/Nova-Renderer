#include "core/gpu/device.h"

#include <optix.h>
#include <optix_stubs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nova::gpu {

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
    if (!device.check(device.cu().p_cuMemAlloc(&m_ptr, bytes), "cuMemAlloc")) {
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

void optixLog(unsigned level, const char *tag, const char *message, void *) {
    if (level <= 2) // fatal, errors
        std::fprintf(stderr, "[Nova] OptiX %s: %s\n", tag, message);
}

} // namespace

bool Device::check(CUresult r, const char *what) {
    if (r == CUDA_SUCCESS)
        return true;
    if (m_error.empty())
        m_error = std::string(what) + ": " + cudaErrorText(r);
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
        char *off = nullptr;
        size_t size = 0;
        const bool disabled = _dupenv_s(&off, &size, "NOVA_XPU_DISABLE") == 0 && off && *off && std::strcmp(off, "0") != 0;
        std::free(off);
        if (disabled) {
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
    char name[256] = {};
    if (!check(m_cu->p_cuDeviceGet(&m_dev, 0), "cuDeviceGet") ||
        !check(m_cu->p_cuDeviceGetName(name, int(sizeof(name)), m_dev), "cuDeviceGetName")) {
        error = m_error;
        return false;
    }
    // Keep the local memory of the largest kernel stack instead of freeing
    // it after each launch (the shade kernel's stack made every pass 20 ms
    // slower). Fails harmlessly when the primary context is active already.
    m_cu->p_cuDevicePrimaryCtxSetFlags(m_dev, CU_CTX_LMEM_RESIZE_TO_MAX);
    if (!check(m_cu->p_cuDevicePrimaryCtxRetain(&m_context, m_dev), "cuDevicePrimaryCtxRetain")) {
        error = m_error;
        return false;
    }
    m_name = name;
    Scope scope(*this);
    if (!check(m_cu->p_cuStreamCreate(&m_stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate")) {
        error = m_error;
        return false;
    }
    const OptixResult r = optixInit();
    if (r != OPTIX_SUCCESS) {
        error = std::string("optixInit: ") + optixGetErrorName(r) +
                (r == OPTIX_ERROR_UNSUPPORTED_ABI_VERSION ? " (the NVIDIA driver is too old for OptiX 9.1)" : "");
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
