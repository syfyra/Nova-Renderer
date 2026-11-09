#pragma once

// Native Hydra instancing: point instancers and USD native instances (the
// Hydra 2 emulation delivers both through HdInstancer). Validated against the
// UsdImaging scene-index chain of USD 26.05 as shipped with Houdini 22.

#include "motion.h"

#include "core/math/spectrum.h"
#include "core/scene/attributes.h"
#include "core/math/xform.h"
#include "core/scene/motion.h"
#include "core/scene/scene.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/pxr.h>

#include <mutex>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaInstancer final : public HdInstancer {
public:
    HdNovaInstancer(HdSceneDelegate *delegate, SdfPath const &id);

    // Called through HdInstancer::_SyncInstancerAndParents() from the rprims
    // (the render index never syncs instancers itself).
    void Sync(HdSceneDelegate *delegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) override;

    // Object-to-world transforms of every instance of `prototypeId`, with all
    // nesting levels flattened and the prototype's own transform applied
    // first (row vectors, Hydra convention):
    //   out[k] = prototypeXf * L_0(i_0) * L_1(i_1) * ... * L_n(i_n)
    //   L(i)   = hydra:instanceTransforms[i] * S(scales[i]) * R(rotations[i])
    //            * T(translations[i]) * instancerTransform
    // Level 0 is this instancer, level n the outermost one. The outermost
    // level varies slowest, the innermost fastest,
    //   k = (i_n * N_{n-1} + ...) * N_0 + i_0,
    // which is Hydra's flattened instance index (HdxPrimOriginInfo decodes
    // it the same way), so k is the value of the instanceId AOV.
    // An invisible instancer anywhere in the chain yields no instances.
    // Read-only, so rprims may call it concurrently once the chain is synced.
    // Evaluated at the frame (clamped into the shutter).
    void ComputeInstanceTransforms(SdfPath const &prototypeId, GfMatrix4d const &prototypeXf,
                                   std::vector<nova::Xform34> &out) const;

    // Motion blur. A shutter change dirties the rprims but not the instancers,
    // so every rprim calls this after _SyncInstancerAndParents(): it resamples
    // this instancer and its parents if they were sampled for another shutter.
    // Safe to call from concurrent rprim syncs.
    void SyncShutter(HdNovaShutter const &shutter);
    // Uniform shutter steps this instancer and its parents need (1 if
    // nothing moves).
    unsigned GetMotionStepCount(unsigned maxSteps) const;
    // The flattened transforms (as ComputeInstanceTransforms) at the shutter
    // time `time` (frames relative to the current one).
    void ComputeInstanceTransformsAt(SdfPath const &prototypeId, GfMatrix4d const &prototypeXf, float time,
                                     std::vector<GfMatrix4d> &out) const;

    // Instance-rate shading primvars of every level (PointInstancer
    // primvars, primvars on native instances), flattened in the same order as
    // the transforms. Per name, the innermost level that defines it wins.
    void ComputeInstanceAttributes(SdfPath const &prototypeId, std::vector<nova::InstanceAttribute> &out) const;
    // The same for string and int primvars (cryptomattes; ints as decimal
    // strings): per name one value per flattened instance.
    void ComputeInstanceStrings(SdfPath const &prototypeId,
                                std::vector<std::pair<std::string, std::vector<std::string>>> &out) const;

private:
    // Samples of a primvar over the shutter (frame-relative times, ascending,
    // possibly bracketing it; one sample at the frame without motion or if
    // the array length changes within the shutter). The VtValues share the
    // scene index buffers.
    struct Samples {
        std::vector<float> times;
        std::vector<VtValue> values;
        bool empty() const { return values.empty(); }
    };
    void _SampleTransform(HdSceneDelegate *delegate);
    void _SamplePrimvars(HdSceneDelegate *delegate);
    // The flattened transforms at `time` without a prototype transform, in
    // double precision (also the parent level of a nested instancer).
    void _ComputeAt(SdfPath const &prototypeId, float time, std::vector<GfMatrix4d> &out) const;
    // Number of flattened instances of `prototypeId` (0 if invisible).
    size_t _FlattenedCount(SdfPath const &prototypeId) const;
    HdNovaInstancer *_Parent() const;

    std::mutex _mutex;      // Sync() and SyncShutter()
    HdNovaShutter _shutter; // the shutter the samples cover
    HdNovaTransformSamples _xf;
    // Instance-rate primvars. Any of them may be empty.
    Samples _translations; // VtVec3fArray (positions), also Vec3d/Vec3h
    Samples _rotations;    // VtQuathArray (orientations) or VtQuatfArray (orientationsf), also Quatd, Vec4f
    Samples _scales;       // VtVec3fArray, also Vec3d/Vec3h
    Samples _transforms;   // VtMatrix4dArray (native instancing), also Matrix4f
    // Instance-rate shading primvars (name, value at the frame).
    std::vector<std::pair<TfToken, VtValue>> _shadingPrimvars;
    bool _visible = true;
};

// The instances of an instanced prim: static transforms in `instances`, or,
// if an instancer level or the prototype moves, `steps` uniform SRT keys per
// instance over the shutter in `keys` (step-major).
void HdNovaComputeInstances(HdSceneDelegate *sd, SdfPath const &id, HdNovaInstancer const &instancer,
                            HdNovaShutter const &shutter, unsigned maxSteps, std::vector<nova::Xform34> &instances,
                            std::vector<nova::SrtTransform> &keys, uint32_t &steps);

// Light linking (docs/design/M9.md D15): the prim's categories (and its
// instancer's) and, under one instancer level, each instance's, as
// HdsiLightLinkingSceneIndex publishes them, into the Nova object.
void HdNovaSyncCategories(HdSceneDelegate *sd, SdfPath const &id, SdfPath const &instancerId, nova::Scene &scene,
                          nova::MeshId mesh);

PXR_NAMESPACE_CLOSE_SCOPE
