#pragma once

// Volume rprims (docs/design/M8.md D3): a UsdVolVolume with its field
// bindings. The fields the volume renders with are loaded from their bprims
// (hydra/field.h) and placed in the volume's object space; the scene builds
// the majorants. The Volume LOP authors no extent, so bounds come from the
// grids. Transforms are sampled over the shutter as for meshes.

#include "motion.h"

#include "core/volume/volume.h"

#include <pxr/imaging/hd/volume.h>
#include <pxr/pxr.h>

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

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
    nova::VolumeId _volumeId = nova::kInvalidId;
    std::vector<HdVolumeFieldDescriptor> _fields;
    std::vector<uint64_t> _fieldVersions; // the field versions the scene's volume was built with
    std::vector<std::string> _neededFields; // the names its material reads (or density)
    float _volumeLimit = -1.0f;             // nova:object:volumeLimit / karma:object:volumelimit
    std::string _lpeTag;                    // karma:object:lpetag
    bool _light = false;                    // nova:object:volumeLight / karma:object:treat_as_lightsource
    float _velocityBlur = 0.0f;             // karma:object:vblur on: volumevblurscale (0: off)
    bool _warnedFilter = false;             // karma:object:volumefilter other than box/1 (once)
    float _lightDiffuse = 1.0f, _lightSpecular = 1.0f; // karma:object:lightsource_*scale
    HdNovaShutter _sampledShutter;
    unsigned _sampledMaxSteps = 0;
};

PXR_NAMESPACE_CLOSE_SCOPE
