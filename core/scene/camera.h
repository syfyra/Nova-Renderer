#pragma once

#include "core/math/mat.h"
#include "core/scene/motion.h"
#include "core/scene/ray.h"

#include <cmath>

namespace nova {

// One step of camera motion: rotation and scale as a decomposed transform,
// translation in double (cameras far from the origin).
struct CameraMotionKey {
    SrtTransform rotScale; // translation fields unused
    double tx = 0.0, ty = 0.0, tz = 0.0;
};

// Shirley-Chiu concentric mapping of [0, 1)^2 onto the unit disk; (0.5, 0.5)
// maps to the center.
NOVA_INLINE Vec2f concentricDisk(Vec2f u) {
    const float a = 2.0f * u.x - 1.0f, b = 2.0f * u.y - 1.0f;
    if (a == 0.0f && b == 0.0f)
        return {0.0f, 0.0f};
    float r, phi;
    if (a * a > b * b) {
        r = a;
        phi = (kPi / 4.0f) * (b / a);
    } else {
        r = b;
        phi = (kPi / 2.0f) - (kPi / 4.0f) * (a / b);
    }
    return {r * std::cos(phi), r * std::sin(phi)};
}

// Projective camera given by the host's projection and camera transform,
// with an optional thin lens (depth of field) and camera motion.
// Raster coordinates are image pixels with (0, 0) at the bottom-left corner
// (row 0 is the bottom row, matching Hydra render buffers). The film may be
// a window of the image: image pixel = film pixel + imageOrigin.
struct Camera {
    static constexpr uint32_t kMaxMotionKeys = kMaxMotionSteps;

    Mat4 ndcToView = Mat4::identity();   // inverse of the projection matrix
    Mat4 viewToWorld = Mat4::identity(); // camera transform at the frame time
    Mat4 worldToView = Mat4::identity(); // its inverse (depth AOV); the camera looks down -Z

    // Thin lens: aperture radius in world units (focalLength / (2 fStop),
    // 0 = pinhole) and the distance of the planar focus plane along -Z.
    float lensRadius = 0.0f;
    float focusDistance = 0.0f;

    // Camera motion over the normalized shutter: 0 = static, else >= 2
    // uniform keys. frameTime is the shutter position of the frame itself,
    // used for the first-hit AOVs.
    CameraMotionKey motion[kMaxMotionKeys];
    uint32_t motionKeys = 0;
    float frameTime = 0.0f;

    // ndc = imageRaster * rasterToNdcScale + rasterToNdcOffset
    Vec2f rasterToNdcScale{1.0f, 1.0f};
    Vec2f rasterToNdcOffset{-1.0f, -1.0f};

    // Position of the film in the full image. Rays and sampler seeds are
    // computed from image pixels, so crop and tile renders reproduce the
    // full image exactly.
    int imageOriginX = 0;
    int imageOriginY = 0;

    // Camera-to-world transforms at `count` uniform shutter steps (fewer than
    // two: no motion, viewToWorld applies). Consecutive rotations take the
    // shortest arc. Set frameTime first: with motion, viewToWorld and
    // worldToView become the camera at frameTime, the one the first-hit AOV
    // rays start from, so the depth AOV is measured from it (it differs from
    // the frame's camera when the shutter does not contain the frame).
    void setMotion(const Mat4 *steps, uint32_t count) {
        motionKeys = count >= 2 ? (count < kMaxMotionKeys ? count : kMaxMotionKeys) : 0;
        for (uint32_t k = 0; k < motionKeys; ++k) {
            CameraMotionKey &key = motion[k];
            key.rotScale = decomposeSrt(steps[k]);
            key.tx = steps[k].m[3][0];
            key.ty = steps[k].m[3][1];
            key.tz = steps[k].m[3][2];
            if (k > 0) {
                const SrtTransform &p = motion[k - 1].rotScale;
                SrtTransform &q = key.rotScale;
                if (p.qw * q.qw + p.qx * q.qx + p.qy * q.qy + p.qz * q.qz < 0.0f) {
                    q.qw = -q.qw;
                    q.qx = -q.qx;
                    q.qy = -q.qy;
                    q.qz = -q.qz;
                }
            }
        }
        if (motionKeys >= 2) {
            viewToWorld = transformAt(frameTime);
            worldToView = inverse(viewToWorld);
        }
    }

    // Camera-to-world transform at a shutter time (the keys interpolated as
    // for rays); viewToWorld without motion.
    Mat4 transformAt(float time) const {
        if (motionKeys < 2)
            return viewToWorld;
        float fr;
        const uint32_t s = motionSegment(time, motionKeys, fr);
        const CameraMotionKey &k0 = motion[s], &k1 = motion[s + 1];
        const Affine3f r = interpolateSrt(k0.rotScale, k1.rotScale, fr);
        const Vec3f rows[3] = {r.x, r.y, r.z};
        Mat4 m = Mat4::identity();
        for (int i = 0; i < 3; ++i)
            for (int c = 0; c < 3; ++c)
                m.m[i][c] = rows[i][c];
        m.m[3][0] = k0.tx + fr * (k1.tx - k0.tx);
        m.m[3][1] = k0.ty + fr * (k1.ty - k0.ty);
        m.m[3][2] = k0.tz + fr * (k1.tz - k0.tz);
        return m;
    }

    // Maps the whole film onto NDC [-1, 1]^2.
    void fitFilm(int width, int height) {
        rasterToNdcScale = {2.0f / float(width > 0 ? width : 1), 2.0f / float(height > 0 ? height : 1)};
        rasterToNdcOffset = {-1.0f, -1.0f};
    }

    NOVA_INLINE Vec2f rasterToNdc(Vec2f raster) const {
        return {raster.x * rasterToNdcScale.x + rasterToNdcOffset.x,
                raster.y * rasterToNdcScale.y + rasterToNdcOffset.y};
    }

    // Primary ray through an NDC position, clipped to the near and far planes.
    // `lensSample` picks the point on the lens ((0.5, 0.5) = center), `time`
    // the shutter position. Computed in double precision: the two points that
    // define the pinhole direction are only about `near` apart, so float
    // rounding at large world coordinates would quantize the directions.
    NOVA_INLINE Ray generateRay(Vec2f ndc, Vec2f lensSample = {0.5f, 0.5f}, float time = 0.0f) const {
        double pNear[3], pMid[3], pFar[3];
        if (!unproject(ndc, -1.0, pNear) || !unproject(ndc, 0.0, pMid)) // NDC z = 0 stays finite for an infinite far plane
            return Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 0.0f, 0.0f};
        double d[3] = {pMid[0] - pNear[0], pMid[1] - pNear[1], pMid[2] - pNear[2]};
        normalize3(d);
        if (!(d[2] < 0.0))
            return Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 0.0f, 0.0f};

        // Pinhole ray at the lens plane z = 0 (the origin for perspective
        // cameras, (x, y, 0) for orthographic ones).
        const double sLens = -pNear[2] / d[2];
        double o[3] = {pNear[0] + sLens * d[0], pNear[1] + sLens * d[1], 0.0};
        if (lensRadius > 0.0f && focusDistance > 0.0f) {
            // Thin lens: every lens point sees the pinhole ray's point on the
            // planar focus plane z = -focusDistance (Karma's convention).
            const double sFocus = double(focusDistance) / -d[2];
            const double f[3] = {o[0] + sFocus * d[0], o[1] + sFocus * d[1], -double(focusDistance)};
            const Vec2f l = concentricDisk(lensSample);
            o[0] += double(lensRadius) * l.x;
            o[1] += double(lensRadius) * l.y;
            for (int c = 0; c < 3; ++c)
                d[c] = f[c] - o[c];
            normalize3(d);
        }
        // Planar near and far clipping.
        const double nearDist = -pNear[2], tNear = nearDist / -d[2];
        double tFar = 1e300;
        if (unproject(ndc, 1.0, pFar) && -pFar[2] > nearDist)
            tFar = -pFar[2] / -d[2];
        const double ov[3] = {o[0] + tNear * d[0], o[1] + tNear * d[1], o[2] + tNear * d[2]};

        double ow[3], dw[3];
        if (motionKeys >= 2) {
            float fr;
            const uint32_t s = motionSegment(time, motionKeys, fr);
            const CameraMotionKey &k0 = motion[s], &k1 = motion[s + 1];
            const Affine3f r = interpolateSrt(k0.rotScale, k1.rotScale, fr);
            const double t[3] = {k0.tx + fr * (k1.tx - k0.tx), k0.ty + fr * (k1.ty - k0.ty),
                                 k0.tz + fr * (k1.tz - k0.tz)};
            const Vec3f rows[3] = {r.x, r.y, r.z};
            for (int c = 0; c < 3; ++c) {
                ow[c] = t[c];
                dw[c] = 0.0;
                for (int k = 0; k < 3; ++k) {
                    ow[c] += ov[k] * double(rows[k][c]);
                    dw[c] += d[k] * double(rows[k][c]);
                }
            }
        } else {
            const auto &m = viewToWorld.m;
            for (int c = 0; c < 3; ++c) {
                ow[c] = ov[0] * m[0][c] + ov[1] * m[1][c] + ov[2] * m[2][c] + m[3][c];
                dw[c] = d[0] * m[0][c] + d[1] * m[1][c] + d[2] * m[2][c];
            }
        }
        const double scale = normalize3(dw); // world length per view unit (scaled cameras)
        Ray ray{{float(ow[0]), float(ow[1]), float(ow[2])}, {float(dw[0]), float(dw[1]), float(dw[2])}, 0.0f, kInf};
        if (tFar < 1e300) {
            const double tMax = (tFar - tNear) * scale;
            if (tMax > 0.0 && tMax < double(kInf))
                ray.tMax = float(tMax);
        }
        return ray;
    }

    // Planar camera-space depth (distance along the view axis), the depth
    // convention of Karma and Arnold in Houdini ("linear" depth style).
    NOVA_INLINE float viewDepth(Vec3f worldPos) const { return -worldToView.transformAffine(worldPos).z; }

    // Image raster position (pixels, y up) of a world point seen by the
    // camera at a shutter time; `viewToNdc` is inverse(ndcToView).
    Vec2f worldToRaster(Vec3f worldPos, float time, const Mat4 &viewToNdc) const {
        const Vec3f view = (motionKeys >= 2 ? inverse(transformAt(time)) : worldToView).transformAffine(worldPos);
        const Vec3f ndc = viewToNdc.transformPoint(view);
        return {(ndc.x - rasterToNdcOffset.x) / rasterToNdcScale.x, (ndc.y - rasterToNdcOffset.y) / rasterToNdcScale.y};
    }

private:
    NOVA_FORCEINLINE static double normalize3(double v[3]) {
        const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 0.0) {
            const double inv = 1.0 / l;
            for (int c = 0; c < 3; ++c)
                v[c] *= inv;
        }
        return l;
    }

    // NDC -> view space in double precision. Returns false for points at infinity.
    NOVA_FORCEINLINE bool unproject(Vec2f ndc, double z, double out[3]) const {
        const auto &m = ndcToView.m;
        const double x = ndc.x, y = ndc.y;
        const double w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        if (!(std::fabs(w) > 1e-300))
            return false;
        const double inv = 1.0 / w;
        for (int c = 0; c < 3; ++c)
            out[c] = (x * m[0][c] + y * m[1][c] + z * m[2][c] + m[3][c]) * inv;
        // Finite: x - x is 0 only for finite x (NaN and infinities give NaN).
        return out[0] - out[0] == 0.0 && out[1] - out[1] == 0.0 && out[2] - out[2] == 0.0;
    }
};

} // namespace nova
