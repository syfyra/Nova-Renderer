// A plain CUDA kernel with the work of __raygen__empty, to separate
// optixLaunch's overhead from a cuLaunchKernel (docs/research/m10/optix-accel.md).
extern "C" __global__ void oa_empty(unsigned *out, const unsigned *count, unsigned width) {
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned n = count ? *count : width;
    if (i < n)
        out[i] = i;
}
