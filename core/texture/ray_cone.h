#pragma once

// Ray cones (docs/design/M3.md K8; Akenine-Moeller et al., "Improved Shader
// and Texture LOD Using Ray Cones", JCGT 10(1) 2021; docs/research/m3/
// textures.md 3.2). Every path carries a cone (width at its origin, spread
// angle); at a hit the cone's diameter W becomes the two footprint vectors
// of an ellipse on the surface, which the shading VM turns into texture
// coordinate derivatives by evaluating the coordinates at shifted positions.
// Scattering widens the spread by the surface curvature and, for rough
// lobes, to at least the lobe's roughness (Cycles' rule).

#include "core/math/vec.h"
#include "core/scene/camera.h"

#include <cmath>

namespace nova {

// The cone of the camera ray through image raster position `raster` (at the
// lens center and the frame time): the distance between the origins of the
// rays one pixel apart (orthographic: the pixel size; perspective: the pixel
// at the near plane) and the angle between their directions, averaged over x
// and y.
NOVA_INLINE void cameraCone(const Camera &camera, Vec2f raster, float &width, float &spread) {
    const Ray c = camera.generateRay(camera.rasterToNdc(raster), {0.5f, 0.5f}, camera.frameTime);
    const Ray x = camera.generateRay(camera.rasterToNdc({raster.x + 1.0f, raster.y}), {0.5f, 0.5f}, camera.frameTime);
    const Ray y = camera.generateRay(camera.rasterToNdc({raster.x, raster.y + 1.0f}), {0.5f, 0.5f}, camera.frameTime);
    auto angle = [](Vec3f a, Vec3f b) {
        // atan2 of |a x b| and a.b: accurate for small angles.
        return std::atan2(length(cross(a, b)), dot(a, b));
    };
    width = 0.5f * (length(x.org - c.org) + length(y.org - c.org));
    spread = 0.5f * (angle(c.dir, x.dir) + angle(c.dir, y.dir));
}

// Cone of diameter w along the unit direction d hitting a surface with unit
// normal f: the two footprint vectors, full diameters (OIIO and tex2DGrad
// take pixel-to-pixel derivatives). The first lies along the projected
// direction and is stretched by 1 / |cos|.
NOVA_INLINE void coneFootprint(Vec3f d, Vec3f f, float w, Vec3f &ax, Vec3f &ay) {
    Vec3f h1 = d - f * dot(f, d);
    const float l = length(h1);
    h1 = l > 1e-6f ? h1 * (1.0f / l) : normalize(cross(f, std::fabs(f.x) < 0.9f ? Vec3f{1, 0, 0} : Vec3f{0, 1, 0}));
    const float cosT = nova::fmax(std::fabs(dot(f, d)), 1e-4f); // grazing hits stay finite
    ax = h1 * (w / cosT);
    ay = cross(f, h1) * w;
}

// Mean normal curvature of a triangle from its corner normals (eqs. 6-7):
// 0 for flat shading, 1/R on a tessellated sphere of radius R (with outward
// normals).
NOVA_INLINE float triangleCurvature(Vec3f p0, Vec3f p1, Vec3f p2, Vec3f n0, Vec3f n1, Vec3f n2) {
    auto edge = [](Vec3f pa, Vec3f pb, Vec3f na, Vec3f nb) {
        const Vec3f e = pb - pa;
        const float ee = dot(e, e);
        return ee > 0.0f ? dot(nb - na, e) / ee : 0.0f;
    };
    return (edge(p0, p1, n0, n1) + edge(p1, p2, n1, n2) + edge(p2, p0, n2, n0)) * (1.0f / 3.0f);
}

// The spread after scattering at a hit: `curvature` relative to the normal
// facing the arriving ray, dotNd = dot(that normal, d) < 0, widthAtHit the
// cone's (signed) width there, alpha the sampled lobe's roughness (0 keeps a
// specular cone). A convex mirror (curvature > 0) widens the cone.
NOVA_INLINE float scatterSpread(float spread, float widthAtHit, float curvature, float dotNd, float alpha) {
    const float s = spread - 2.0f * curvature * std::fabs(widthAtHit) / nova::fmin(dotNd, -1e-4f);
    return alpha > 0.0f ? nova::fmax(s, alpha) : s;
}

} // namespace nova
