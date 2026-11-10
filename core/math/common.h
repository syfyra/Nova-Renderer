#pragma once

// Definitions shared by every core module. Everything below core/ must compile
// for the CPU today and for CUDA later, so kernel-side helpers are marked
// NOVA_HOST_DEVICE and avoid virtual calls, exceptions and the STL.

#include <cstdint>

#if defined(__CUDACC__)
#define NOVA_HOST_DEVICE __host__ __device__
#else
#define NOVA_HOST_DEVICE
#endif

#define NOVA_INLINE NOVA_HOST_DEVICE inline

// Small hot helpers the host compiler must inline (MSVC's heuristics keep
// some out of line inside large shading functions; docs/design/M7.md D8).
#if defined(__CUDACC__)
#define NOVA_FORCEINLINE __forceinline__ __host__ __device__
#elif defined(_MSC_VER)
#define NOVA_FORCEINLINE __forceinline
#else
#define NOVA_FORCEINLINE inline __attribute__((always_inline))
#endif

// An inline (header) function the host compiler keeps out of line: large
// dispatch switches would otherwise bloat the hot loops they are called from.
#if defined(__CUDACC__)
#define NOVA_OUTLINE NOVA_HOST_DEVICE inline
#elif defined(_MSC_VER)
#define NOVA_OUTLINE __declspec(noinline) inline
#else
#define NOVA_OUTLINE __attribute__((noinline)) inline
#endif

namespace nova {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kHalfPi = 0.5f * kPi;
constexpr float kInvPi = 1.0f / kPi;
constexpr float kInvTwoPi = 1.0f / kTwoPi;
constexpr float kInf = 1e30f; // "no limit" for ray extents

constexpr uint32_t kInvalidId = 0xFFFFFFFFu;

// std::fmin / std::fmax with their NaN rule (a NaN operand yields the other
// one), inline: MSVC compiles the std:: versions into CRT calls (D8).
NOVA_FORCEINLINE float fmin(float a, float b) { return (b < a || a != a) ? b : a; }
NOVA_FORCEINLINE float fmax(float a, float b) { return (b > a || a != a) ? b : a; }

// float -> int with a defined result for every input. The plain conversion is
// undefined beyond int's range and for NaN, and the devices disagree there:
// x86 gives INT_MIN, CUDA saturates and turns NaN into 0.
//
// toInt truncates and gives INT_MIN wherever x86 did (out of range, NaN), so
// shading on values run wild (a noise position of 1e12, an infinite octave
// count) stays what the CPU rendered and is the same on the GPU.
NOVA_FORCEINLINE int toInt(float x) {
    return (x >= -2147483648.0f && x < 2147483648.0f) ? int(x) : (-2147483647 - 1);
}
// clampToInt truncates x clamped to [lo, hi]; a NaN gives lo.
NOVA_FORCEINLINE int clampToInt(float x, int lo, int hi) { return x >= float(hi) ? hi : (x > float(lo) ? int(x) : lo); }

} // namespace nova
