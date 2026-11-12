#pragma once

// Volume rprims (docs/design/M8.md D3): a UsdVolVolume with its field
// bindings. The fields the volume renders with are loaded from their bprims
// (hydra/field.h) and placed in the volume's object space; the scene builds
// the majorants. The Volume LOP authors no extent, so bounds come from the
// grids. Transforms are sampled over the shutter as for meshes. An instanced
// Volume (the prototype of a point instancer, a native instance) is one scene
// volume per instance, which share the grids and differ in their transforms
// (as the media bounded by surfaces, hydra/medium.h).

#include "karmaSettings.h"
#include "motion.h"

#include "core/scene/visibility.h"
#include "core/volume/volume.h"

#include <pxr/imaging/hd/volume.h>
#include <pxr/pxr.h>

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// A volume's per-object settings from its constant primvars (volumes and
// media bounded by surfaces, docs/design/M11-P5.md): the Volume ray limit
// (nova:object:volumeLimit, else karma:object:volumelimit), the LPE tag and
// the light source settings (D13; `defaults`: the render settings' Karma
// values, docs/design/M11-P1.md).
void HdNovaVolumeObjectSettings(HdSceneDelegate *sd, SdfPath const &id, nova::VolumeDesc &desc,
                                HdNovaObjectDefaults const &defaults = HdNovaObjectDefaults{});

class HdNovaVolume final : public HdVolume {
public:
    explicit HdNovaVolume(SdfPath const &id);

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits,
              TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override;

private:
    nova::VolumeId _volumeId = nova::kInvalidId;    // the prim's, or its first instance's
    std::vector<nova::VolumeId> _instanceVolumeIds; // of the instances after the first
    nova::VolumeDesc _desc;                         // what they were built from (instances that appear later take it)
    bool _instanced = false;
    bool _noInstances = false; // an instancer without (visible) instances: the volume is kept, hidden
    std::vector<HdVolumeFieldDescriptor> _fields;
    std::vector<uint64_t> _fieldVersions; // the field versions the scene's volume was built with
    std::vector<std::string> _neededFields; // the names its material reads (or density)
    float _volumeLimit = -1.0f;             // nova:object:volumeLimit / karma:object:volumelimit
    std::string _lpeTag;                    // karma:object:lpetag
    bool _light = false;                    // nova:object:volumeLight / karma:object:treat_as_lightsource
    float _velocityBlur = 0.0f;             // velocity blur on: volumevblurscale / fps (0: off)
    bool _warnedFilter = false;             // karma:object:volumefilter other than box/1 (once)
    float _lightDiffuse = 1.0f, _lightSpecular = 1.0f; // karma:object:lightsource_*scale
    nova::ObjectVisibility _visibility; // karma:object:rendervisibility / holdoutmode (M11-P2 D15)
    HdNovaShutter _sampledShutter;
    unsigned _sampledMaxSteps = 0;
};

PXR_NAMESPACE_CLOSE_SCOPE
