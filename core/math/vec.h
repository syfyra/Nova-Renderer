#pragma once

#include "core/math/common.h"

#include <cmath>

namespace nova {

struct Vec2f {
    float x, y;
};

struct Vec3f {
    float x, y, z;
    NOVA_FORCEINLINE float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    NOVA_FORCEINLINE float &operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

NOVA_FORCEINLINE Vec2f operator+(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
NOVA_FORCEINLINE Vec2f operator-(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }
NOVA_FORCEINLINE Vec2f operator*(Vec2f a, float s) { return {a.x * s, a.y * s}; }
NOVA_FORCEINLINE float dot(Vec2f a, Vec2f b) { return a.x * b.x + a.y * b.y; }

NOVA_FORCEINLINE Vec3f operator+(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
NOVA_FORCEINLINE Vec3f operator-(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
NOVA_FORCEINLINE Vec3f operator-(Vec3f a) { return {-a.x, -a.y, -a.z}; }
NOVA_FORCEINLINE Vec3f operator*(Vec3f a, Vec3f b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
NOVA_FORCEINLINE Vec3f operator*(Vec3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
NOVA_FORCEINLINE Vec3f operator*(float s, Vec3f a) { return a * s; }
NOVA_FORCEINLINE Vec3f operator/(Vec3f a, float s) { return a * (1.0f / s); }
NOVA_FORCEINLINE Vec3f operator/(Vec3f a, Vec3f b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
NOVA_FORCEINLINE Vec3f &operator+=(Vec3f &a, Vec3f b) { a = a + b; return a; }
NOVA_FORCEINLINE Vec3f &operator-=(Vec3f &a, Vec3f b) { a = a - b; return a; }
NOVA_FORCEINLINE Vec3f &operator*=(Vec3f &a, Vec3f b) { a = a * b; return a; }
NOVA_FORCEINLINE Vec3f &operator*=(Vec3f &a, float s) { a = a * s; return a; }

NOVA_FORCEINLINE float sqr(float x) { return x * x; }
NOVA_FORCEINLINE float saturate(float x) { return nova::fmin(nova::fmax(x, 0.0f), 1.0f); }
NOVA_FORCEINLINE float safeSqrt(float x) { return std::sqrt(nova::fmax(0.0f, x)); }
// a (1 - t) + b t: exact at both ends (MaterialX's mix).
NOVA_FORCEINLINE float lerp(float a, float b, float t) { return a * (1.0f - t) + b * t; }
NOVA_FORCEINLINE Vec3f lerp(Vec3f a, Vec3f b, float t) { return a * (1.0f - t) + b * t; }

NOVA_FORCEINLINE float dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
NOVA_FORCEINLINE Vec3f cross(Vec3f a, Vec3f b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
NOVA_FORCEINLINE float length(Vec3f a) { return std::sqrt(dot(a, a)); }
NOVA_FORCEINLINE Vec3f normalize(Vec3f a) {
    const float l2 = dot(a, a);
    return l2 > 0.0f ? a * (1.0f / std::sqrt(l2)) : Vec3f{0.0f, 0.0f, 1.0f};
}
NOVA_FORCEINLINE Vec3f absolute(Vec3f a) { return {std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}; }
NOVA_FORCEINLINE Vec3f componentMin(Vec3f a, Vec3f b) {
    return {nova::fmin(a.x, b.x), nova::fmin(a.y, b.y), nova::fmin(a.z, b.z)};
}
NOVA_FORCEINLINE Vec3f componentMax(Vec3f a, Vec3f b) {
    return {nova::fmax(a.x, b.x), nova::fmax(a.y, b.y), nova::fmax(a.z, b.z)};
}
NOVA_FORCEINLINE float maxComponent(Vec3f a) { return nova::fmax(a.x, nova::fmax(a.y, a.z)); }
NOVA_FORCEINLINE bool isBlack(Vec3f a) { return a.x == 0.0f && a.y == 0.0f && a.z == 0.0f; }
// MSVC's std::isfinite is a host-only template; CUDA has its own.
NOVA_FORCEINLINE bool isFinite(float v) {
#if defined(__CUDA_ARCH__)
    return isfinite(v);
#else
    return std::isfinite(v);
#endif
}
NOVA_FORCEINLINE bool isFinite(Vec3f a) { return isFinite(a.x) && isFinite(a.y) && isFinite(a.z); }

// Flips v into the hemisphere of ref.
NOVA_FORCEINLINE Vec3f faceForward(Vec3f v, Vec3f ref) { return dot(v, ref) < 0.0f ? -v : v; }

} // namespace nova
