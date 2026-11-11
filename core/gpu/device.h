#pragma once

// The GPU device of the backend (docs/design/M10.md D2, D4, section 8): the
// CUDA primary context of one device (the driver's first, its fastest, or
// the one NOVA_XPU_DEVICE names), one non-blocking stream that every copy
// and launch goes through (research oa 9.4: an unordered copy corrupted
// builds), the OptiX context, and device buffers.

#include "core/gpu/cuda_api.h"

#include <optix_types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nova::gpu {

class Device;

// A device allocation owned by its Device; moves only.
class DeviceBuffer {
public:
    DeviceBuffer() = default;
    ~DeviceBuffer() { release(); }
    DeviceBuffer(DeviceBuffer &&o) noexcept { *this = static_cast<DeviceBuffer &&>(o); }
    DeviceBuffer &operator=(DeviceBuffer &&o) noexcept;
    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;

    // (Re)allocates `bytes` (keeps the allocation if it is large enough).
    bool reserve(Device &device, size_t bytes);
    void release();
    CUdeviceptr ptr() const { return m_ptr; }
    size_t size() const { return m_size; }
    template <class T> T *as() const { return reinterpret_cast<T *>(m_ptr); }

private:
    Device *m_device = nullptr;
    CUdeviceptr m_ptr = 0;
    size_t m_size = 0;
};

class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    // Opens the device with OptiX; false with the reason in `error` (no
    // driver, no device, a GPU older than the kernels are built for, a
    // driver older than OptiX needs).
    bool init(std::string &error);
    bool ok() const { return m_optix != nullptr; }
    const std::string &name() const { return m_name; }
    // "name (compute capability M.m, N GB)" for messages.
    std::string description() const;
    // The compute capability as major * 10 + minor (75: Turing, 120: RTX 50).
    int computeCapability() const { return m_capability; }
    // What Nova XPU does with a GPU of a compute capability: nothing (older
    // than the kernels are built for), render on request only (built for its
    // architecture but never run on it: NOVA_XPU_EXPERIMENTAL_GPUS), or
    // render (an architecture it has been tested on).
    enum class Support { TooOld, Experimental, Tested };
    static Support support(int capability);
    // The device is of an architecture Nova XPU has not been tested on.
    bool experimental() const { return m_experimental; }
    // For tests: every device opened from now on reports this compute
    // capability (0: its own), so the branches for other GPUs run here.
    static void setCapabilityOverride(int capability);
    size_t totalMemory() const { return m_totalMemory; }
    // The device's streaming multiprocessors (the kernels' stacks grow with them).
    int processors() const { return m_processors; }
    // Whether the kernels' fatbin holds code for this device's architecture;
    // without it the driver compiles their PTX when the module loads, which
    // takes minutes the first time (the driver keeps the result).
    bool hasKernelCode() const;
    // The device memory free now: what the texture budget and the waves are
    // planned with. With NOVA_XPU_MEMORY_MB set, as on a device that had
    // only that much free when it was opened (a smaller card, for tests and
    // to leave memory to other programs).
    size_t freeMemory() const;

    const CudaApi &cu() const { return *m_cu; }
    CUstream stream() const { return m_stream; }
    OptixDeviceContext optix() const { return m_optix; }

    // Makes the context current on this thread for the lifetime of the scope
    // (nothing without one: destructors of a device that never initialized).
    class Scope {
    public:
        explicit Scope(const Device &d);
        ~Scope();
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        const Device &m_device;
        bool m_pushed = false;
    };

    // Stream-ordered copies; download() and sync() wait for the stream.
    bool upload(CUdeviceptr dst, const void *src, size_t bytes);
    bool download(void *dst, CUdeviceptr src, size_t bytes);
    bool sync();
    // A buffer filled with `count` values of T (empty: none allocated).
    template <class T> bool upload(DeviceBuffer &buf, const T *data, size_t count) {
        if (!count)
            return true;
        return buf.reserve(*this, count * sizeof(T)) && upload(buf.ptr(), data, count * sizeof(T));
    }
    template <class T> bool upload(DeviceBuffer &buf, const std::vector<T> &v) { return upload(buf, v.data(), v.size()); }

    // Records the first failure (CUDA or OptiX) and returns false for it.
    bool check(CUresult r, const char *what);
    bool check(OptixResult r, const char *what);
    const std::string &lastError() const { return m_error; }
    // For tests: a buffer above `bytes` fails as it does when the device's
    // memory is exhausted (0: no limit), for every device of the process.
    static void setAllocationLimit(size_t bytes);
    // The recorded failure is the device running out of memory.
    bool outOfMemory() const { return m_outOfMemory; }
    // Forgets a recorded failure the caller recovered from (an allocation
    // it repeats smaller).
    void forgetError() {
        m_error.clear();
        m_outOfMemory = false;
    }

private:
    const CudaApi *m_cu = nullptr;
    CUdevice m_dev = 0;
    int m_capability = 0, m_processors = 0;
    size_t m_totalMemory = 0;
    // NOVA_XPU_MEMORY_MB in bytes (0: none) and the memory free when the
    // device was opened, which the cap stands in for.
    size_t m_memoryCap = 0, m_freeAtStart = 0;
    CUcontext m_context = nullptr;
    CUstream m_stream = nullptr;
    OptixDeviceContext m_optix = nullptr;
    std::string m_name, m_error;
    bool m_outOfMemory = false, m_experimental = false;
};

} // namespace nova::gpu
