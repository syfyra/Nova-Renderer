#pragma once

// Per-sample AOV state (docs/design/M11-P6.md 4): each camera sample's first
// surface (the hit the cryptomattes take), its radiance and alpha feed the
// film's filtered utility planes (Karma's karma:filter) and its deep camera
// map. The CPU waves and the GPU (after a wave's download) record the same
// data and fold it here, so both devices give the same planes. The
// first-hit AOVs through the pixel centres share the value computation.

#include "core/film/film.h"
#include "core/integrator/material_lane.h"
#include "core/integrator/settings.h"
#include "core/scene/camera.h"
#include "core/scene/hit.h"
#include "core/scene/scene.h"

#include <cstdint>
#include <vector>

namespace nova {

// One camera sample's record, per path slot (struct of arrays).
struct CameraSampleRecords {
    std::vector<uint32_t> mesh; // the first surface (kInvalidId: none; volumes are transparent to it)
    std::vector<uint32_t> prim;
    std::vector<uint32_t> instance;
    std::vector<Vec2f> uv;      // the hit's barycentrics
    std::vector<float> t;       // its distance along the camera ray
    std::vector<float> eventT;  // the first event's: a volume scatter in front of the surface, else t (kInf: none)
    std::vector<Vec3f> org;     // the camera ray (from the near plane) and its shutter time
    std::vector<Vec3f> dir;
    std::vector<float> time;
    std::vector<float> alpha;   // the sample's alpha (the beauty's)

    void reserve(uint32_t paths);
};

// Records of `count` paths (pointers to their slot arrays) and their pixels,
// sample numbers and radiance.
struct CameraSampleView {
    uint32_t count = 0;
    const uint32_t *pixel = nullptr;
    const uint32_t *sampleIndex = nullptr;
    const Spectrum *radiance = nullptr;
    const uint32_t *mesh = nullptr;
    const uint32_t *prim = nullptr;
    const uint32_t *instance = nullptr;
    const Vec2f *uv = nullptr;
    const float *t = nullptr;
    const float *eventT = nullptr;
    const Vec3f *org = nullptr;
    const Vec3f *dir = nullptr;
    const float *time = nullptr;
    const float *alpha = nullptr;

    static CameraSampleView of(const CameraSampleRecords &r, uint32_t count, const uint32_t *pixel,
                               const uint32_t *sampleIndex, const Spectrum *radiance);
};

// Folds the samples into the film's filtered utility planes and deep map.
// `globals`: the render's shading globals (materials of shader AOVs).
void accumulateCameraSamples(const Scene &scene, const Camera &camera, const ShadeGlobals &globals, Film &film,
                             const CameraSampleView &samples);

// A surface (or a camera-visible light: hit null) as the utility values see
// it: the hit, its barycentrics and distance, position, shading and
// geometric normals as authored, the eye ray, the surface inputs (null: not
// evaluated), the material, the sample's shutter time and number.
struct UtilityHit {
    const SurfaceHit *hit = nullptr;
    Vec2f uv;
    float tHit = 0.0f;
    Vec3f P, n, ng;
    Vec3f org, dir;
    const SurfaceInputs *inputs = nullptr;
    MaterialId material = kDefaultMaterialId;
    float time = 0.0f;
    uint32_t sampleIndex = 0;
};

// What the utility values need of the camera.
struct UtilityContext {
    const Scene &scene;
    const Camera &camera;
    Mat4 viewToNdc;
    Vec3f eye;
    bool ortho = false;
    float nearDepth = 0.0f, farDepth = 0.0f; // the clipping planes' view depths (far: 0 = none)

    static UtilityContext make(const Scene &scene, const Camera &camera);
    // Whether a request reads the surface inputs (shader AOVs, base colour).
    static bool needsInputs(const UtilityRequest &r) {
        return r.kind == UtilityKind::BaseColor || r.kind == UtilityKind::ShaderAov;
    }
    // The request's value at the hit (utilityWidth(kind) floats; values the
    // hit does not define are left as they are).
    void value(const UtilityRequest &r, const UtilityHit &h, float *o) const;
    // The camera ray's origin on the lens plane and its distance from there
    // to `org` (the near plane: rays start there).
    Vec3f lensOrigin(Vec3f org, Vec3f dir, float &toOrg) const;
};

} // namespace nova
