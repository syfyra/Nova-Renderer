// M10 research probe (docs/research/m10/cuda-wavefront.md, section 4):
// texture lookups on the GPU with CUDA texture objects.
//
//   T1 creation: a 4096^2 RGBA16F and an RGBA8 MIP chain (13 levels):
//      allocation, upload time and the device memory it takes
//   T2 throughput: tex2DGrad (trilinear, anisotropic 8) per second, coherent
//      and random coordinates; bindless: 64 textures, the handle per lane from
//      a table (warp-uniform or random per lane)
//   T3 precision: hardware bilinear filtering (fixed-point weights) against
//      the exact bilinear value on a float ramp, and a software bilinear filter
//      from global memory (4 loads) as the alternative, with its throughput
//
// Build: cw_build.bat <out> tex. Run: cw_tex_probe.exe

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

// A MIP chain of size^2 texels: half4 (fp16) or uchar4 (normalized 8-bit).
static cudaTextureObject_t makeMipTexture(int size, bool fp16, double *uploadMs, size_t *bytes,
                                          cudaMipmappedArray_t *keep) {
    int levels = 1;
    while ((size >> (levels - 1)) > 1)
        ++levels;
    const cudaChannelFormatDesc fmt = fp16 ? cudaCreateChannelDescHalf4() : cudaCreateChannelDesc<uchar4>();
    const size_t texel = fp16 ? 8 : 4;
    size_t freeBefore, freeAfter, total;
    CK(cudaDeviceSynchronize());
    CK(cudaMemGetInfo(&freeBefore, &total));
    cudaMipmappedArray_t mip;
    const auto t0 = std::chrono::steady_clock::now();
    CK(cudaMallocMipmappedArray(&mip, &fmt, make_cudaExtent(size, size, 0), levels));
    std::vector<unsigned char> data(size_t(size) * size * texel);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = (unsigned char)(hash32(unsigned(i / 64)) & 0x3Fu); // small values: valid halves and bytes
    for (int l = 0; l < levels; ++l) {
        const int s = std::max(size >> l, 1);
        cudaArray_t level;
        CK(cudaGetMipmappedArrayLevel(&level, mip, l));
        CK(cudaMemcpy2DToArray(level, 0, 0, data.data(), size_t(s) * texel, size_t(s) * texel, s,
                               cudaMemcpyHostToDevice));
    }
    CK(cudaDeviceSynchronize());
    if (uploadMs)
        *uploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CK(cudaMemGetInfo(&freeAfter, &total));
    if (bytes)
        *bytes = freeBefore - freeAfter;
    *keep = mip;
    cudaResourceDesc res{};
    res.resType = cudaResourceTypeMipmappedArray;
    res.res.mipmap.mipmap = mip;
    cudaTextureDesc td{};
    td.addressMode[0] = td.addressMode[1] = cudaAddressModeWrap;
    td.filterMode = cudaFilterModeLinear;
    td.mipmapFilterMode = cudaFilterModeLinear;
    td.maxAnisotropy = 8;
    td.maxMipmapLevelClamp = float(levels - 1);
    td.readMode = fp16 ? cudaReadModeElementType : cudaReadModeNormalizedFloat;
    td.normalizedCoords = 1;
    cudaTextureObject_t t;
    CK(cudaCreateTextureObject(&t, &res, &td, nullptr));
    return t;
}

// L lookups per thread; coordinates on a 2048-wide screen (coherent) or
// hashed (random); footprint = `texels` texels of a `size` texture per pixel.
template <bool Random>
__global__ void lookups(int n, int L, cudaTextureObject_t t, float footprint, float *out, float footprintY = -1.0f) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    float acc = 0.0f;
    for (int k = 0; k < L; ++k) {
        float u, v;
        if (Random) {
            const unsigned h = hash32(unsigned(i * L + k));
            u = float(h & 0xFFFFu) / 65536.0f;
            v = float(h >> 16) / 65536.0f;
        } else {
            u = float(i % 2048) / 2048.0f + 0.37f * k;
            v = float(i / 2048) / 2048.0f + 0.11f * k;
        }
        const float4 c = tex2DGrad<float4>(t, u, v, make_float2(footprint, 0.0f),
                                           make_float2(0.0f, footprintY < 0.0f ? footprint : footprintY));
        acc += c.x + c.y + c.z + c.w;
    }
    out[i] = acc;
}

// Bindless: the handle from a table, per lane (random) or per warp.
template <bool PerLane>
__global__ void bindless(int n, int L, const cudaTextureObject_t *table, int count, float footprint, float *out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    const unsigned slot = PerLane ? hash32(unsigned(i)) % unsigned(count) : hash32(unsigned(i / 32)) % unsigned(count);
    const cudaTextureObject_t t = table[slot];
    float acc = 0.0f;
    for (int k = 0; k < L; ++k) {
        const float u = float(i % 2048) / 2048.0f + 0.37f * k, v = float(i / 2048) / 2048.0f + 0.11f * k;
        const float4 c = tex2DGrad<float4>(t, u, v, make_float2(footprint, 0.0f), make_float2(0.0f, footprint));
        acc += c.x + c.y + c.z + c.w;
    }
    out[i] = acc;
}

// T3: a 1D ramp of `w` float texels, sampled with hardware linear filtering
// (unnormalized coordinates) and in software.
__global__ void rampHardware(int n, cudaTextureObject_t t, float scale, float *out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        out[i] = tex2D<float>(t, 0.5f + float(i) * scale, 0.5f);
}

// Software bilinear on a float4 image in global memory (clamp addressing).
__global__ void softBilinear(int n, int L, const float4 *__restrict__ img, int size, float *out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    float acc = 0.0f;
    for (int k = 0; k < L; ++k) {
        const float x = float(i % 2048) * (float(size) / 2048.0f) + 0.37f * k * size - 0.5f;
        const float y = float(i / 2048) * (float(size) / 2048.0f) + 0.11f * k * size - 0.5f;
        const float fx = floorf(x), fy = floorf(y);
        const float ax = x - fx, ay = y - fy;
        const int x0 = int(fx) & (size - 1), y0 = int(fy) & (size - 1);
        const int x1 = (x0 + 1) & (size - 1), y1 = (y0 + 1) & (size - 1);
        const float4 a = img[y0 * size + x0], b = img[y0 * size + x1], c = img[y1 * size + x0], d = img[y1 * size + x1];
        const float r = (a.x * (1 - ax) + b.x * ax) * (1 - ay) + (c.x * (1 - ax) + d.x * ax) * ay;
        const float g = (a.y * (1 - ax) + b.y * ax) * (1 - ay) + (c.y * (1 - ax) + d.y * ax) * ay;
        acc += r + g;
    }
    out[i] = acc;
}

__global__ void hwBilinear(int n, int L, cudaTextureObject_t t, int size, float *out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    float acc = 0.0f;
    for (int k = 0; k < L; ++k) {
        const float x = float(i % 2048) * (float(size) / 2048.0f) + 0.37f * k * size;
        const float y = float(i / 2048) * (float(size) / 2048.0f) + 0.11f * k * size;
        const float4 c = tex2D<float4>(t, x / size, y / size);
        acc += c.x + c.y;
    }
    out[i] = acc;
}

int main() {
    cudaDeviceProp prop{};
    CK(cudaGetDeviceProperties(&prop, 0));
    std::printf("device: %s, %d SMs\n", prop.name, prop.multiProcessorCount);

    // T1.
    std::printf("\nT1. MIP chain creation (4096^2, 13 levels)\n");
    std::vector<cudaMipmappedArray_t> keep;
    cudaMipmappedArray_t m;
    double ms;
    size_t bytes;
    const cudaTextureObject_t t16 = makeMipTexture(4096, true, &ms, &bytes, &m);
    keep.push_back(m);
    const double theory16 = 4096.0 * 4096.0 * 8.0 * 4.0 / 3.0;
    std::printf("  RGBA16F: %.1f MB allocated (theory %.1f MB), allocate + upload %.1f ms\n", bytes / 1e6,
                theory16 / 1e6, ms);
    const cudaTextureObject_t t8 = makeMipTexture(4096, false, &ms, &bytes, &m);
    keep.push_back(m);
    std::printf("  RGBA8:   %.1f MB allocated (theory %.1f MB), allocate + upload %.1f ms\n", bytes / 1e6,
                theory16 / 2e6, ms);

    // T2.
    const int n = 1 << 22, L = 4, block = 256, grid = (n + block - 1) / block;
    float *dOut;
    CK(cudaMalloc(&dOut, size_t(n) * 4));
    std::printf("\nT2. tex2DGrad throughput, %d threads x %d lookups\n", n, L);
    auto rep = [&](const char *name, float t) {
        std::printf("  %-58s %7.3f ms  %6.2f G lookups/s\n", name, t, double(n) * L / (t * 1e6));
    };
    const float fp = 2.0f / 2048.0f; // about 2 texels of level 0 per pixel step: level 1
    rep("RGBA16F 4096^2, coherent", timeIt([&] { lookups<false><<<grid, block>>>(n, L, t16, fp, dOut); }));
    rep("RGBA16F 4096^2, random", timeIt([&] { lookups<true><<<grid, block>>>(n, L, t16, fp, dOut); }));
    rep("RGBA8 4096^2, coherent", timeIt([&] { lookups<false><<<grid, block>>>(n, L, t8, fp, dOut); }));
    rep("RGBA8 4096^2, random", timeIt([&] { lookups<true><<<grid, block>>>(n, L, t8, fp, dOut); }));
    rep("RGBA16F, random, footprint 1/4096 (level 0)",
        timeIt([&] { lookups<true><<<grid, block>>>(n, L, t16, 1.0f / 4096.0f, dOut); }));
    rep("RGBA16F, random, footprint 8/4096 (level 3)",
        timeIt([&] { lookups<true><<<grid, block>>>(n, L, t16, 8.0f / 4096.0f, dOut); }));
    rep("RGBA16F, random, anisotropic 8:1 (8/4096 x 1/4096)",
        timeIt([&] { lookups<true><<<grid, block>>>(n, L, t16, 8.0f / 4096.0f, dOut, 1.0f / 4096.0f); }));
    rep("RGBA16F, coherent, anisotropic 8:1 (8/4096 x 1/4096)",
        timeIt([&] { lookups<false><<<grid, block>>>(n, L, t16, 8.0f / 4096.0f, dOut, 1.0f / 4096.0f); }));
    {
        const int count = 64;
        std::vector<cudaTextureObject_t> table;
        for (int i = 0; i < count; ++i) {
            table.push_back(makeMipTexture(1024, true, nullptr, nullptr, &m));
            keep.push_back(m);
        }
        cudaTextureObject_t *dTable;
        CK(cudaMalloc(&dTable, count * sizeof(cudaTextureObject_t)));
        CK(cudaMemcpy(dTable, table.data(), count * sizeof(cudaTextureObject_t), cudaMemcpyHostToDevice));
        rep("bindless 64 x 1024^2 RGBA16F, handle per warp", timeIt([&] {
                bindless<false><<<grid, block>>>(n, L, dTable, count, 1.0f / 1024.0f, dOut);
            }));
        rep("bindless 64 x 1024^2 RGBA16F, handle per lane (random)", timeIt([&] {
                bindless<true><<<grid, block>>>(n, L, dTable, count, 1.0f / 1024.0f, dOut);
            }));
    }

    // T3: precision.
    std::printf("\nT3. Bilinear precision and software filtering\n");
    {
        const int w = 64;
        std::vector<float> ramp(w);
        for (int i = 0; i < w; ++i)
            ramp[i] = float(i % 2); // 0,1,0,1: the largest neighbour difference
        cudaArray_t arr;
        const cudaChannelFormatDesc fmt = cudaCreateChannelDesc<float>();
        CK(cudaMallocArray(&arr, &fmt, w, 1));
        CK(cudaMemcpy2DToArray(arr, 0, 0, ramp.data(), w * 4, w * 4, 1, cudaMemcpyHostToDevice));
        cudaResourceDesc res{};
        res.resType = cudaResourceTypeArray;
        res.res.array.array = arr;
        cudaTextureDesc td{};
        td.addressMode[0] = td.addressMode[1] = cudaAddressModeClamp;
        td.filterMode = cudaFilterModeLinear;
        td.readMode = cudaReadModeElementType;
        td.normalizedCoords = 0;
        cudaTextureObject_t rt;
        CK(cudaCreateTextureObject(&rt, &res, &td, nullptr));
        const int samples = 1 << 16;
        const float scale = float(w - 2) / float(samples);
        rampHardware<<<(samples + 255) / 256, 256>>>(samples, rt, scale, dOut);
        std::vector<float> got(samples);
        CK(cudaMemcpy(got.data(), dOut, samples * 4, cudaMemcpyDeviceToHost));
        double maxErr = 0.0;
        int distinct = 0;
        float last = -1.0f;
        for (int i = 0; i < samples; ++i) {
            const double x = double(i) * scale; // texel space, texel centres at integers
            const int x0 = int(std::floor(x));
            const double a = x - x0;
            const double exact = ramp[x0] * (1.0 - a) + ramp[x0 + 1] * a;
            maxErr = std::max(maxErr, std::fabs(exact - got[i]));
            if (i < samples / (w - 2) && got[i] != last) {
                ++distinct;
                last = got[i];
            }
        }
        std::printf("  hardware linear filter on a 0/1 step: max |error| %.5f of the step (1/%.0f); %d distinct "
                    "values between two texels\n",
                    maxErr, 1.0 / maxErr, distinct);
    }
    {
        const int size = 2048;
        std::vector<float4> img(size_t(size) * size);
        for (size_t i = 0; i < img.size(); ++i)
            img[i] = make_float4(float(hash32(unsigned(i)) & 255u) / 255.0f, 0.5f, 0.25f, 1.0f);
        float4 *dImg;
        CK(cudaMalloc(&dImg, img.size() * sizeof(float4)));
        CK(cudaMemcpy(dImg, img.data(), img.size() * sizeof(float4), cudaMemcpyHostToDevice));
        cudaArray_t arr;
        const cudaChannelFormatDesc fmt = cudaCreateChannelDesc<float4>();
        CK(cudaMallocArray(&arr, &fmt, size, size));
        CK(cudaMemcpy2DToArray(arr, 0, 0, img.data(), size * sizeof(float4), size * sizeof(float4), size,
                               cudaMemcpyHostToDevice));
        cudaResourceDesc res{};
        res.resType = cudaResourceTypeArray;
        res.res.array.array = arr;
        cudaTextureDesc td{};
        td.addressMode[0] = td.addressMode[1] = cudaAddressModeWrap;
        td.filterMode = cudaFilterModeLinear;
        td.readMode = cudaReadModeElementType;
        td.normalizedCoords = 1;
        cudaTextureObject_t ft;
        CK(cudaCreateTextureObject(&ft, &res, &td, nullptr));
        rep("float4 2048^2 bilinear, hardware (tex2D)", timeIt([&] { hwBilinear<<<grid, block>>>(n, L, ft, size, dOut); }));
        rep("float4 2048^2 bilinear, software (4 global loads)",
            timeIt([&] { softBilinear<<<grid, block>>>(n, L, dImg, size, dOut); }));
    }
    (void)t8;
    std::printf("\ndone\n");
    return 0;
}
