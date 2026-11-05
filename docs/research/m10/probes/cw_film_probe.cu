// M10 research probe (docs/research/m10/cuda-wavefront.md, section 6.3):
// accumulating a wave's path radiance into the film on the GPU. 1920 x 1080
// pixels; each path adds RGB + count, and with light path expressions
// `outputs` more RGB sums (output-major planes, as Film::lpeSum).
//
//   F1 owned: a pixel's samples are consecutive path slots handled by one
//      thread (Nova's CPU layout: slot = pixel * spp + j), plain += (deterministic)
//   F2 atomicAdd (RED.ADD.F32) per path, paths in pixel order
//   F3 atomicAdd per path, paths shuffled (after a sort or compaction)
//   F4 atomicAdd with spp paths of one pixel in one wave (contention)
//
// Build: cw_build.bat <out> film. Run: cw_film_probe.exe

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

#define CK(x)                                                                                                          \
    do {                                                                                                               \
        cudaError_t e_ = (x);                                                                                          \
        if (e_ != cudaSuccess) {                                                                                       \
            std::printf("CUDA error %s at %s:%d\n", cudaGetErrorString(e_), __FILE__, __LINE__);                    \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

constexpr int kPixels = 1920 * 1080;

// Path radiance: 3 floats per path per plane (plane 0 beauty, 1.. LPE outputs).
__global__ void owned(int pixels, int spp, int planes, const float *L, float *film, float *count) {
    const int p = blockIdx.x * blockDim.x + threadIdx.x;
    if (p >= pixels)
        return;
    const int paths = pixels * spp;
    for (int k = 0; k < planes; ++k) {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        for (int j = 0; j < spp; ++j) {
            const int s = p * spp + j;
            r += L[(size_t(k) * 3 + 0) * paths + s];
            g += L[(size_t(k) * 3 + 1) * paths + s];
            b += L[(size_t(k) * 3 + 2) * paths + s];
        }
        float *f = film + size_t(k) * 3 * pixels;
        f[p] += r;
        f[pixels + p] += g;
        f[2 * pixels + p] += b;
    }
    count[p] += float(spp);
}

__global__ void atomics(int paths, int pixels, int spp, int planes, const int *order, const float *L, float *film,
                        float *count) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= paths)
        return;
    const int s = order ? order[i] : i;
    const int p = s / spp;
    for (int k = 0; k < planes; ++k) {
        float *f = film + size_t(k) * 3 * pixels;
        atomicAdd(&f[p], L[(size_t(k) * 3 + 0) * paths + s]);
        atomicAdd(&f[pixels + p], L[(size_t(k) * 3 + 1) * paths + s]);
        atomicAdd(&f[2 * pixels + p], L[(size_t(k) * 3 + 2) * paths + s]);
    }
    atomicAdd(&count[p], 1.0f);
}

static float median(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <typename F>
static float timeIt(F &&f, int reps = 15) {
    cudaEvent_t a, b;
    CK(cudaEventCreate(&a));
    CK(cudaEventCreate(&b));
    f();
    CK(cudaDeviceSynchronize());
    std::vector<float> ms;
    for (int r = 0; r < reps; ++r) {
        CK(cudaEventRecord(a));
        f();
        CK(cudaEventRecord(b));
        CK(cudaEventSynchronize(b));
        float t;
        CK(cudaEventElapsedTime(&t, a, b));
        ms.push_back(t);
    }
    CK(cudaGetLastError());
    return median(ms);
}

int main() {
    cudaDeviceProp prop{};
    CK(cudaGetDeviceProperties(&prop, 0));
    std::printf("device: %s; %d pixels\n", prop.name, kPixels);
    std::printf("%-6s %-4s %10s %10s %10s %10s\n", "planes", "spp", "owned", "atomic", "atomic-shuf", "(ms)");
    for (int planes : {1, 9})
        for (int spp : {1, 4}) {
            const int paths = kPixels * spp;
            float *L, *film, *count;
            int *order;
            CK(cudaMalloc(&L, size_t(planes) * 3 * paths * 4));
            CK(cudaMalloc(&film, size_t(planes) * 3 * kPixels * 4));
            CK(cudaMalloc(&count, size_t(kPixels) * 4));
            CK(cudaMalloc(&order, size_t(paths) * 4));
            CK(cudaMemset(L, 0, size_t(planes) * 3 * paths * 4));
            std::vector<int> h(paths);
            std::iota(h.begin(), h.end(), 0);
            std::mt19937 rng(1);
            std::shuffle(h.begin(), h.end(), rng);
            CK(cudaMemcpy(order, h.data(), size_t(paths) * 4, cudaMemcpyHostToDevice));
            const int block = 256;
            const float o = timeIt([&] { owned<<<(kPixels + block - 1) / block, block>>>(kPixels, spp, planes, L, film, count); });
            const float a = timeIt([&] {
                atomics<<<(paths + block - 1) / block, block>>>(paths, kPixels, spp, planes, nullptr, L, film, count);
            });
            const float s = timeIt([&] {
                atomics<<<(paths + block - 1) / block, block>>>(paths, kPixels, spp, planes, order, L, film, count);
            });
            std::printf("%-6d %-4d %10.4f %10.4f %10.4f\n", planes, spp, o, a, s);
            cudaFree(L);
            cudaFree(film);
            cudaFree(count);
            cudaFree(order);
        }
    std::printf("\ndone\n");
    return 0;
}
