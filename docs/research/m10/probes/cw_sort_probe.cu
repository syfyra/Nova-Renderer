// M10 research probe (docs/research/m10/cuda-wavefront.md, section 2.5):
// sorting a wave's hits by material on the GPU. n = 1920 x 1080 = 2,073,600
// (key = material id, value = ray index), K distinct materials.
//
//   1 CUB DeviceRadixSort::SortPairs, all 32 key bits
//   2 the same with end_bit = ceil(log2 K) (only the bits materials use)
//   3 CUB DeviceRadixSort::SortKeys on a packed key (material << 21 | index),
//     end_bit = 21 + ceil(log2 K): one 32-bit stream instead of two
//   4 counting sort: shared-memory histogram, scan of K bins, atomic scatter
//     (unstable order within a material)
//   5 after 2: DeviceRunLengthEncode of the sorted keys (the material runs a
//     per-material launch needs)
//   6 after 2: gather of a 24-byte hit record into sorted order
//   7 launch overhead: back-to-back launches of an empty kernel (one launch
//     per material), stream and CUDA graph
//
// Build: cw_build.bat. Run: cw_sort_probe.exe

#include <cub/cub.cuh>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

static unsigned hashRegion(unsigned x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int bitsFor(unsigned k) {
    int b = 0;
    while ((1u << b) < k)
        ++b;
    return std::max(b, 1);
}

__global__ void iota(int n, unsigned *v) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        v[i] = unsigned(i);
}

__global__ void pack(int n, const unsigned *key, unsigned *packed) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        packed[i] = key[i] << 21 | unsigned(i);
}

template <int MaxK>
__global__ void histogram(int n, const unsigned *key, unsigned *hist, int K) {
    __shared__ unsigned h[MaxK];
    for (int k = threadIdx.x; k < K; k += blockDim.x)
        h[k] = 0;
    __syncthreads();
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x)
        atomicAdd(&h[key[i]], 1u);
    __syncthreads();
    for (int k = threadIdx.x; k < K; k += blockDim.x)
        if (h[k])
            atomicAdd(&hist[k], h[k]);
}

__global__ void scatterCount(int n, const unsigned *key, unsigned *cursor, unsigned *outIdx) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        outIdx[atomicAdd(&cursor[key[i]], 1u)] = unsigned(i);
}

struct Hit {
    float t;
    unsigned mesh, prim;
    float u, v;
    unsigned instance;
};

__global__ void gatherHits(int n, const unsigned *order, const Hit *in, Hit *out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        out[i] = in[order[i]];
}

__global__ void empty() {}

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
    unsigned *keys0, *keys1, *vals0, *vals1, *keysIn, *packed0, *packed1, *hist, *cursor, *runs, *counts, *numRuns;
    CK(cudaMalloc(&keys0, n * 4));
    CK(cudaMalloc(&keys1, n * 4));
    CK(cudaMalloc(&vals0, n * 4));
    CK(cudaMalloc(&vals1, n * 4));
    CK(cudaMalloc(&keysIn, n * 4));
    CK(cudaMalloc(&packed0, n * 4));
    CK(cudaMalloc(&packed1, n * 4));
    CK(cudaMalloc(&hist, 65536 * 4));
    CK(cudaMalloc(&cursor, 65536 * 4));
    CK(cudaMalloc(&runs, n * 4));
    CK(cudaMalloc(&counts, n * 4));
    CK(cudaMalloc(&numRuns, 4));
    Hit *hitsIn, *hitsOut;
    CK(cudaMalloc(&hitsIn, size_t(n) * sizeof(Hit)));
    CK(cudaMalloc(&hitsOut, size_t(n) * sizeof(Hit)));
    CK(cudaMemset(hitsIn, 0, size_t(n) * sizeof(Hit)));

    size_t tempBytes = 0;
    {
        cub::DoubleBuffer<unsigned> k(keys0, keys1), v(vals0, vals1);
        size_t t1 = 0, t2 = 0, t3 = 0;
        CK(cub::DeviceRadixSort::SortPairs(nullptr, t1, k, v, n));
        cub::DoubleBuffer<unsigned> p(packed0, packed1);
        CK(cub::DeviceRadixSort::SortKeys(nullptr, t2, p, n));
        CK(cub::DeviceRunLengthEncode::Encode(nullptr, t3, keys0, runs, counts, numRuns, n));
        tempBytes = std::max(t1, std::max(t2, t3));
        std::printf("n = %d; temp storage: SortPairs %zu B, SortKeys %zu B, RLE %zu B\n", n, t1, t2, t3);
    }
    void *temp;
    CK(cudaMalloc(&temp, tempBytes));

    std::mt19937 rng(11);
    struct Case {
        unsigned K;
        bool coherent; // keys constant over runs of 256 elements (screen regions)
    };
    const Case cases[] = {{1, false}, {16, false}, {256, false}, {256, true}, {4096, false}, {65536, false}};
    std::printf("\n%-22s %9s %9s %9s %9s %9s %9s\n", "keys", "pairs32", "pairsK", "packedK", "count", "+rle", "+gather");
    for (const Case &c : cases) {
        std::vector<unsigned> h(n);
        for (int i = 0; i < n; ++i)
            h[i] = c.coherent ? hashRegion(unsigned(i / 256)) % c.K : unsigned(rng() % c.K);
        CK(cudaMemcpy(keysIn, h.data(), size_t(n) * 4, cudaMemcpyHostToDevice));
        const int bits = bitsFor(c.K);
        auto sortPairs = [&](int endBit) {
            CK(cudaMemcpyAsync(keys0, keysIn, size_t(n) * 4, cudaMemcpyDeviceToDevice));
            iota<<<grid, block>>>(n, vals0);
            cub::DoubleBuffer<unsigned> k(keys0, keys1), v(vals0, vals1);
            size_t tb = tempBytes;
            cub::DeviceRadixSort::SortPairs(temp, tb, k, v, n, 0, endBit);
            return std::make_pair(k.Current(), v.Current());
        };
        // Copy + iota alone (subtracted below).
        const float base = timeIt([&] {
            CK(cudaMemcpyAsync(keys0, keysIn, size_t(n) * 4, cudaMemcpyDeviceToDevice));
            iota<<<grid, block>>>(n, vals0);
        });
        const float pairs32 = timeIt([&] { sortPairs(32); }) - base;
        const float pairsK = timeIt([&] { sortPairs(bits); }) - base;
        const float packBase = timeIt([&] { pack<<<grid, block>>>(n, keysIn, packed0); });
        const float packedK = timeIt([&] {
                                  pack<<<grid, block>>>(n, keysIn, packed0);
                                  cub::DoubleBuffer<unsigned> p(packed0, packed1);
                                  size_t tb = tempBytes;
                                  cub::DeviceRadixSort::SortKeys(temp, tb, p, n, 0, std::min(32, 21 + bits));
                              }) -
                              packBase;
        float counting = -1.0f;
        if (c.K <= 4096) {
            counting = timeIt([&] {
                CK(cudaMemsetAsync(hist, 0, c.K * 4));
                histogram<4096><<<prop.multiProcessorCount * 4, 256>>>(n, keysIn, hist, int(c.K));
                size_t tb = tempBytes;
                cub::DeviceScan::ExclusiveSum(temp, tb, hist, cursor, int(c.K));
                scatterCount<<<grid, block>>>(n, keysIn, cursor, vals1);
            });
        }
        auto sorted = sortPairs(bits);
        CK(cudaDeviceSynchronize());
        const float rle = timeIt([&] {
            size_t tb = tempBytes;
            cub::DeviceRunLengthEncode::Encode(temp, tb, sorted.first, runs, counts, numRuns, n);
        });
        const float gather = timeIt([&] { gatherHits<<<grid, block>>>(n, sorted.second, hitsIn, hitsOut); });
        // Check the sort.
        std::vector<unsigned> sk(n), sv(n);
        CK(cudaMemcpy(sk.data(), sorted.first, size_t(n) * 4, cudaMemcpyDeviceToHost));
        CK(cudaMemcpy(sv.data(), sorted.second, size_t(n) * 4, cudaMemcpyDeviceToHost));
        bool ok = true;
        for (int i = 1; i < n && ok; ++i)
            ok = sk[i - 1] < sk[i] || (sk[i - 1] == sk[i] && sv[i - 1] < sv[i]); // stable
        char label[64];
        std::snprintf(label, sizeof(label), "K=%u%s", c.K, c.coherent ? " coherent" : " random");
        std::printf("%-22s %9.4f %9.4f %9.4f ", label, pairs32, pairsK, packedK);
        if (counting >= 0.0f)
            std::printf("%9.4f ", counting);
        else
            std::printf("%9s ", "-");
        std::printf("%9.4f %9.4f   %s (ms)\n", rle, gather, ok ? "stable ok" : "NOT SORTED/STABLE");
    }

    // Launch overhead.
    std::printf("\nlaunch overhead (empty kernel, 1 block of 32 threads):\n");
    for (int count : {16, 256, 1024}) {
        const float ms = timeIt([&] {
            for (int i = 0; i < count; ++i)
                empty<<<1, 32>>>();
        });
        std::printf("  %5d launches on a stream: %8.4f ms (%.2f us each)\n", count, ms, ms * 1000.0f / count);
        cudaStream_t s;
        CK(cudaStreamCreate(&s));
        cudaGraph_t g;
        cudaGraphExec_t ge;
        CK(cudaStreamBeginCapture(s, cudaStreamCaptureModeGlobal));
        for (int i = 0; i < count; ++i)
            empty<<<1, 32, 0, s>>>();
        CK(cudaStreamEndCapture(s, &g));
        CK(cudaGraphInstantiate(&ge, g, 0));
        const float gms = timeIt([&] { CK(cudaGraphLaunch(ge, 0)); });
        std::printf("  %5d launches in a graph:  %8.4f ms (%.2f us each)\n", count, gms, gms * 1000.0f / count);
        cudaGraphExecDestroy(ge);
        cudaGraphDestroy(g);
        cudaStreamDestroy(s);
    }
    std::printf("\ndone\n");
    return 0;
}
