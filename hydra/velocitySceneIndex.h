#pragma once

// Velocity and acceleration motion blur of geometry (docs/design/M8.md D19).
//
// pxr's HdsiVelocityMotionResolvingSceneIndex turns the velocities,
// accelerations and angular velocities of meshes, curves, points and point
// instancers into time-sampled points and instance positions/orientations,
// so Nova's motion sampling (HdNovaSampleVec3fArray, the instancer's primvar
// samples) sees them as ordinary deformation. It resolves each prim as its
// Hydra parameter __velocityMotionMode says; this filtering scene index,
// right before it, authors that parameter from the object settings as Karma
// reads them (measured, see the design notes):
//
//   meshes, curves, points: nova:object:velocityBlur, else karma:object:vblur
//     (constant primvars), else the render settings' karma:object:vblur;
//   point instancers: nova:object:instanceVelocityBlur, else
//     karma:object:instance_vblur, else the render settings' value;
//   0 (default) -> ignore (time samples only), 1 -> noAcceleration,
//   2 -> enable (accelerations too).
//
// Acceleration blur samples the shutter nova:object:accelerationSamples
// times, else karma:object:geosamples (instancers: instance_samples), else
// the render settings', else 2 (Karma's default: a straight segment); the
// resolving index reads that count from the primvar nonlinearSampleCount,
// which this index provides only to prims in acceleration mode.
//
// Velocities are per second and the shutter is in time codes: the resolving
// index converts with the scene globals' timeCodesPerSecond, which this
// index sets on the root prim from the render settings' houdini:fps (pxr's
// UsdImaging leaves it out; Houdini's says 24 whatever the stage has, while
// houdini:fps follows the stage's timeCodesPerSecond as Karma does).

#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/pxr.h>

#include <memory>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

// The render settings' defaults for objects that author nothing (Karma's
// karma:object:* on the render settings prim).
struct HdNovaVelocityDefaults {
    int velocityBlur = 0, instanceVelocityBlur = 0;
    int geoSamples = 2, instanceSamples = 2;
    double timeCodesPerSecond = 24.0;
    bool operator==(HdNovaVelocityDefaults const &) const = default;
};

// A velocity blur mode (0, 1, 2) from an authored value: a number, or one
// of Karma's menu labels ("No Velocity Blur", "Velocity Blur",
// "Acceleration Blur") as the Karma render settings author them. -1 if the
// value says nothing.
int HdNovaVelocityBlurMode(VtValue const &value);

TF_DECLARE_WEAK_AND_REF_PTRS(HdNovaVelocityModeSceneIndex);

class HdNovaVelocityModeSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static HdNovaVelocityModeSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input,
                                                  std::string const &renderInstanceId);
    ~HdNovaVelocityModeSceneIndex() override;

    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const &primPath) const override;

    // Sets the render settings' defaults of a render instance. If they
    // changed, every prim that can blur by velocity is dirtied (it resamples
    // in the next sync) and true is returned.
    static bool SetDefaults(std::string const &renderInstanceId, HdNovaVelocityDefaults const &defaults);

    // The whole chain Nova appends: this index, pxr's resolving index and
    // one that keeps shutter open and close only where Karma samples the
    // velocity motion twice (pxr samples rotations and accelerations at
    // least three times).
    static HdSceneIndexBaseRefPtr Append(HdSceneIndexBaseRefPtr const &input, std::string const &renderInstanceId);

    struct State;

protected:
    void _PrimsAdded(HdSceneIndexBase const &sender, HdSceneIndexObserver::AddedPrimEntries const &entries) override;
    void _PrimsRemoved(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::RemovedPrimEntries const &entries) override;
    void _PrimsDirtied(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::DirtiedPrimEntries const &entries) override;

private:
    HdNovaVelocityModeSceneIndex(HdSceneIndexBaseRefPtr const &input, std::string const &renderInstanceId);
    // Dirties every prim that can blur by velocity (and the root's frame
    // rate if `timeCodes`).
    void _DirtyAll(bool timeCodes);

    std::string _renderInstanceId;
    std::shared_ptr<State> _state;
};

PXR_NAMESPACE_CLOSE_SCOPE
