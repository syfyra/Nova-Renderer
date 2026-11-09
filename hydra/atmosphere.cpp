#include "atmosphere.h"

#include "colorSpace.h"
#include "diagnostics.h"
#include "primvarUtil.h"
#include "renderParam.h"
#include "volume.h"

#include "core/scene/visibility.h"

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/sceneDelegate.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// Visible atmospheres; Karma renders one (the first) and warns about the rest.
std::atomic<int> g_visibleAtmospheres{0};

bool numbers(HdSceneDelegate *sd, SdfPath const &id, const char *name, std::vector<float> &out) {
    int width = 0;
    out.clear();
    return HdNovaPrimvarToFloats(sd->Get(id, TfToken(name)), &width, &out) && !out.empty();
}

float number(HdSceneDelegate *sd, SdfPath const &id, const char *name, float fallback) {
    std::vector<float> v;
    return numbers(sd, id, name, v) && std::isfinite(v[0]) ? v[0] : fallback;
}

nova::Spectrum color(HdSceneDelegate *sd, SdfPath const &id, const char *name, nova::Spectrum fallback) {
    std::vector<float> v;
    if (!numbers(sd, id, name, v))
        return fallback;
    if (v.size() < 3)
        return nova::spectrum(v[0]);
    return {v[0], v[1], v[2]};
}

std::string text(HdSceneDelegate *sd, SdfPath const &id, const char *name) {
    const VtValue v = sd->Get(id, TfToken(name));
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    if (v.IsHolding<VtArray<std::string>>() && v.UncheckedGet<VtArray<std::string>>().size() == 1)
        return v.UncheckedGet<VtArray<std::string>>()[0];
    return {};
}

// Coefficients authored in `space` to the rendering space (Karma's ociofrom:
// "transformed into scene_linear when computing transmittance").
void toSceneLinear(std::string const &space, nova::Spectrum *colors, int count) {
    if (space.empty())
        return;
    std::vector<float> px;
    for (int i = 0; i < count; ++i)
        px.insert(px.end(), {colors[i].x, colors[i].y, colors[i].z});
    HdNovaConvertToSceneLinear("the sky atmosphere's ociofrom", space, px.data(), size_t(count), 3);
    for (int i = 0; i < count; ++i)
        colors[i] = {px[size_t(i) * 3], px[size_t(i) * 3 + 1], px[size_t(i) * 3 + 2]};
}

} // namespace

TfToken const &HdNovaAtmosphere::TypeToken() {
    static const TfToken token("karmaSkyAtmosphere");
    return token;
}

HdNovaAtmosphere::HdNovaAtmosphere(SdfPath const &id) : HdRprim(id) {}

HdNovaAtmosphere::~HdNovaAtmosphere() {
    if (_counted)
        --g_visibleAtmospheres;
}

HdDirtyBits HdNovaAtmosphere::GetInitialDirtyBitsMask() const {
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility;
}

void HdNovaAtmosphere::_InitRepr(TfToken const &reprToken, HdDirtyBits *) {
    auto it = std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken));
    if (it == _reprs.end())
        _reprs.emplace_back(reprToken, std::make_shared<HdRepr>());
}

TfTokenVector const &HdNovaAtmosphere::GetBuiltinPrimvarNames() const {
    static const TfTokenVector names;
    return names;
}

void HdNovaAtmosphere::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits,
                            TfToken const &reprToken) {
    (void)reprToken;
    auto &param = *static_cast<HdNovaRenderParam *>(renderParam);
    const SdfPath &id = GetId();
    const HdDirtyBits bits = *dirtyBits;
    if (bits & (HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility)) {
        if (bits & HdChangeTracker::DirtyVisibility)
            _UpdateVisibility(sd, dirtyBits);
        const bool enabled = number(sd, id, "enableatmosphere", 1.0f) != 0.0f;
        const bool visible = IsVisible() && enabled;
        if (visible != _counted) {
            g_visibleAtmospheres += visible ? 1 : -1;
            _counted = visible;
        }
        if (visible && g_visibleAtmospheres.load() > 1 && !_warnedSecond) {
            HD_NOVA_WARN("%s: more than one Karma Sky Atmosphere; only the first is rendered (as in Karma)",
                         id.GetText());
            _warnedSecond = true;
        }

        auto desc = std::make_shared<nova::AtmosphereDesc>();
        // The planet: the prim's transform rotates and scales the centre as
        // a vector and scales the radii (Karma: its translation is ignored).
        const GfMatrix4d m = sd->GetTransform(id);
        const nova::Spectrum pc = color(sd, id, "planetcenter", {0.0f, -6371000.0f, 0.0f});
        const GfVec3d cw = m.TransformDir(GfVec3d(pc.x, pc.y, pc.z));
        const double det = std::fabs(m.ExtractRotationMatrix().GetDeterminant());
        const double scale = det > 0.0 ? std::cbrt(det) : 1.0;
        for (int k = 0; k < 3; ++k)
            desc->center[k] = cw[k];
        desc->seaLevel = scale * double(number(sd, id, "sealevel", 6370900.0f));
        desc->groundOffset = scale * double(number(sd, id, "groundoffset", 0.0f));
        // The layers.
        nova::Spectrum coeffs[4] = {color(sd, id, "rayleighcoeff", desc->rayleigh),
                                    color(sd, id, "miescattercoeff", desc->mieScatter),
                                    color(sd, id, "mieabsorpcoeff", desc->mieAbsorb),
                                    color(sd, id, "extraabsorpcoeff", desc->ozone)};
        toSceneLinear(text(sd, id, "ociofrom"), coeffs, 4);
        desc->rayleigh = coeffs[0];
        desc->mieScatter = coeffs[1];
        desc->mieAbsorb = coeffs[2];
        desc->ozone = coeffs[3];
        desc->rayleighHeight = number(sd, id, "rayleighdist", desc->rayleighHeight);
        desc->mieHeight = number(sd, id, "miedist", desc->mieHeight);
        desc->mieG = number(sd, id, "miephase", desc->mieG);
        desc->ozoneAltitude = number(sd, id, "extraaltitude", desc->ozoneAltitude);
        desc->ozoneWidth = number(sd, id, "extrawidth", desc->ozoneWidth);
        // The ground and the rays.
        desc->groundAlbedo = color(sd, id, "groundalbedo", desc->groundAlbedo);
        const VtValue groundVisibility = sd->Get(id, TfToken("groundvisibility"));
        if (!groundVisibility.IsEmpty()) {
            const int mask = HdNovaRenderVisibilityValue(groundVisibility, id.GetString() + " groundvisibility");
            if (mask >= 0)
                desc->groundVisibility = uint8_t(mask);
        }
        desc->groundShadows = number(sd, id, "groundreceiveshadow", 0.0f) != 0.0f;
        desc->backside = number(sd, id, "renderbackside", 0.0f) != 0.0f;
        desc->ignoreCameraClip = number(sd, id, "ignorecameraclip", 1.0f) != 0.0f;

        nova::VolumeDesc volume;
        volume.name = id.GetString();
        volume.visible = visible;
        volume.atmosphere = std::move(desc);
        HdNovaVolumeObjectSettings(sd, id, volume);
        nova::Scene &scene = param.AcquireSceneForEdit();
        if (_volumeId == nova::kInvalidId)
            _volumeId = scene.createVolume(nova::objectKey(id.GetString())); // stable, not the sync order
        scene.updateVolume(_volumeId, std::move(volume));
    }
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

void HdNovaAtmosphere::Finalize(HdRenderParam *renderParam) {
    if (_counted) {
        --g_visibleAtmospheres;
        _counted = false;
    }
    if (_volumeId != nova::kInvalidId) {
        static_cast<HdNovaRenderParam *>(renderParam)->AcquireSceneForEdit().removeVolume(_volumeId);
        _volumeId = nova::kInvalidId;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
