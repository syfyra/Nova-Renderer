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

#include <atomic>
#include <memory>
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
    // Rprims may call it concurrently once the chain is synced. Evaluated at
    // the frame (clamped into `shutter`, the calling prim's).
    void ComputeInstanceTransforms(SdfPath const &prototypeId, GfMatrix4d const &prototypeXf,
                                   HdNovaShutter const &shutter, std::vector<nova::Xform34> &out) const;

    // Motion blur. Every prim samples the instancer chain over its own
    // shutter (Karma's per-object motion blur: the prototypes of one
    // instancer may differ, docs/design/M11-P1.md), and they sync in
    // parallel. So each level keeps its samples per shutter: built on first
    // use (locked), immutable after, held by the readers while they compute;
    // a Sync with a dirty transform or primvars drops them (a new render
    // shutter dirties the instancers).
    // Uniform shutter steps this instancer and its parents need over
    // `shutter` (1 if nothing moves).
    unsigned GetMotionStepCount(HdNovaShutter const &shutter, unsigned maxSteps) const;
    // The flattened transforms (as ComputeInstanceTransforms) at the time
    // `time` of `shutter` (frames relative to the current one).
    void ComputeInstanceTransformsAt(SdfPath const &prototypeId, GfMatrix4d const &prototypeXf,
                                     HdNovaShutter const &shutter, float time, std::vector<GfMatrix4d> &out) const;

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
    // This level's samples over one shutter; never changed once built.
    struct ShutterSamples {
        HdNovaShutter shutter;
        HdNovaTransformSamples xf;
        // Instance-rate primvars. Any of them may be empty.
        Samples translations; // VtVec3fArray (positions), also Vec3d/Vec3h
        Samples rotations;    // VtQuathArray (orientations) or VtQuatfArray (orientationsf), also Quatd, Vec4f
        Samples scales;       // VtVec3fArray, also Vec3d/Vec3h
        Samples transforms;   // VtMatrix4dArray (native instancing), also Matrix4f
    };
    using ShutterSamplesPtr = std::shared_ptr<const ShutterSamples>;
    // Instance-rate shading primvars (name, value at the frame).
    using ShadingPrimvars = std::vector<std::pair<TfToken, VtValue>>;

    // The samples over `shutter`, sampled now if this level has none yet.
    ShutterSamplesPtr _SamplesFor(HdNovaShutter const &shutter) const;
    ShutterSamplesPtr _Sample(HdNovaShutter const &shutter) const;
    std::shared_ptr<const ShadingPrimvars> _Shading() const;
    // The flattened transforms at `time` without a prototype transform, in
    // double precision (also the parent level of a nested instancer).
    void _ComputeAt(SdfPath const &prototypeId, HdNovaShutter const &shutter, float time,
                    std::vector<GfMatrix4d> &out) const;
    // Number of flattened instances of `prototypeId` (0 if invisible).
    size_t _FlattenedCount(SdfPath const &prototypeId) const;
    HdNovaInstancer *_Parent() const;

    mutable std::mutex _mutex; // the members below
    // Per shutter, the most recently built last (a few: the render's and the
    // frame alone of prims with motion blur off).
    mutable std::vector<ShutterSamplesPtr> _samples;
    std::shared_ptr<const ShadingPrimvars> _shading;
    std::atomic<bool> _visible{true};
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
