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

// The scene volume of a gprim that is a medium.
class HdNovaMedium {
public:
    bool Active() const { return _volumeId != nova::kInvalidId; }
    // Creates or updates the volume: `shell` is the gprim's new shell in its
    // object space (null: keep the last one; the first call needs one),
    // `transformDirty` resamples its transform over `shutter` (the gprim's:
    // the frame alone with Karma's per-object motion blur off).
    void Sync(HdSceneDelegate *sd, HdNovaRenderParam &param, SdfPath const &id, SdfPath const &material, bool visible,
              int primId, nova::MediumShellPtr shell, bool transformDirty, HdNovaShutter const &shutter);
    // Removes the volume (the gprim is a surface again, or goes away).
    void Remove(HdNovaRenderParam &param);

private:
    nova::VolumeId _volumeId = nova::kInvalidId;
    nova::MediumShellPtr _shell;
    bool _warnedOpen = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
