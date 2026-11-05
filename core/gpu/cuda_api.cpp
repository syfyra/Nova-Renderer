#include "core/gpu/cuda_api.h"

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>

namespace nova::gpu {

namespace {

struct Loaded {
    CudaApi api;
    bool ok = false;
    std::string error;
};

Loaded &loaded() {
    static Loaded l;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE dll = LoadLibraryA("nvcuda.dll");
        if (!dll) {
            l.error = "no NVIDIA driver (nvcuda.dll not found)";
            return;
        }
#define NOVA_GPU_CU_LOAD(fn)                                                                                           \
    l.api.p_##fn = reinterpret_cast<decltype(&fn)>(GetProcAddress(dll, NOVA_GPU_CU_NAME(fn)));                         \
    if (!l.api.p_##fn) {                                                                                               \
        l.error = std::string("nvcuda.dll lacks ") + NOVA_GPU_CU_NAME(fn);                                            \
        return;                                                                                                        \
    }
        NOVA_GPU_CU_FUNCS(NOVA_GPU_CU_LOAD)
#undef NOVA_GPU_CU_LOAD
        l.ok = true;
    });
    return l;
}

} // namespace

const CudaApi *cudaApi(std::string *error) {
    Loaded &l = loaded();
    if (!l.ok && error)
        *error = l.error;
    return l.ok ? &l.api : nullptr;
}

std::string cudaErrorText(CUresult r) {
    const CudaApi *cu = cudaApi();
    const char *name = "?", *text = "";
    if (cu) {
        cu->p_cuGetErrorName(r, &name);
        cu->p_cuGetErrorString(r, &text);
    }
    return std::string(name) + ": " + text;
}

} // namespace nova::gpu
