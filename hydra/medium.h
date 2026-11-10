#pragma once

// Media bounded by surfaces (docs/design/M11-P5.md): a mesh or sphere whose
// material makes it a medium (Karma's uniform volumes, the Karma Fog Box)
// renders as a scene volume filling its inside (core/volume/shell.h)
// instead of a surface. Measured in Karma CPU 22.0.466:
// - karma:object:volumeuniform on the prim (a constant primvar, inherited)
//   or karma:volumeuniform on the volume shader makes the prim a uniform
//   volume when its material has a volume shader (a surface shader beside it
//   is ignored), or a white medium of density 1 when it has no material; a
//   prim with only a surface material stays a surface;
// - a volume-only material without volumeuniform renders nothing in Karma
//   CPU (Karma XPU renders a volume): Nova renders the medium (the material's
//   intent; also the Fog Box's noise option, which turns volumeuniform off);
// - karma:object:volumeuniformdensity changes nothing (the shader's density
//   is used); volumesteprate is ignored (Nova does not march).

#include "motion.h"
#include "material/registry.h"

#include "core/scene/scene.h"

#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaRenderParam;

// Whether gprim `id`, bound to `material` (empty: none), is a medium.
bool HdNovaIsMedium(HdSceneDelegate *sd, SdfPath const &id, SdfPath const &material,
                    HdNovaMaterialRegistry const &registry);

// The scene volumes of a gprim that is a medium: one, or one per instance of
// an instanced gprim (point instancers and native instances, every nesting
// level flattened), which share the shell and the material and differ in
// their transforms.
class HdNovaMedium {
public:
    bool Active() const { return _volumeId != nova::kInvalidId; }
    // Creates or updates the volumes: `shell` is the gprim's new shell in its
    // object space (null: keep the last one; the first call needs one),
    // `transformDirty` resamples the transforms over `shutter` (the gprim's:
    // the frame alone with Karma's per-object motion blur off). `instancer`
    // is the gprim's instancer (empty: none), synced by the caller; its
    // changes count as a dirty transform.
    void Sync(HdSceneDelegate *sd, HdNovaRenderParam &param, SdfPath const &id, SdfPath const &material, bool visible,
              int primId, nova::MediumShellPtr shell, bool transformDirty, HdNovaShutter const &shutter,
              SdfPath const &instancer = SdfPath());
    // Removes the volumes (the gprim is a surface again, or goes away).
    void Remove(HdNovaRenderParam &param);

private:
    nova::VolumeId _volumeId = nova::kInvalidId;  // the gprim's, or its first instance's
    std::vector<nova::VolumeId> _instanceVolumeIds; // of the instances after the first
    nova::MediumShellPtr _shell;
    bool _instanced = false;
    bool _noInstances = false; // an instancer without (visible) instances: the volume is kept, hidden
    bool _warnedOpen = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
