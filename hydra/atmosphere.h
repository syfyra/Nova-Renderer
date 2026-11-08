#pragma once

// The Karma Sky Atmosphere (docs/design/M11-P7.md): Houdini's imaging adapter
// hands the KarmaSkyAtmosphere gprim (the Karma Sky Atmosphere LOP, the
// Karma Physical Sky in atmosphere mode) to delegates as an rprim of type
// "karmaSkyAtmosphere" with the LOP's constant primvars. Nova renders it as
// the scene's planetary atmosphere (core/volume/atmosphere.h): a scene volume
// without fields that carries an AtmosphereDesc, its volume limit and LPE tag
// (karma:object:volumelimit, karma:object:lpetag) like any volume's.
//
// Read (as Karma, measured): enableatmosphere, rayleighcoeff, rayleighdist,
// miescattercoeff, mieabsorpcoeff, miedist, miephase, extraabsorpcoeff,
// extraaltitude, extrawidth (the ozone tent's full width), planetcenter,
// sealevel, groundoffset, groundalbedo, groundvisibility (a rendervisibility
// expression), groundreceiveshadow, renderbackside, ignorecameraclip and
// ociofrom (the coefficients' colour space). The prim's transform rotates and
// scales the planet centre as a vector and scales the sea level and ground
// offset (its translation does not move the planet; Karma, measured).
// Ignored, documented: lutdim, stepsizemin, stepsizemax (Karma's
// transmittance table and marching; Nova computes the transmittance) and
// solidalphainipr (alpha in Karma's IPR only).

#include <pxr/imaging/hd/rprim.h>
#include <pxr/pxr.h>

#include <cstdint>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaAtmosphere final : public HdRprim {
public:
    explicit HdNovaAtmosphere(SdfPath const &id);
    ~HdNovaAtmosphere() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits,
              TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;
    TfTokenVector const &GetBuiltinPrimvarNames() const override;

    // The rprim type Houdini's adapter emits.
    static TfToken const &TypeToken();

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override;

private:
    uint32_t _volumeId = 0xFFFFFFFFu;
    bool _counted = false;      // counted among the visible atmospheres (more than one warns)
    bool _warnedSecond = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
