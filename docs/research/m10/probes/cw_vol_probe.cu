// M10 research probe (docs/research/m10/cuda-wavefront.md, section 5):
// NanoVDB 32.6 (Houdini 22.0.466's $HFS/toolkit/include/nanovdb) on the GPU,
// and the cost and SIMD efficiency of Nova-style trackers there.
//
// A heterogeneous cloud (sphere of radius 96 voxels modulated by a sum of
// sines, density in [0, 1], about 3.5M active voxels) is built on the host
// with nanovdb::build::Grid, converted with createNanoGrid and uploaded as
// one buffer (the same bytes Nova keeps in VolumeGrid). A majorant grid of
// 8^3-voxel cells (dilated by one voxel for trilinear lookups, as
// core/volume/majorant.h) is built on the host. 1024 x 1024 orthographic rays.
//
//   V1 trilinear lookups (ReadAccessor + SampleFromVoxels<1>), random points
//      and points along rays
//   V2 ratio tracking, one global majorant (transmittance to the far side)
//   V3 ratio tracking on the majorant grid (DDA over the cells)
//   V4 delta tracking free flights on the majorant grid (stop at the first
//      real collision: Nova's free flight without the emission gather)
//   each tracker: rays in pixel order and shuffled; steps per ray and SIMD
//   efficiency (sum of lane steps / (32 x sum of warp maxima))
//
// Build: cw_build.bat <out> vol (adds -I $HFS/toolkit/include). Run: cw_vol_probe.exe

#include <nanovdb/NanoVDB.h>
#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/GridBuilder.h>
#include <nanovdb/util/SampleFromVoxels.h>

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

constexpr int kLo = -112, kRes = 28, kCell = 8; // majorant cells cover [kLo, kLo + kRes * kCell)
constexpr float kSigma = 0.05f;                 // extinction per voxel at density 1
constexpr int kImage = 1024;

__host__ __device__ inline unsigned hash32(unsigned x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

struct Rng {
    unsigned s;
    __device__ float next() {
        s = s * 747796405u + 2891336453u;
        unsigned w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
        w = (w >> 22u) ^ w;
        return float(w >> 8) * (1.0f / 16777216.0f);
    }
};

static float density(int x, int y, int z) {
    const float r = std::sqrt(float(x * x + y * y + z * z));
    const float n = 0.5f * std::sin(0.11f * x) * std::sin(0.13f * y + 1.0f) * std::sin(0.07f * z + 2.0f) +
                    0.25f * std::sin(0.31f * x + 0.5f) * std::sin(0.29f * z);
    const float d = (1.0f - r / 96.0f) * 1.5f + n;
    return d <= 0.0f ? 0.0f : (d >= 1.0f ? 1.0f : d);
}

using Acc = nanovdb::DefaultReadAccessor<float>;
using Sampler = nanovdb::SampleFromVoxels<Acc, 1, true>;

struct Ray {
    float ox, oy, oz;
    float t0, t1; // inside the box
};

__device__ inline Ray makeRay(int pixel) {
    const int px = pixel % kImage, py = pixel / kImage;
    Ray r;
    r.ox = float(kLo) + (float(px) + 0.5f) * (float(kRes * kCell) / kImage);
    r.oy = float(kLo) + (float(py) + 0.5f) * (float(kRes * kCell) / kImage);
    r.oz = float(kLo) - 10.0f;
    r.t0 = 10.0f;
    r.t1 = 10.0f + float(kRes * kCell);
    return r; // direction +z
}

__global__ void lookupsRandom(int n, const nanovdb::FloatGrid *grid, float *out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    Acc acc = grid->getAccessor();
    Sampler s(acc);
    float sum = 0.0f;
    for (int k = 0; k < 8; ++k) {
        const unsigned h = hash32(unsigned(i * 8 + k));
        const float x = float(kLo) + float(h & 1023u) * (224.0f / 1024.0f);
        const float y = float(kLo) + float((h >> 10) & 1023u) * (224.0f / 1024.0f);
        const float z = float(kLo) + float((h >> 20) & 1023u) * (224.0f / 1024.0f);
        sum += s(nanovdb::Vec3f(x, y, z));
    }
    out[i] = sum;
}

__global__ void lookupsAlongRays(int n, const nanovdb::FloatGrid *grid, float *out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    Acc acc = grid->getAccessor();
    Sampler s(acc);
    const Ray r = makeRay(i);
    float sum = 0.0f;
    for (int k = 0; k < 8; ++k)
        sum += s(nanovdb::Vec3f(r.ox, r.oy, r.oz + r.t0 + 28.0f * k + 0.37f));
    out[i] = sum;
}

struct Stats {
    unsigned long long laneSteps, warpMaxSteps;
};

__device__ inline void countSteps(int steps, Stats *st) {
    if (!st)
        return;
    int m = steps;
    for (int o = 16; o > 0; o >>= 1)
        m = max(m, __shfl_xor_sync(0xFFFFFFFFu, m, o));
    int sum = steps;
    for (int o = 16; o > 0; o >>= 1)
        sum += __shfl_xor_sync(0xFFFFFFFFu, sum, o);
    if ((threadIdx.x & 31) == 0) {
        atomicAdd(&st->laneSteps, (unsigned long long)sum);
        atomicAdd(&st->warpMaxSteps, (unsigned long long)m);
    }
}

// Mode 0: ratio tracking, global majorant; 1: ratio tracking, majorant grid;
// 2: delta tracking free flight, majorant grid.
template <int Mode>
__global__ void track(int n, const int *order, const nanovdb::FloatGrid *grid, const float *majorant,
                      float globalMajorant, float *out, Stats *st) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    const int pixel = order ? order[i] : i;
    const Ray r = makeRay(pixel);
    Acc acc = grid->getAccessor();
    Sampler s(acc);
    Rng rng{hash32(unsigned(pixel) * 9781u + 1u)};
    float T = 1.0f;
    int steps = 0;
    float result = 0.0f;
    if (Mode == 0) {
        const float sb = globalMajorant;
        float t = r.t0;
        for (;;) {
            t -= logf(1.0f - rng.next()) / sb;
            if (t >= r.t1)
                break;
            ++steps;
            const float sigma = kSigma * s(nanovdb::Vec3f(r.ox, r.oy, r.oz + t));
            T *= 1.0f - sigma / sb;
            if (T < 0.1f) { // roulette as Nova's trackers
                if (rng.next() < 0.5f) {
                    T = 0.0f;
                    break;
                }
                T *= 2.0f;
            }
        }
        result = T;
    } else {
        // DDA along +z through the column of cells of this ray.
        const int cx = min(max(int((r.ox - kLo) / kCell), 0), kRes - 1);
        const int cy = min(max(int((r.oy - kLo) / kCell), 0), kRes - 1);
        float t = r.t0;
        bool done = false;
        for (int cz = 0; cz < kRes && !done; ++cz) {
            const float cellEnd = r.t0 + float((cz + 1) * kCell);
            const float sb = majorant[(cz * kRes + cy) * kRes + cx];
            if (!(sb > 0.0f)) {
                t = cellEnd;
                continue;
            }
            for (;;) {
                const float tn = t - logf(1.0f - rng.next()) / sb;
                if (tn >= cellEnd) {
                    t = cellEnd; // memoryless: restart at the cell boundary
                    break;
                }
                t = tn;
                ++steps;
                const float sigma = kSigma * s(nanovdb::Vec3f(r.ox, r.oy, r.oz + t));
                if (Mode == 1) {
                    T *= 1.0f - sigma / sb;
                    if (T < 0.1f) {
                        if (rng.next() < 0.5f) {
                            T = 0.0f;
                            done = true;
                            break;
                        }
                        T *= 2.0f;
                    }
                } else if (rng.next() < sigma / sb) { // a real collision
                    result = t;
                    done = true;
                    break;
                }
            }
        }
        if (Mode == 1)
            result = T;
    }
    out[i] = result;
    countSteps(steps, st);
}

static float median(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <typename F>
static float timeIt(F &&f, int reps = 11) {
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
    std::printf("device: %s; NanoVDB %d.%d.%d\n", prop.name, NANOVDB_MAJOR_VERSION_NUMBER,
                NANOVDB_MINOR_VERSION_NUMBER, NANOVDB_PATCH_VERSION_NUMBER);

    // Host: the source grid, the dense copy for majorants.
    const auto t0 = std::chrono::steady_clock::now();
    const int side = kRes * kCell;
    std::vector<float> dense(size_t(side) * side * side, 0.0f);
    nanovdb::build::Grid<float> src(0.0f);
    {
        auto acc = src.getAccessor();
        for (int z = kLo; z < kLo + side; ++z)
            for (int y = kLo; y < kLo + side; ++y)
                for (int x = kLo; x < kLo + side; ++x) {
                    const float d = density(x, y, z);
                    if (d > 0.0f) {
                        acc.setValue(nanovdb::Coord(x, y, z), d);
                        dense[(size_t(z - kLo) * side + (y - kLo)) * side + (x - kLo)] = d;
                    }
                }
    }
    auto handle = nanovdb::createNanoGrid(src);
    const nanovdb::FloatGrid *hostGrid = handle.grid<float>();
    std::printf("grid: %llu active voxels, %.1f MB NanoVDB buffer, built in %.0f ms\n",
                (unsigned long long)hostGrid->activeVoxelCount(), handle.size() / 1e6,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    // Majorants per cell over the voxels whose trilinear support reaches it.
    std::vector<float> maj(size_t(kRes) * kRes * kRes, 0.0f);
    float globalMax = 0.0f;
    for (int cz = 0; cz < kRes; ++cz)
        for (int cy = 0; cy < kRes; ++cy)
            for (int cx = 0; cx < kRes; ++cx) {
                float m = 0.0f;
                for (int z = cz * kCell - 1; z <= cz * kCell + kCell; ++z)
                    for (int y = cy * kCell - 1; y <= cy * kCell + kCell; ++y)
                        for (int x = cx * kCell - 1; x <= cx * kCell + kCell; ++x)
                            if (x >= 0 && y >= 0 && z >= 0 && x < side && y < side && z < side)
                                m = std::max(m, dense[(size_t(z) * side + y) * side + x]);
                maj[(size_t(cz) * kRes + cy) * kRes + cx] = m * kSigma;
                globalMax = std::max(globalMax, m * kSigma);
            }
    int emptyCells = 0;
    for (float m : maj)
        emptyCells += m == 0.0f;
    std::printf("majorant grid: %d^3 cells of %d^3 voxels, %d empty (%.0f %%)\n", kRes, kCell, emptyCells,
                100.0 * emptyCells / maj.size());

    void *dGridBuf;
    CK(cudaMalloc(&dGridBuf, handle.size()));
    CK(cudaMemcpy(dGridBuf, handle.data(), handle.size(), cudaMemcpyHostToDevice));
    const auto *dGrid = reinterpret_cast<const nanovdb::FloatGrid *>(dGridBuf);
    float *dMaj, *dOut;
    CK(cudaMalloc(&dMaj, maj.size() * 4));
    CK(cudaMemcpy(dMaj, maj.data(), maj.size() * 4, cudaMemcpyHostToDevice));
    const int n = kImage * kImage, block = 128, grid = (n + block - 1) / block;
    CK(cudaMalloc(&dOut, size_t(n) * 4));
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(5);
    std::shuffle(order.begin(), order.end(), rng);
    int *dOrder;
    CK(cudaMalloc(&dOrder, size_t(n) * 4));
    CK(cudaMemcpy(dOrder, order.data(), size_t(n) * 4, cudaMemcpyHostToDevice));
    Stats *dStats;
    CK(cudaMalloc(&dStats, sizeof(Stats)));

    std::printf("\nV1. trilinear lookups, %d threads x 8\n", n);
    const float msR = timeIt([&] { lookupsRandom<<<grid, block>>>(n, dGrid, dOut); });
    const float msA = timeIt([&] { lookupsAlongRays<<<grid, block>>>(n, dGrid, dOut); });
    std::printf("  random points in the box:  %7.3f ms  %6.2f G lookups/s\n", msR, 8.0 * n / (msR * 1e6));
    std::printf("  points along the rays:     %7.3f ms  %6.2f G lookups/s\n", msA, 8.0 * n / (msA * 1e6));

    std::printf("\nV2-V4. trackers, %d rays through the box (+z)\n", n);
    const char *names[3] = {"ratio tracking, global majorant", "ratio tracking, majorant grid",
                            "delta tracking free flight, grid"};
    for (int mode = 0; mode < 3; ++mode)
        for (int shuffled = 0; shuffled < 2; ++shuffled) {
            const int *ord = shuffled ? dOrder : nullptr;
            auto launch = [&](Stats *st) {
                if (mode == 0)
                    track<0><<<grid, block>>>(n, ord, dGrid, dMaj, globalMax, dOut, st);
                else if (mode == 1)
                    track<1><<<grid, block>>>(n, ord, dGrid, dMaj, globalMax, dOut, st);
                else
                    track<2><<<grid, block>>>(n, ord, dGrid, dMaj, globalMax, dOut, st);
            };
            const float ms = timeIt([&] { launch(nullptr); });
            Stats h{};
            CK(cudaMemset(dStats, 0, sizeof(Stats)));
            launch(dStats);
            CK(cudaMemcpy(&h, dStats, sizeof(Stats), cudaMemcpyDeviceToHost));
            std::printf("  %-34s %-9s %7.3f ms  %5.1f steps/ray  %6.2f G steps/s  SIMD efficiency %4.1f %%\n",
                        names[mode], shuffled ? "shuffled" : "in order", ms, double(h.laneSteps) / n,
                        double(h.laneSteps) / (ms * 1e6), 100.0 * double(h.laneSteps) / (32.0 * h.warpMaxSteps));
        }
    std::printf("\ndone\n");
    return 0;
}
