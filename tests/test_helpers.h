#pragma once

#include "core/integrator/renderer.h"
#include "core/scene/mesh.h"

#include <cmath>

namespace nova::test {

inline MeshDesc makeSphere(float radius, int segments, int rings) {
    MeshDesc m;
    for (int r = 0; r <= rings; ++r) {
        const float theta = kPi * float(r) / float(rings);
        for (int s = 0; s <= segments; ++s) {
            const float phi = kTwoPi * float(s) / float(segments);
            const Vec3f n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
            m.points.push_back(n * radius);
            m.normals.push_back(n);
        }
    }
    const uint32_t stride = uint32_t(segments) + 1;
    for (uint32_t r = 0; r < uint32_t(rings); ++r) {
        for (uint32_t s = 0; s < uint32_t(segments); ++s) {
            const uint32_t a = r * stride + s, b = a + stride;
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return m;
}

// Axis-aligned square in the z = 0 plane, facing +Z (counter-clockwise).
inline MeshDesc makeQuad(float halfSize) {
    MeshDesc m;
    m.points = {{-halfSize, -halfSize, 0.0f}, {halfSize, -halfSize, 0.0f},
                {halfSize, halfSize, 0.0f}, {-halfSize, halfSize, 0.0f}};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

inline Mat4 translation(Vec3f t) {
    Mat4 m = Mat4::identity();
    m.m[3][0] = t.x;
    m.m[3][1] = t.y;
    m.m[3][2] = t.z;
    return m;
}

// Orthographic camera at `eye` looking down -Z; NDC [-1, 1] covers
// [-halfWidth, halfWidth] in x and y, depth range 0..20 in front of the eye.
inline Camera makeOrthoCamera(Vec3f eye, float halfWidth, int width, int height) {
    Mat4 ndcToView = Mat4::identity();
    ndcToView.m[0][0] = halfWidth;
    ndcToView.m[1][1] = halfWidth;
    ndcToView.m[2][2] = -10.0; // NDC z -1 -> view z 0, +1 -> -20
    ndcToView.m[3][2] = -10.0;
    Camera c;
    c.ndcToView = ndcToView;
    c.viewToWorld = translation(eye);
    c.worldToView = inverse(c.viewToWorld);
    c.fitFilm(width, height);
    return c;
}

// Perspective camera at `eye` looking down -Z, as Hydra/GfCamera builds it:
// focal length and horizontal aperture in world units, square pixels.
inline Camera makePerspectiveCamera(Vec3f eye, double focalLength, double horizontalAperture, int width, int height,
                                    double nearClip = 0.1, double farClip = 1000.0) {
    const double sx = 2.0 * focalLength / horizontalAperture;
    const double sy = sx * double(width) / double(height);
    Mat4 proj{};
    proj.m[0][0] = sx;
    proj.m[1][1] = sy;
    proj.m[2][2] = (farClip + nearClip) / (nearClip - farClip);
    proj.m[2][3] = -1.0;
    proj.m[3][2] = 2.0 * farClip * nearClip / (nearClip - farClip);
    Camera c;
    c.ndcToView = inverse(proj);
    c.viewToWorld = translation(eye);
    c.worldToView = inverse(c.viewToWorld);
    c.fitFilm(width, height);
    return c;
}

// Rotation by `degrees` about `axis` (row-vector convention, as Mat4).
inline Mat4 rotationAbout(Vec3f axis, double degrees) {
    const double l = std::sqrt(double(dot(axis, axis)));
    const double x = axis.x / l, y = axis.y / l, z = axis.z / l;
    const double a = degrees * double(kPi) / 180.0, c = std::cos(a), s = std::sin(a), t = 1.0 - c;
    const double r[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                            {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                            {t * x * z - s * y, t * y * z + s * x, t * z * z + c}}; // column-vector form
    Mat4 m = Mat4::identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m.m[i][j] = r[j][i];
    return m;
}

// A quad of half-size `half` in the local plane z = `z`, baked into world
// space by `rotation` (like SOP output: the corners carry the rotation).
inline MeshDesc rotatedQuad(float half, float z, const Mat4 &rotation) {
    MeshDesc m = makeQuad(half);
    for (Vec3f &p : m.points)
        p = rotation.transformPoint({p.x, p.y, z});
    return m;
}

struct Orientation {
    Vec3f axis;
    double degrees;
};
inline const Orientation kOrientations[] = {{{1.0f, 1.0f, 0.0f}, 37.0},  {{0.3f, -0.7f, 0.2f}, 61.0},
                                     {{-0.5f, 0.2f, 0.9f}, 23.0}, {{0.8f, 0.1f, -0.4f}, 77.0},
                                     {{0.1f, 0.9f, 0.3f}, 49.0},  {{-0.6f, -0.6f, 0.5f}, 12.0}};

inline void renderToConvergence(Renderer &renderer) {
    while (!renderer.converged())
        renderer.renderPass({});
}

inline Spectrum pixelMean(const Film &film, int x, int y) {
    const size_t i = size_t(y) * size_t(film.width) + size_t(x);
    return film.colorSum[i] / float(film.count[i]);
}

} // namespace nova::test
