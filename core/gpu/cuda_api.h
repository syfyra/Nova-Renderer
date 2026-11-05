#pragma once

// The CUDA driver API of the GPU backend (docs/design/M10.md D2), loaded from
// nvcuda.dll at run time as the OptiX denoiser does (M7), so Nova loads on
// machines without an NVIDIA driver.

#include <cuda.h>

#include <string>

namespace nova::gpu {

// The CUDA headers map some names to versioned exports (cuMemAlloc ->
// cuMemAlloc_v2): stringize after expansion.
#define NOVA_GPU_CU_NAME2(x) #x
#define NOVA_GPU_CU_NAME(x) NOVA_GPU_CU_NAME2(x)
#define NOVA_GPU_CU_FUNCS(X)                                                                                           \
    X(cuInit) X(cuDriverGetVersion) X(cuDeviceGetCount) X(cuDeviceGet) X(cuDeviceGetName) X(cuDeviceGetAttribute)     \
    X(cuDeviceTotalMem) X(cuDevicePrimaryCtxRetain) X(cuDevicePrimaryCtxRelease) X(cuCtxPushCurrent)                  \
    X(cuCtxPopCurrent) X(cuCtxSetCurrent) X(cuStreamCreate) X(cuStreamDestroy) X(cuStreamSynchronize) X(cuMemAlloc)  \
    X(cuMemFree) X(cuMemAllocHost) X(cuMemFreeHost) X(cuMemcpyHtoDAsync) X(cuMemcpyDtoHAsync) X(cuMemsetD8Async)      \
    X(cuMemsetD32Async) X(cuMemGetInfo) X(cuModuleLoadData) X(cuModuleUnload) X(cuModuleGetFunction)                  \
    X(cuModuleGetGlobal) X(cuLaunchKernel) X(cuGetErrorName) X(cuGetErrorString) X(cuFuncGetAttribute)          \
    X(cuMipmappedArrayCreate) X(cuMipmappedArrayGetLevel) X(cuMipmappedArrayDestroy) X(cuMemcpy2DAsync)               \
    X(cuTexObjectCreate) X(cuTexObjectDestroy) X(cuDevicePrimaryCtxSetFlags)

struct CudaApi {
#define NOVA_GPU_CU_DECLARE(fn) decltype(&fn) p_##fn = nullptr;
    NOVA_GPU_CU_FUNCS(NOVA_GPU_CU_DECLARE)
#undef NOVA_GPU_CU_DECLARE
};

// The driver API, loaded once (null with the reason in `error` if there is
// no usable driver).
const CudaApi *cudaApi(std::string *error = nullptr);

// "name: description" of a CUDA error.
std::string cudaErrorText(CUresult r);

} // namespace nova::gpu
