// M10 research probe (docs/research/m10/cuda-wavefront.md, section 2.4):
// stream compaction of a wavefront queue on the GPU. n = 1920 x 1080 =
// 2,073,600 elements (a 1080p wave at 1 spp), survival rates 10 / 50 / 90 %.
//
//   indices only:
//     1 atomicAdd per surviving element (nvcc may aggregate it per warp)
//     2 warp-aggregated atomic (ballot + popc + one atomicAdd per warp)
//     3 block-aggregated atomic (one atomicAdd per block, order kept in a block)
//     4 CUB DeviceSelect::Flagged on a byte flag array (stable)
//     5 CUB DeviceSelect::If with a predicate (stable, no flag array)
//     6 CUB DeviceScan::ExclusiveSum + scatter kernel (stable, two passes)
//     7 thrust::copy_if (stable)
//   ray payload (8 floats SoA = 32 bytes per ray):
//     8 fused push: the producing kernel appends the ray at a warp-aggregated slot
//     9 fixed slots + flags, then CUB DeviceSelect::Flagged on indices and a gather
//    10 fixed slots, no compaction (the copy alone: the bandwidth floor)
//
// Build: cw_build.bat. Run: cw_compact_probe.exe

#include <cub/cub.cuh>
#include <cuda_runtime.h>
#include <thrust/copy.h>
#include <thrust/execution_policy.h>
#include <thrust/iterator/counting_iterator.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CK(x)                                                                                                          \
    do {                                                                                                               \
        cudaError_t e_ = (x);                                                                                          \
        if (e_ != cudaSuccess) {                                                                                       \
            std::printf("CUDA error %s at %s:%d\n", cudaGetErrorString(e_), __FILE__, __LINE__);                    \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

__host__ __device__ inline unsigned hash32(unsigned x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

struct Survives {
    unsigned threshold;
    __host__ __device__ bool operator()(unsigned i) const { return hash32(i) < threshold; }
};

__global__ void makeFlags(int n, unsigned threshold, unsigned char *flags, unsigned *flags32) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        const unsigned f = hash32(unsigned(i)) < threshold;
        flags[i] = (unsigned char)f;
        flags32[i] = f;
    }
}

__global__ void naiveAtomic(int n, unsigned threshold, unsigned *out, unsigned *counter) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n && hash32(unsigned(i)) < threshold)
        out[atomicAdd(counter, 1u)] = unsigned(i);
}

__device__ __forceinline__ unsigned warpAppend(bool keep, unsigned *counter) {
    const unsigned mask = __ballot_sync(0xFFFFFFFFu, keep);
    const int laneId = threadIdx.x & 31;
    const int leader = __ffs(mask) - 1;
    unsigned base = 0;
    if (laneId == leader)
        base = atomicAdd(counter, unsigned(__popc(mask)));
    base = __shfl_sync(0xFFFFFFFFu, base, leader < 0 ? 0 : leader);
    return base + unsigned(__popc(mask & ((1u << laneId) - 1u)));
}

__global__ void warpAtomic(int n, unsigned threshold, unsigned *out, unsigned *counter) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const bool keep = i < n && hash32(unsigned(i)) < threshold;
    const unsigned slot = warpAppend(keep, counter);
    if (keep)
        out[slot] = unsigned(i);
}

template <int Block>
__global__ void blockAtomic(int n, unsigned threshold, unsigned *out, unsigned *counter) {
    using Scan = cub::BlockScan<unsigned, Block>;
    __shared__ typename Scan::TempStorage temp;
    __shared__ unsigned base;
    const int i = blockIdx.x * Block + threadIdx.x;
    const unsigned keep = (i < n && hash32(unsigned(i)) < threshold) ? 1u : 0u;
    unsigned offset, total;
    Scan(temp).ExclusiveSum(keep, offset, total);
    if (threadIdx.x == 0)
        base = atomicAdd(counter, total);
    __syncthreads();
    if (keep)
        out[base + offset] = unsigned(i);
}

__global__ void scatterScan(int n, const unsigned *flags32, const unsigned *offsets, unsigned *out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n && flags32[i])
        out[offsets[i]] = unsigned(i);
}

// Ray payload: 8 floats SoA.
struct Rays {
    float *f[8];
};

__device__ __forceinline__ void makeRay(unsigned i, float v[8]) {
    const float x = float(i & 2047u) * (1.0f / 2048.0f);
    v[0] = x;
    v[1] = x * 0.5f;
    v[2] = 1.0f - x;
    v[3] = 0.577f;
    v[4] = 0.577f;
    v[5] = 0.577f;
    v[6] = 1e30f;
    v[7] = __uint_as_float(i);
}

__global__ void pushFused(int n, unsigned threshold, Rays out, unsigned *counter) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const bool keep = i < n && hash32(unsigned(i)) < threshold;
    const unsigned slot = warpAppend(keep, counter);
    if (keep) {
        float v[8];
        makeRay(unsigned(i), v);
        for (int k = 0; k < 8; ++k)
            out.f[k][slot] = v[k];
    }
}

__global__ void writeFixed(int n, unsigned threshold, Rays out, unsigned char *flags) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    const bool keep = hash32(unsigned(i)) < threshold;
    flags[i] = keep;
    if (keep) {
        float v[8];
        makeRay(unsigned(i), v);
        for (int k = 0; k < 8; ++k)
            out.f[k][i] = v[k];
    }
}

__global__ void gatherRays(const unsigned *count, const unsigned *idx, Rays in, Rays out) {
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= *count)
        return;
    const unsigned j = idx[i];
    for (int k = 0; k < 8; ++k)
        out.f[k][i] = in.f[k][j];
}

static float median(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <typename F>
static float timeIt(F &&f, int reps = 21) {
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
    std::printf("device: %s, %d SMs; CUB %d\n", prop.name, prop.multiProcessorCount, CUB_VERSION);
    const int n = 1920 * 1080;
    const int block = 256, grid = (n + block - 1) / block;
    unsigned *dOut, *dCounter, *dFlags32, *dOffsets, *dNum;
    unsigned char *dFlags;
    CK(cudaMalloc(&dOut, size_t(n) * 4));
    CK(cudaMalloc(&dCounter, 4));
    CK(cudaMalloc(&dNum, 4));
    CK(cudaMalloc(&dFlags32, size_t(n) * 4));
    CK(cudaMalloc(&dOffsets, size_t(n) * 4));
    CK(cudaMalloc(&dFlags, size_t(n)));
    Rays a{}, b{};
    for (int k = 0; k < 8; ++k) {
        CK(cudaMalloc(&a.f[k], size_t(n) * 4));
        CK(cudaMalloc(&b.f[k], size_t(n) * 4));
    }
    // CUB temporary storage (the largest of the algorithms).
    size_t tempBytes = 0, t1 = 0, t2 = 0, t3 = 0;
    cub::CountingInputIterator<unsigned> counting(0);
    CK(cub::DeviceSelect::Flagged(nullptr, t1, counting, dFlags, dOut, dNum, n));
    CK(cub::DeviceSelect::If(nullptr, t2, counting, dOut, dNum, n, Survives{0u}));
    CK(cub::DeviceScan::ExclusiveSum(nullptr, t3, dFlags32, dOffsets, n));
    tempBytes = std::max(t1, std::max(t2, t3));
    void *dTemp;
    CK(cudaMalloc(&dTemp, tempBytes));
    std::printf("n = %d; CUB temp storage: select-flagged %zu B, select-if %zu B, scan %zu B\n", n, t1, t2, t3);

    std::vector<unsigned> ref, got(n);
    for (double p : {0.1, 0.5, 0.9}) {
        const unsigned threshold = unsigned(p * 4294967295.0);
        Survives pred{threshold};
        makeFlags<<<grid, block>>>(n, threshold, dFlags, dFlags32);
        ref.clear();
        for (int i = 0; i < n; ++i)
            if (hash32(unsigned(i)) < threshold)
                ref.push_back(unsigned(i));
        std::printf("\nsurvival %.0f %% (%zu of %d)\n", p * 100.0, ref.size(), n);
        auto check = [&](const char *name, float ms, bool stableExpected) {
            unsigned count = 0;
            CK(cudaMemcpy(&count, dCounter, 4, cudaMemcpyDeviceToHost));
            CK(cudaMemcpy(got.data(), dOut, ref.size() * 4, cudaMemcpyDeviceToHost));
            bool ordered = std::equal(ref.begin(), ref.end(), got.begin());
            std::vector<unsigned> s(got.begin(), got.begin() + ref.size());
            std::sort(s.begin(), s.end());
            const bool sameSet = std::equal(ref.begin(), ref.end(), s.begin());
            std::printf("  %-48s %7.4f ms  %6.2f G elem/s  %s%s\n", name, ms, n / (ms * 1e6),
                        sameSet ? "ok" : "WRONG SET", ordered ? ", stable order" : ", order differs");
            (void)stableExpected;
        };
        auto withCounter = [&](auto launch) {
            return [=] {
                cudaMemsetAsync(dCounter, 0, 4);
                launch();
            };
        };
        check("1 atomicAdd per element",
              timeIt(withCounter([=] { naiveAtomic<<<grid, block>>>(n, threshold, dOut, dCounter); })), false);
        check("2 warp-aggregated atomic",
              timeIt(withCounter([=] { warpAtomic<<<grid, block>>>(n, threshold, dOut, dCounter); })), false);
        check("3 block-aggregated atomic (BlockScan)",
              timeIt(withCounter([=] { blockAtomic<256><<<grid, 256>>>(n, threshold, dOut, dCounter); })), false);
        check("4 CUB DeviceSelect::Flagged (byte flags)", timeIt([&] {
                  size_t tb = tempBytes;
                  cub::DeviceSelect::Flagged(dTemp, tb, counting, dFlags, dOut, dCounter, n);
              }),
              true);
        check("5 CUB DeviceSelect::If (predicate)", timeIt([&] {
                  size_t tb = tempBytes;
                  cub::DeviceSelect::If(dTemp, tb, counting, dOut, dCounter, n, pred);
              }),
              true);
        check("6 CUB ExclusiveSum + scatter", timeIt([&] {
                  size_t tb = tempBytes;
                  cub::DeviceScan::ExclusiveSum(dTemp, tb, dFlags32, dOffsets, n);
                  scatterScan<<<grid, block>>>(n, dFlags32, dOffsets, dOut);
              }),
              true);
        check("7 thrust::copy_if (predicate)", timeIt([&] {
                  thrust::copy_if(thrust::device, thrust::counting_iterator<unsigned>(0),
                                  thrust::counting_iterator<unsigned>(unsigned(n)), dOut, pred);
              }),
              true);
        // Payload.
        const float fused = timeIt([&] {
            cudaMemsetAsync(dCounter, 0, 4);
            pushFused<<<grid, block>>>(n, threshold, a, dCounter);
        });
        const float viaSelect = timeIt([&] {
            writeFixed<<<grid, block>>>(n, threshold, b, dFlags);
            size_t tb = tempBytes;
            cub::DeviceSelect::Flagged(dTemp, tb, counting, dFlags, dOut, dNum, n);
            gatherRays<<<grid, block>>>(dNum, dOut, b, a);
        });
        const float fixedOnly = timeIt([&] { writeFixed<<<grid, block>>>(n, threshold, b, dFlags); });
        std::printf("  %-48s %7.4f ms\n", "8 rays: fused warp-aggregated push", fused);
        std::printf("  %-48s %7.4f ms\n", "9 rays: fixed slots + Select::Flagged + gather", viaSelect);
        std::printf("  %-48s %7.4f ms\n", "10 rays: fixed slots only (no compaction)", fixedOnly);
    }
    std::printf("\ndone\n");
    return 0;
}
