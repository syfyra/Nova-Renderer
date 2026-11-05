#pragma once

// The GPU device of the backend (docs/design/M10.md D2, D4): the CUDA primary
// context of device 0, one non-blocking stream that every copy and launch
// goes through (research oa 9.4: an unordered copy corrupted builds), the
// OptiX context, and device buffers.

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

    // Opens device 0 with OptiX; false with the reason in `error`.
    bool init(std::string &error);
    bool ok() const { return m_optix != nullptr; }
    const std::string &name() const { return m_name; }

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

private:
    const CudaApi *m_cu = nullptr;
    CUdevice m_dev = 0;
    CUcontext m_context = nullptr;
    CUstream m_stream = nullptr;
    OptixDeviceContext m_optix = nullptr;
    std::string m_name, m_error;
};

} // namespace nova::gpu
