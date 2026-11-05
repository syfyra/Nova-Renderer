#pragma once

// Kernel-side light representation and sampling. LightData is a POD derived
// from the host-side LightDesc when the scene is committed.
//
// Conventions follow UsdLux and Karma CPU 22, checked with renders
// (tests/karma/compare_lights.py, probe_dome_mapping.py):
// - Area lights have radiance color * intensity * 2^exposure, divided by
//   their world-space area with `normalize`. Rect and disk lights emit from
//   their -Z side (both sides if two-sided), cylinder lights outwards (no end
//   caps), sphere lights outwards and, for points inside them, inwards (Karma
//   lights the inside of sphere lights, not of cylinders).
// - Distant lights: angles above 180 degrees act as 180 (a hemisphere).
// - Negative intensities subtract light; such lights are sampled in
//   proportion to the magnitude of their power.
// - A sphere light with treatAsPoint is a point light whose radiant
//   intensity is color * intensity * 2^exposure (a quarter of it with
//   `normalize`); the radius is ignored.
// - Lights never block rays. Camera rays see only lights marked camera
//   visible (karma:light:renderlightgeo), which are then opaque (alpha 1;
//   the back of a single-sided light is black). Rays scattered by surfaces
//   see every light and collect its emission, weighted by MIS.
// - ShapingAPI: focus and focusTint as documented; the cone edge is
//   1 - smoothstep over cos(theta) between cos(angle * (1 - softness)) and
//   cos(angle). The UsdLux docs describe the blend in the angle instead,
//   which Karma does not do. A cone angle of exactly 90 degrees (the schema
//   fallback) means no cone.
// - Dome textures: latlong after OpenEXR; rect textures: top-left image
//   corner at the light's (+X, +Y) corner with the default flip.

#include "core/lights/light_strategy.h"
#include "core/lights/light_tree.h"
#include "core/lights/shapes.h"
#include "core/math/distribution.h"
#include "core/math/frame.h"
#include "core/math/sampling.h"
#include "core/math/spectrum.h"
#include "core/scene/light.h"

#include <cstdint>

namespace nova {

struct LightData {
    LightType type = LightType::Distant;
    // Area lights and cone-shaped distant lights: radiance. Point lights:
    // radiant intensity. Zero-angle distant lights: irradiance.
    Spectrum emission{0.0f, 0.0f, 0.0f};
    float diffuse = 1.0f;               // UsdLux diffuse multiplier
    float specular = 1.0f;              // UsdLux specular multiplier
    bool delta = false;                 // point light or zero-angle distant light
    bool cameraVisible = false;         // camera rays see the light
    bool twoSided = false;              // rect and disk lights emit from both sides

    // Shapes, in world space. Rect: center and half-extent vectors along the
    // local X and Y axes. Disk: center and radius vectors along local X and
    // Y. Cylinder: center, half-length vector along local X (axisU) and
    // radius vectors along local Y and Z (axisV, axisW). Sphere and point:
    // center and radius.
    Vec3f position{0.0f, 0.0f, 0.0f};
    Vec3f axisU{1.0f, 0.0f, 0.0f};
    Vec3f axisV{0.0f, 1.0f, 0.0f};
    Vec3f axisW{0.0f, 0.0f, 1.0f};
    Vec3f normal{0.0f, 0.0f, -1.0f};    // rect and disk: unit emission direction (local -Z)
    float radius = 0.0f;
    float area = 0.0f;                  // world-space emitting area (one side)
    RectFrame rect{};                   // rect: corner and edges for sampling (makeLightData)

    // Distant lights.
    Vec3f direction{0.0f, 0.0f, 1.0f};  // unit vector towards the light
    float oneMinusCosThetaMax = 0.0f;   // cone of half-angle angle/2 (up to 1)

    // UsdLuxShapingAPI around `axis` (local -Z).
    bool shaped = false;
    Vec3f axis{0.0f, 0.0f, -1.0f};
    float cosConeCut = -2.0f;           // no emission below this (-2: no cone)
    float cosConeStart = -2.0f;         // full emission above this
    float focus = 0.0f;
    Spectrum focusTint{0.0f, 0.0f, 0.0f};

    // Texture (row 0 = top): latlong environment of a dome, or the image on
    // a rect light. Multiplies the emission.
    const Spectrum *texels = nullptr;
    uint32_t texWidth = 0, texHeight = 0;
    float texMeanLuminance = 1.0f;      // power estimate of textured rects
    RectTextureFlip rectFlip = RectTextureFlip::FlipX;

    // Dome: orthonormal world axes of its frame, and the distribution that
    // samples (u, v). The distribution is empty if the dome is left to BSDF
    // sampling (constant or nearly constant environments).
    Vec3f domeX{1.0f, 0.0f, 0.0f}, domeY{0.0f, 1.0f, 0.0f}, domeZ{0.0f, 0.0f, 1.0f};
    Distribution2DView distribution;

    // Volume lights (docs/design/M8.md D13): the volume, whose cells the
    // integrator samples (VolumeRecord::lightCells), and its power.
    uint32_t volume = 0;
    float power = 0.0f;

    // Emissive triangles (M9 D14): corners position, position + axisU and
    // position + axisV in world space (the mesh's winding), two-sided;
    // `emission` is the power estimate only, the material gives the radiance
    // where a sample lands. The triangle: mesh, instance, primitive.
    uint32_t mesh = 0, instance = 0, prim = 0;

    // Multiplies the light's selection weight (Karma's sampling quality, M9
    // D18). 0 turns the illumination of a light other than an emissive
    // triangle off, as in Karma (measured): light sampling never picks it and
    // no ray collects it, but the camera still sees it (illuminates()).
    float selection = 1.0f;
    NOVA_INLINE bool illuminates() const { return selection > 0.0f; }
    // Equiangular distance sampling along rays in volumes (M9 D17): point,
    // spot, sphere and area lights that opted in.
    bool equiangular = false;
    // Karma's per-light sampling mode Uniform (M9 D18): the light stays out
    // of the light tree and is chosen with the lights at infinity.
    bool outsideTree = false;
    // Shadow linking (M9 D15): the category of the objects that shadow the
    // light (0xFFFF: all of them). Continuations of non-delta events never
    // collect such lights: most are sampled by next-event estimation alone
    // (neeOnly; it covers their whole emission, as every light that
    // illuminates has a selection weight above 0), a dome's BSDF samples are
    // shadow rays of their own (core/integrator/path.h).
    uint16_t shadowCategory = 0xFFFFu;
    NOVA_INLINE bool shadowLinked() const { return shadowCategory != 0xFFFFu; }
    NOVA_INLINE bool neeOnly() const { return shadowLinked() && type != LightType::Dome; }
};

// Applies the UsdLux conventions (see above) and transforms the shape to
// world space. Dome lights ignore `normalize`; distant lights divide by
// pi sin^2 of their half-angle. A dome's sampling distribution and a rect
// texture's mean luminance (textureMeanLuminance) are attached by the scene.
LightData makeLightData(const LightDesc &desc);

// Mean luminance of a texture, the power estimate of a textured rect light.
float textureMeanLuminance(const Image &texture);

// Estimated power for light selection (its magnitude, for lights with a
// negative intensity). Lights at infinity count the flux through a disk of
// the scene's bounding radius. A dome without a sampling distribution
// (constant environment) returns 0: BSDF sampling alone is the better
// strategy for it.
float lightPower(const LightData &light, float sceneRadius);

// Sampling function of a dome texture over (u, v): the largest luminance
// within the texel's bilinear footprint, minus the solid-angle weighted mean
// (MIS compensation, Karlik et al. 2019), times sin(theta). BSDF sampling
// covers the average, light sampling the brighter-than-average part. Maps
// wider than 2048 texels are sampled on a coarser grid whose cells take the
// maximum over their exact footprint. Returns false if nothing is left to
// sample.
bool buildDomeDistribution(const Image &texture, Distribution2D &out);

NOVA_INLINE bool isAreaLight(LightType type) {
    return type == LightType::Sphere || type == LightType::Rect || type == LightType::Disk ||
           type == LightType::Cylinder;
}

// Whether the side of an area light that a ray or light sample meets emits:
// the front, both sides of two-sided rects and disks, and the inside of
// spheres.
NOVA_INLINE bool emitsFrom(const LightData &light, bool front) {
    return front || light.twoSided || light.type == LightType::Sphere;
}

// Whether `dir` lies within the cone of a distant light (half-angle up to 90
// degrees). Comparing sin^2 through the cross product keeps small cones exact
// where 1 - cos would cancel (0.05 degrees: 1 - cos = 1e-7).
NOVA_INLINE bool inDistantCone(const LightData &light, Vec3f dir) {
    const float c = dot(dir, light.direction);
    if (!(c > 0.0f))
        return false;
    const float omc = light.oneMinusCosThetaMax;
    const Vec3f x = cross(dir, light.direction);
    return dot(x, x) <= omc * (2.0f - omc) * dot(dir, dir);
}

// ShapingAPI factor for light leaving the light along `emitDir`.
NOVA_INLINE Spectrum shapingFactor(const LightData &light, Vec3f emitDir) {
    if (!light.shaped)
        return {1.0f, 1.0f, 1.0f};
    const float c = dot(light.axis, emitDir);
    if (c < light.cosConeCut)
        return {0.0f, 0.0f, 0.0f};
    float cone = 1.0f;
    if (c < light.cosConeStart) {
        const float t = (c - light.cosConeCut) / (light.cosConeStart - light.cosConeCut);
        cone = t * t * (3.0f - 2.0f * t);
    }
    if (light.focus > 0.0f) {
        const float f = std::pow(std::fabs(c), light.focus);
        const Spectrum one{1.0f, 1.0f, 1.0f};
        return (light.focusTint + (one - light.focusTint) * f) * cone;
    }
    return {cone, cone, cone};
}

// Bilinear lookup with texel centers at ((i + 0.5) / w, (j + 0.5) / h). u
// and v either wrap around (periodic) or clamp to the border texels.
NOVA_INLINE Spectrum bilinearTexel(const Spectrum *texels, uint32_t width, uint32_t height, Vec2f uv, bool wrapU,
                                   bool wrapV) {
    const int w = int(width), h = int(height);
    const float x = uv.x * float(w) - 0.5f, y = uv.y * float(h) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    int x0 = int(fx), y0 = int(fy);
    int x1 = x0 + 1, y1 = y0 + 1;
    if (wrapU) {
        x0 = (x0 % w + w) % w;
        x1 = (x1 % w + w) % w;
    } else {
        x0 = x0 < 0 ? 0 : (x0 >= w ? w - 1 : x0);
        x1 = x1 < 0 ? 0 : (x1 >= w ? w - 1 : x1);
    }
    if (wrapV) {
        y0 = (y0 % h + h) % h;
        y1 = (y1 % h + h) % h;
    } else {
        y0 = y0 < 0 ? 0 : (y0 >= h ? h - 1 : y0);
        y1 = y1 < 0 ? 0 : (y1 >= h ? h - 1 : y1);
    }
    const Spectrum *row0 = texels + size_t(y0) * size_t(w);
    const Spectrum *row1 = texels + size_t(y1) * size_t(w);
    return (row0[x0] * (1.0f - tx) + row0[x1] * tx) * (1.0f - ty) + (row1[x0] * (1.0f - tx) + row1[x1] * tx) * ty;
}

// ---------------------------------------------------------------------------
// Dome lights: latlong mapping after UsdLux / OpenEXR. In the dome's frame,
// +Y is the pole (v = 0), the image center (u = 0.5) looks down +Z and
// u = 0.25 looks down +X: u = 0.5 - atan2(x, z) / 2 pi, v = acos(y) / pi.
// ---------------------------------------------------------------------------

NOVA_INLINE Vec2f domeUv(const LightData &light, Vec3f dir) {
    const float x = dot(dir, light.domeX), y = dot(dir, light.domeY), z = dot(dir, light.domeZ);
    float u = 0.5f - std::atan2(x, z) * kInvTwoPi;
    u = u < 0.0f ? u + 1.0f : (u >= 1.0f ? u - 1.0f : u);
    const float len = std::sqrt(x * x + y * y + z * z);
    const float cosTheta = len > 0.0f ? nova::fmin(1.0f, nova::fmax(-1.0f, y / len)) : 1.0f;
    return {u, std::acos(cosTheta) * kInvPi};
}

// Unit world direction of (u, v); also returns sin(theta).
NOVA_INLINE Vec3f domeDirection(const LightData &light, Vec2f uv, float &sinTheta) {
    const float phi = kPi * (1.0f - 2.0f * uv.x);
    const float theta = kPi * uv.y;
    sinTheta = std::sin(theta);
    const Vec3f local{sinTheta * std::sin(phi), std::cos(theta), sinTheta * std::cos(phi)};
    return normalize(light.domeX * local.x + light.domeY * local.y + light.domeZ * local.z);
}

NOVA_INLINE Spectrum domeTexel(const LightData &light, Vec2f uv) {
    return bilinearTexel(light.texels, light.texWidth, light.texHeight, uv, true, false); // around the pole axis
}

NOVA_INLINE Spectrum domeRadiance(const LightData &light, Vec3f dir) {
    return light.texels ? light.emission * domeTexel(light, domeUv(light, dir)) : light.emission;
}

// Solid-angle density with which the dome's distribution produces `dir`.
NOVA_INLINE float domePdf(const LightData &light, Vec3f dir) {
    if (!(light.distribution.integral > 0.0f))
        return 0.0f;
    const Vec2f uv = domeUv(light, dir);
    const float sinTheta = std::sin(kPi * uv.y);
    return sinTheta > 0.0f ? light.distribution.pdf(uv) / (2.0f * kPi * kPi * sinTheta) : 0.0f;
}

// ---------------------------------------------------------------------------
// Rect lights
// ---------------------------------------------------------------------------

// Corner of the rect: the shape is corner + a * 2 axisU + b * 2 axisV, a, b in [0, 1].
NOVA_INLINE Vec3f rectCorner(const LightData &light) { return light.position - light.axisU - light.axisV; }

// Texture coordinates (u, v from the top row) of a point on the rect.
NOVA_INLINE Vec2f rectUv(const LightData &light, Vec3f p) {
    // Coordinates along the edges ex = 2 axisU, ey = 2 axisV, which a sheared
    // transform leaves non-orthogonal: q = a ex + b ey.
    const Vec3f q = p - rectCorner(light);
    const Vec3f ex = light.axisU * 2.0f, ey = light.axisV * 2.0f;
    const Vec3f n = cross(ex, ey);
    const float nn = dot(n, n);
    const float a = dot(cross(q, ey), n) / nn; // local x / width + 1/2
    const float b = dot(cross(ex, q), n) / nn; // local y / height + 1/2
    switch (light.rectFlip) {
    case RectTextureFlip::None:
        return {a, 1.0f - b};
    case RectTextureFlip::FlipX:
        return {1.0f - a, 1.0f - b};
    case RectTextureFlip::FlipY:
        return {a, b};
    default:
        return {1.0f - a, b};
    }
}

NOVA_INLINE Spectrum rectTexel(const LightData &light, Vec3f p) {
    if (!light.texels)
        return {1.0f, 1.0f, 1.0f};
    // Karma's texture lookup wraps at the rect's border (a half-white texture
    // gives 0.51 / 0.49 in the border pixels), so Nova wraps u and v as well.
    return bilinearTexel(light.texels, light.texWidth, light.texHeight, rectUv(light, p), true, true);
}

// ---------------------------------------------------------------------------
// Light sampling (next-event estimation)
// ---------------------------------------------------------------------------

struct LightSample {
    Vec3f wi;          // unit direction towards the light
    Spectrum Li;       // radiance along wi (irradiance for delta lights; 1 for emissive triangles)
    float pdf;         // solid-angle density of wi (1 for delta lights, 0 = no sample)
    float distance;    // shadow ray length; kInf for lights at infinity
    bool delta;
    // Emissive triangles (M9 D14): the point's barycentrics, where the
    // material's emission is evaluated once the shadow ray is unoccluded.
    Vec2f bary{0.0f, 0.0f};
};

NOVA_INLINE LightSample sampleDistantLight(const LightData &light, Vec2f u) {
    LightSample s;
    s.distance = kInf;
    s.delta = light.delta;
    if (light.delta) {
        s.wi = light.direction;
        s.Li = light.emission;
        s.pdf = 1.0f;
        return s;
    }
    const Vec3f local = sampleUniformCone(u, light.oneMinusCosThetaMax);
    s.wi = normalize(Frame::fromNormal(light.direction).toWorld(local));
    s.Li = light.emission;
    s.pdf = 1.0f / uniformConeSolidAngle(light.oneMinusCosThetaMax);
    return s;
}

// Shadow rays stop just short of the light, so geometry the light sits on
// does not shadow it. The light is sampled from the shadow ray's (offset)
// origin, so the margin is measured from where the ray starts.
constexpr float kShadowRayShortening = 1.0f - 1e-4f;

// The float error of a hit distance grows with the coordinates of the
// triangle hit (Embree intersects relative to its corners), not with the
// distance: a light lying on a large rotated plane can be a few ulps of the
// plane's coordinates behind it. Tolerances for that error are this many
// float ulps of the coordinates involved.
constexpr float kIntersectionUlps = 32.0f;
constexpr float kFloatUlp = 1.0f / 8388608.0f; // 2^-23

// Length of a shadow ray towards a light that sampleLight() placed at
// `distance` (already shortened): also at least `tolerance` short of the
// light (Scene::intersectionTolerance()), but never shorter than half the way.
NOVA_INLINE float shadowRayLength(float distance, float tolerance) {
    return nova::fmin(distance, nova::fmax(distance / kShadowRayShortening - tolerance, 0.5f * distance));
}

// How far a ray that hit a surface at tHit looks for lights: just past the
// surface by the same margins, so a light lying on the surface counts for
// BSDF samples exactly when next-event estimation counts it (otherwise MIS
// loses part of the light).
NOVA_INLINE float lightSearchLimit(float tHit, float tolerance = 0.0f) {
    return nova::fmax(tHit / kShadowRayShortening, tHit + tolerance);
}

NOVA_INLINE LightSample noLightSample() {
    LightSample s;
    s.wi = {0.0f, 0.0f, 1.0f};
    s.Li = {0.0f, 0.0f, 0.0f};
    s.pdf = 0.0f;
    s.distance = 0.0f;
    s.delta = false;
    return s;
}

// Light sample for next-event estimation from P, the origin of the shadow
// ray (the surface point offset by offsetRayOrigin()).
NOVA_INLINE LightSample sampleLight(const LightData &light, Vec3f P, Vec2f u) {
    if (light.type == LightType::Distant)
        return sampleDistantLight(light, u);
    if (light.type == LightType::Dome) {
        if (!(light.distribution.integral > 0.0f))
            return noLightSample(); // left to BSDF sampling
        float pdfUv = 0.0f, sinTheta = 0.0f;
        const Vec2f uv = light.distribution.sample(u, pdfUv);
        LightSample s;
        s.wi = domeDirection(light, uv, sinTheta);
        if (!(pdfUv > 0.0f) || !(sinTheta > 0.0f))
            return noLightSample();
        s.pdf = pdfUv / (2.0f * kPi * kPi * sinTheta);
        s.Li = light.texels ? light.emission * domeTexel(light, uv) : light.emission;
        s.distance = kInf;
        s.delta = false;
        return s;
    }

    if (light.delta) { // point light
        LightSample s;
        const Vec3f d = light.position - P;
        const float dist2 = dot(d, d);
        if (!(dist2 > 0.0f))
            return noLightSample();
        const float dist = std::sqrt(dist2);
        s.wi = d / dist;
        s.Li = light.emission * shapingFactor(light, -s.wi) * (1.0f / dist2);
        s.pdf = 1.0f;
        s.distance = dist * kShadowRayShortening;
        s.delta = true;
        return s;
    }

    ShapeSample ss;
    Vec2f bary{0.0f, 0.0f};
    switch (light.type) {
    case LightType::Sphere:
        ss = sampleSphere(light.position, light.radius, P, u);
        break;
    case LightType::Triangle:
        ss = sampleTriangle(light.position, light.axisU, light.axisV, P, u, bary);
        break;
    case LightType::Rect:
        ss = sampleRectangle(light.rect, P, u);
        ss.n = light.normal;
        break;
    case LightType::Disk:
        ss = sampleDisk(light.position, light.axisU, light.axisV, P, u);
        ss.n = light.normal;
        break;
    default: // cylinder
        ss = sampleCylinder(light.position, light.axisU, light.axisV, light.axisW, P, u);
        break;
    }
    const Vec3f d = ss.p - P;
    const float dist2 = dot(d, d);
    if (!(ss.pdf > 0.0f) || !(dist2 > 0.0f))
        return noLightSample();
    LightSample s;
    const float dist = std::sqrt(dist2);
    s.wi = d / dist;
    s.pdf = ss.pdf;
    s.distance = dist * kShadowRayShortening;
    s.delta = false;
    if (light.type == LightType::Triangle) {
        s.Li = {1.0f, 1.0f, 1.0f}; // the emission follows at the shadow ray's end
        s.bary = bary;
        return s;
    }
    // The front emits (P on the normal's side), the back only if emitsFrom().
    const bool emits = emitsFrom(light, dot(ss.n, s.wi) < 0.0f);
    s.Li = emits ? light.emission * shapingFactor(light, -s.wi) : Spectrum{0.0f, 0.0f, 0.0f};
    if (emits && light.type == LightType::Rect)
        s.Li *= rectTexel(light, ss.p);
    return s;
}

// ---------------------------------------------------------------------------
// Combining strategies
// ---------------------------------------------------------------------------

// Weight of a light sample whose direction the BSDF would sample with
// `bsdfPdf` (lightPdf includes the selection probability).
NOVA_INLINE float lightSampleWeight(LightStrategy strategy, float lightPdf, float bsdfPdf, bool delta) {
    if (delta)
        return 1.0f; // BSDF samples never reach delta lights
    switch (strategy) {
    case LightStrategy::MIS:
        return powerHeuristic(lightPdf, bsdfPdf);
    case LightStrategy::LightOnly:
        return 1.0f;
    default:
        return 0.0f;
    }
}

// Weight of a BSDF sample that reached a light which next-event estimation
// picks with density `lightPdf` (selection included). Lights that are never
// light-sampled (lightPdf 0) and rays from delta events (bsdfPdf < 0: a
// mirror or a smooth interface, which light sampling cannot reach) get 1.
NOVA_INLINE float bsdfSampleWeight(LightStrategy strategy, float bsdfPdf, float lightPdf) {
    if (!(lightPdf > 0.0f) || bsdfPdf < 0.0f)
        return 1.0f;
    switch (strategy) {
    case LightStrategy::MIS:
        return powerHeuristic(bsdfPdf, lightPdf);
    case LightStrategy::LightOnly:
        return 0.0f;
    default:
        return 1.0f;
    }
}

// Whether a BSDF sample's weight depends on the lights' selection
// probabilities (MIS of a non-delta event); the light tree's are not free.
NOVA_INLINE bool misNeedsPmf(LightStrategy strategy, float bsdfPdf) {
    return bsdfPdf >= 0.0f && strategy != LightStrategy::BsdfOnly;
}

// ---------------------------------------------------------------------------
// Lights reached by rays
// ---------------------------------------------------------------------------

// The light's multiplier for a ray scattered by a surface: its diffuse and
// specular multipliers in the proportion of the scattering event's
// diffuse-class share (1: diffuse, 0: specular).
NOVA_INLINE float lightMultiplier(const LightData &light, float diffuseShare) {
    return diffuseShare * light.diffuse + (1.0f - diffuseShare) * light.specular;
}

// The functions that sum light over several lights take an optional sink,
// `sink(i, L_i)`, which receives each light's share of the sum (light path
// expressions give every light its own event, docs/design/M6.md D4).
struct NoLightSink {
    NOVA_INLINE void operator()(uint32_t, const Spectrum &) const {}
};

// The attenuation of a light crossed at distance t along the ray (volumes
// in front of it, docs/design/M8.md D9): `atten(i, t)`; none by default.
struct NoAtten {
    NOVA_INLINE Spectrum operator()(uint32_t, float) const { return {1.0f, 1.0f, 1.0f}; }
};

// Radiance from the lights at infinity along `dir` for a ray scattered by a
// surface with BSDF density `bsdfPdf` (< 0: a delta event) that escaped the
// scene, scaled by the lights' multipliers for the event's `diffuseShare`.
// `pmf(i)` is light i's selection probability at the vertex the ray left
// (asked only where MIS needs it).
template <typename Pmf, typename Sink = NoLightSink>
NOVA_INLINE Spectrum escapedRadiance(const LightData *lights, const Pmf &pmf, uint32_t count, Vec3f dir,
                                     float bsdfPdf, LightStrategy strategy, float diffuseShare = 1.0f,
                                     const Sink &sink = Sink{}) {
    Spectrum L{0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < count; ++i) {
        const LightData &light = lights[i];
        // Off (sampling quality 0, M9 D18), unlinked, or shadow-linked after a
        // non-delta event (D15): only the light's own samples see its shadow
        // set (LightData::shadowCategory).
        if (!light.illuminates() || !pmf.links(i) || (light.shadowLinked() && bsdfPdf >= 0.0f))
            continue;
        if (light.type == LightType::Dome) {
            const float weight =
                bsdfSampleWeight(strategy, bsdfPdf, misNeedsPmf(strategy, bsdfPdf) ? pmf(i) * domePdf(light, dir) : 0.0f);
            if (weight > 0.0f) {
                const Spectrum Li = domeRadiance(light, dir) * (lightMultiplier(light, diffuseShare) * weight);
                L += Li;
                sink(i, Li);
            }
        } else if (light.type == LightType::Distant && !light.delta && inDistantCone(light, dir)) {
            const float lightPdf =
                misNeedsPmf(strategy, bsdfPdf) ? pmf(i) / uniformConeSolidAngle(light.oneMinusCosThetaMax) : 0.0f;
            const Spectrum Li =
                light.emission * (lightMultiplier(light, diffuseShare) * bsdfSampleWeight(strategy, bsdfPdf, lightPdf));
            L += Li;
            sink(i, Li);
        }
    }
    return L;
}

// What a camera ray that escaped the scene sees: camera-visible domes and
// cone-shaped distant lights, unweighted. `visible` reports whether any
// light was seen (it then counts as opaque).
template <typename Sink = NoLightSink>
NOVA_INLINE Spectrum cameraEnvironment(const LightData *lights, uint32_t count, Vec3f dir, bool &visible,
                                       const Sink &sink = Sink{}) {
    Spectrum L{0.0f, 0.0f, 0.0f};
    visible = false;
    for (uint32_t i = 0; i < count; ++i) {
        const LightData &light = lights[i];
        if (!light.cameraVisible)
            continue;
        if (light.type == LightType::Dome) {
            const Spectrum Li = domeRadiance(light, dir);
            L += Li;
            sink(i, Li);
            visible = true;
        } else if (light.type == LightType::Distant && !light.delta && inDistantCone(light, dir)) {
            L += light.emission;
            sink(i, light.emission);
            visible = true;
        }
    }
    return L;
}

// Intersection of a ray with an area light's shape in (0, tMax), from either
// side. `n` is the normal of the emitting side (outwards for spheres and
// cylinders, the emission direction for rects and disks); `front` tells
// whether the ray meets that side.
NOVA_INLINE bool intersectAreaLight(const LightData &light, Vec3f o, Vec3f d, float tMax, float &t, Vec3f &n,
                                    bool &front) {
    switch (light.type) {
    case LightType::Sphere:
        if (!intersectSphere(light.position, light.radius, o, d, tMax, t))
            return false;
        n = normalize(o + d * t - light.position);
        front = dot(n, d) < 0.0f; // both sides emit (emitsFrom), so rounding here is harmless
        return true;
    case LightType::Rect:
        n = light.normal;
        front = dot(n, d) < 0.0f;
        return intersectParallelogram(rectCorner(light), light.axisU * 2.0f, light.axisV * 2.0f, o, d, tMax, t);
    case LightType::Disk:
        n = light.normal;
        front = dot(n, d) < 0.0f;
        return intersectDisk(light.position, light.axisU, light.axisV, o, d, tMax, t);
    case LightType::Cylinder:
        return intersectCylinder(light.position, light.axisU, light.radius, o, d, tMax, t, n, front);
    default:
        return false;
    }
}

// Density with which next-event estimation from `o` produces the point p
// (normal n) on an area light, without the selection probability.
NOVA_INLINE float areaLightPdf(const LightData &light, Vec3f o, Vec3f p, Vec3f n) {
    switch (light.type) {
    case LightType::Sphere:
        return sphereSamplePdf(light.position, light.radius, o, p, n);
    case LightType::Rect:
        return rectangleSamplePdf(light.rect, o, p);
    default: // disk and cylinder: uniform area sampling
        return areaToSolidAngle(1.0f / light.area, o, p, n);
    }
}

// Density with which next-event estimation from `o` produces the point p on
// an emissive triangle (M9 D14), without the selection probability.
NOVA_INLINE float emitterPdf(const LightData &light, Vec3f o, Vec3f p) {
    return triangleSamplePdf(light.position, light.axisU, light.axisV, light.normal, light.area, o, p);
}

// Emitted radiance of an area light at the point p towards direction -d.
NOVA_INLINE Spectrum areaLightRadiance(const LightData &light, Vec3f p, Vec3f d) {
    Spectrum Le = light.emission * shapingFactor(light, -d);
    if (light.type == LightType::Rect)
        Le *= rectTexel(light, p);
    return Le;
}

// Calls f(i, t, n, front) for every emitting side of an area light that the
// ray (origin o, unit direction d) crosses in (0, tMax), unordered. With a
// light tree, only the lights whose subtree boxes the ray crosses are tested
// (docs/design/M9.md D13), and only subtrees with crossable lights; lights the
// tree leaves out have no power. Without, every light is. Lights whose
// illumination is off (sampling quality 0, D18) are never crossed.
template <typename F>
NOVA_INLINE void forEachAreaLightCrossing(const LightData *lights, uint32_t count, const LightTreeView *tree, Vec3f o,
                                          Vec3f d, float tMax, const F &f) {
    const auto test = [&](uint32_t i) {
        const LightData &light = lights[i];
        if (!isAreaLight(light.type) || light.delta || !light.illuminates())
            return;
        float t = 0.0f;
        Vec3f n;
        bool front = false;
        if (intersectAreaLight(light, o, d, tMax, t, n, front) && emitsFrom(light, front))
            f(i, t, n, front);
    };
    if (!tree) {
        for (uint32_t i = 0; i < count; ++i)
            test(i);
        return;
    }
    const Vec3f inv{d.x != 0.0f ? 1.0f / d.x : 1e30f, d.y != 0.0f ? 1.0f / d.y : 1e30f, d.z != 0.0f ? 1.0f / d.z : 1e30f};
    uint32_t stack[96];
    int top = 0;
    // An empty tree (every light outside it) still has the lights below.
    if (tree->nodeCount > 0)
        stack[top++] = 0;
    while (top > 0) {
        const LightTreeNode &node = tree->nodes[stack[--top]];
        if (!(node.flags & LightTreeNode::kCrossable))
            continue; // emissive triangles only: rays find them as geometry (M9 D14)
        // Slab test against the node's box (padded by an ulp-scale margin).
        float t0 = 0.0f, t1 = tMax;
        for (int a = 0; a < 3; ++a) {
            const float lo = (node.lo[a] - o[a]) * inv[a], hi = (node.hi[a] - o[a]) * inv[a];
            const float tn = nova::fmin(lo, hi), tf = nova::fmax(lo, hi) * 1.0000005f;
            t0 = nova::fmax(t0, tn);
            t1 = nova::fmin(t1, tf);
        }
        if (t0 > t1)
            continue;
        if (node.flags & LightTreeNode::kLeaf) {
            test(node.child);
        } else if (top + 2 <= 96) {
            stack[top++] = node.child;
            stack[top++] = uint32_t(&node - tree->nodes) + 1;
        }
    }
    // Area lights kept out of the tree (Karma's sampling mode Uniform, D18).
    for (uint32_t i = 0; i < tree->infiniteCount; ++i)
        test(tree->infinite[i]);
}

// Radiance from the area lights that a BSDF-sampled ray (origin o, unit
// direction d) crosses before tMax (lightSearchLimit() of the surface hit),
// weighted against next-event estimation. Lights never block rays, so every
// light along the segment contributes. `pdfOrigin` is the path's MIS vertex,
// where next-event estimation sampled the lights: the ray's origin, except
// past null events (fake-caustic pass-throughs), which move the ray on but
// keep the vertex (M4 3.3).
template <typename Pmf, typename Sink = NoLightSink, typename Atten = NoAtten>
NOVA_INLINE Spectrum areaLightsAlongRay(const LightData *lights, const Pmf &pmf, uint32_t count,
                                        const LightTreeView *tree, Vec3f o, Vec3f d, float tMax, Vec3f pdfOrigin,
                                        float bsdfPdf, LightStrategy strategy, float diffuseShare = 1.0f,
                                        const Sink &sink = Sink{}, const Atten &atten = Atten{}) {
    Spectrum L{0.0f, 0.0f, 0.0f};
    forEachAreaLightCrossing(lights, count, tree, o, d, tMax, [&](uint32_t i, float t, Vec3f n, bool) {
        const LightData &light = lights[i];
        if (!pmf.links(i) || (light.neeOnly() && bsdfPdf >= 0.0f))
            return; // unlinked, or a shadow-linked light that only NEE samples (M9 D15)
        const Vec3f p = o + d * t;
        const float weight = bsdfSampleWeight(
            strategy, bsdfPdf, misNeedsPmf(strategy, bsdfPdf) ? pmf(i) * areaLightPdf(light, pdfOrigin, p, n) : 0.0f);
        if (weight > 0.0f) {
            const Spectrum Li =
                areaLightRadiance(light, p, d) * (lightMultiplier(light, diffuseShare) * weight) * atten(i, t);
            L += Li;
            sink(i, Li);
        }
    });
    return L;
}

// The area lights a ray (origin o, unit direction d) crosses before tMax,
// as areaLightsAlongRay() finds them: their distances (the same values) and
// indices, unsorted; at most `max`. Returns how many.
NOVA_INLINE int areaLightCrossings(const LightData *lights, uint32_t count, const LightTreeView *tree, Vec3f o, Vec3f d,
                                   float tMax, float *t, uint32_t *index, int max) {
    int n = 0;
    forEachAreaLightCrossing(lights, count, tree, o, d, tMax, [&](uint32_t i, float th, Vec3f, bool) {
        if (n < max) {
            t[n] = th;
            index[n++] = i;
        }
    });
    return n;
}

// A light lying on the surface a camera ray hits (a rect snapped to a wall)
// meets the ray at the same distance up to float error. The tie tolerance is
// that error: kIntersectionUlps float ulps of the coordinates involved (the
// ray origin, the hit point and the corners of the triangle hit), divided by
// the cosine at the light (clamped at kCameraTieMinCos). Unlike the relative
// band of lightSearchLimit() it does not grow with the distance to the
// camera, so lights a little behind a wall stay hidden from far away.
constexpr float kCameraTieMinCos = 1e-3f;

// The nearest camera-visible area light that a camera ray (origin o, unit
// direction d) meets: its index, distance, emitting-side normal and side.
// With `surface`, the ray hits a surface at tEnd and a light within the tie
// tolerance of it counts as lying on it: the light wins if the ray meets an
// emitting side, otherwise the surface does; `extent` is the hit triangle's
// SurfaceHit::extent. Without, the ray ends at tEnd. Returns false if there
// is none.
NOVA_INLINE bool nearestCameraLight(const LightData *lights, uint32_t count, Vec3f o, Vec3f d, float tEnd,
                                    bool surface, float extent, uint32_t &index, float &tHit, Vec3f &nHit,
                                    bool &frontHit) {
    float tieScale = 0.0f, tLimit = tEnd;
    if (surface) {
        tieScale = kIntersectionUlps * kFloatUlp *
                   nova::fmax(extent, nova::fmax(maxComponent(absolute(o)), maxComponent(absolute(o + d * tEnd))));
        tLimit = tEnd + tieScale / kCameraTieMinCos;
    }
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        const LightData &light = lights[i];
        if (!light.cameraVisible || !isAreaLight(light.type) || light.delta)
            continue;
        float t = 0.0f;
        Vec3f n;
        bool front = false;
        if (!intersectAreaLight(light, o, d, tLimit, t, n, front))
            continue;
        if (surface) {
            const float tie = tieScale / nova::fmax(std::fabs(dot(d, n)), kCameraTieMinCos);
            if (t > tEnd + tie || (t > tEnd - tie && !emitsFrom(light, front)))
                continue; // behind the surface, or lying on it with its dark side towards the camera
        }
        tLimit = tHit = t;
        nHit = n;
        frontHit = front;
        index = i;
        found = true;
    }
    return found;
}

// The nearest camera-visible area light ends a camera ray: its emission, or
// black for a side that does not emit, and its index. Returns false if there
// is none. `tEnd`, `surface` and `extent` as for nearestCameraLight().
NOVA_INLINE bool cameraAreaLight(const LightData *lights, uint32_t count, Vec3f o, Vec3f d, float tEnd, bool surface,
                                 float extent, Spectrum &L, uint32_t &i) {
    i = 0;
    float t = 0.0f;
    Vec3f n;
    bool front = false;
    if (!nearestCameraLight(lights, count, o, d, tEnd, surface, extent, i, t, n, front))
        return false;
    const LightData &light = lights[i];
    L = emitsFrom(light, front) ? areaLightRadiance(light, o + d * t, d) : Spectrum{0.0f, 0.0f, 0.0f};
    return true;
}

} // namespace nova
