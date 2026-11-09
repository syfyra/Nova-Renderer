#include "light.h"

#include "colorSpace.h"
#include "diagnostics.h"
#include "instancer.h"
#include "renderParam.h"

#include <pxr/imaging/hd/renderIndex.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usdLux/blackbody.h>

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// Names that are not HdLightTokens are looked up as attribute names verbatim.
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (treatAsPoint)
    (latlong)
    (automatic)
    ((renderLightGeo, "inputs:karma:light:renderlightgeo"))
    ((singleSided, "inputs:karma:light:singlesided"))
    ((rectTextureFlip, "inputs:karma:light:recttextureflip"))
    ((lpeTag, "inputs:karma:light:lpetag"))
    ((samplingQuality, "inputs:karma:light:samplingquality"))
    ((novaSamplingQuality, "inputs:nova:light:samplingQuality"))
    ((equiangular, "inputs:karma:light:equiangular"))
    ((novaEquiangular, "inputs:nova:light:equiangular"))
    ((samplingMode, "inputs:karma:light:samplingmode"))
    ((novaSamplingMode, "inputs:nova:light:samplingMode"))
    ((illumBackground, "inputs:karma:light:illumbackground"))
    ((portalMisBias, "inputs:karma:light:portalmisbias"))
    ((novaPortalMisBias, "inputs:nova:light:portalMisBias"))
    ((legacyCategory, "inputs:karma:light:category"))
    ((legacyShadowSet, "inputs:karma:light:shadowtraceset"))
    ((flipNone, "None"))
    ((flipX, "Flip X"))
    ((flipY, "Flip Y"))
    ((flipXY, "Flip XY"))
    ((skyShaderId, "kma:light:shaderId"))
    ((karmaSkyShader, "KMAskyDomeLight"))
    ((skyResolution, "inputs:nova:light:skyResolution")));

// Karma sky inputs by their input name: the attribute name first, then the
// bare name (scene delegates differ in which they answer).
VtValue skyInput(HdSceneDelegate *sd, SdfPath const &id, char const *name) {
    VtValue v = sd->GetLightParamValue(id, TfToken(std::string("inputs:") + name));
    return v.IsEmpty() ? sd->GetLightParamValue(id, TfToken(name)) : v;
}

template <typename T>
T skyInput(HdSceneDelegate *sd, SdfPath const &id, char const *name, T fallback) {
    return VtValue::Cast<T>(skyInput(sd, id, name)).template GetWithDefault<T>(fallback);
}

nova::Spectrum toSpectrum(GfVec3f const &v) { return {v[0], v[1], v[2]}; }

// Casts, so e.g. a double-authored intensity is still read.
template <typename T>
T lightParam(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name, T fallback) {
    return VtValue::Cast<T>(sd->GetLightParamValue(id, name)).template GetWithDefault<T>(fallback);
}

// File path of an asset-valued light parameter; empty if unset or unresolved.
std::string assetPath(VtValue const &value) {
    if (value.IsHolding<SdfAssetPath>()) {
        SdfAssetPath const &asset = value.UncheckedGet<SdfAssetPath>();
        return asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
    }
    return value.IsHolding<std::string>() ? value.UncheckedGet<std::string>() : std::string();
}

// A string- or token-valued parameter; empty if unset.
std::string textParam(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name) {
    const VtValue v = sd->GetLightParamValue(id, name);
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    return {};
}

nova::RectTextureFlip rectFlip(TfToken const &flip) {
    if (flip == _tokens->flipNone)
        return nova::RectTextureFlip::None;
    if (flip == _tokens->flipY)
        return nova::RectTextureFlip::FlipY;
    if (flip == _tokens->flipXY)
        return nova::RectTextureFlip::FlipXY;
    return nova::RectTextureFlip::FlipX; // Karma's default
}

nova::LightType lightType(TfToken const &type) {
    if (type == HdPrimTypeTokens->domeLight)
        return nova::LightType::Dome;
    if (type == HdPrimTypeTokens->sphereLight)
        return nova::LightType::Sphere;
    if (type == HdPrimTypeTokens->rectLight)
        return nova::LightType::Rect;
    if (type == HdPrimTypeTokens->diskLight)
        return nova::LightType::Disk;
    if (type == HdPrimTypeTokens->cylinderLight)
        return nova::LightType::Cylinder;
    return nova::LightType::Distant;
}

// Karma's sky dome (KarmaSkyDomeLight) reaches Hydra as a generic light;
// it is recognised by its shader id or its sky inputs.
bool isKarmaSky(HdSceneDelegate *sd, SdfPath const &id) {
    const VtValue shader = sd->GetLightParamValue(id, _tokens->skyShaderId);
    if (shader.IsHolding<TfToken>() && shader.UncheckedGet<TfToken>() == _tokens->karmaSkyShader)
        return true;
    return !skyInput(sd, id, "solar_altitude").IsEmpty() || !skyInput(sd, id, "turbidity").IsEmpty();
}

} // namespace

void HdNovaLight::_SyncKarmaSky(HdSceneDelegate *sd, nova::LightDesc &desc) {
    const SdfPath &id = GetId();
    // Defaults: the KarmaSkyDomeLight schema's.
    nova::KarmaSkyDesc sky;
    sky.solarAltitude = skyInput<float>(sd, id, "solar_altitude", 45.0f);
    sky.solarAzimuth = skyInput<float>(sd, id, "solar_azimuth", 0.0f);
    sky.turbidity = skyInput<float>(sd, id, "turbidity", 3.0f);
    sky.horizonBlur = skyInput<float>(sd, id, "horizon_blur", 0.5f);
    sky.groundAlbedo = toSpectrum(skyInput<GfVec3f>(sd, id, "ground_albedo", GfVec3f(0.5f)));
    sky.groundColor = toSpectrum(skyInput<GfVec3f>(sd, id, "ground_color", GfVec3f(0.0f)));
    const VtValue compute = skyInput(sd, id, "computegroundcolor");
    sky.computeGroundColor = compute.IsEmpty() || VtValue::Cast<int>(compute).GetWithDefault(1) != 0;
    // Karma's sky ignores the colour temperature and normalize (measured).
    const GfVec3f color = lightParam<GfVec3f>(sd, id, HdLightTokens->color, GfVec3f(1.0f));
    sky.lightColor = toSpectrum(color) * (lightParam<float>(sd, id, HdLightTokens->intensity, 1.0f) *
                                          std::exp2(lightParam<float>(sd, id, HdLightTokens->exposure, 0.0f)));
    // Karma converts the model's linear Rec.709 into the rendering space.
    const HdNovaColorTransform toRendering = HdNovaGetColorTransform("lin_rec709");
    if (toRendering.kind == HdNovaColorTransform::Kind::Matrix)
        std::copy(std::begin(toRendering.matrix), std::end(toRendering.matrix), std::begin(sky.skyToRendering));
    else if (toRendering.kind == HdNovaColorTransform::Kind::Nonlinear)
        HD_NOVA_WARN("the rendering space is not a linear transform of Linear Rec.709; the Karma sky %s is not "
                     "converted to it",
                     id.GetText());

    // Latlong width (height: half). 2048 keeps the bilinear lookup within
    // 0.05 % of the model away from the horizon and resolves the default
    // 1 degree horizon fade in 6 rows; baking takes about 20 ms, the
    // sampling distribution 30 ms (docs/design/M11-P3.md).
    const int width = std::clamp(lightParam<int>(sd, id, _tokens->skyResolution, 2048), 16, 16384) & ~1;

    // The texture holds the final radiance.
    desc.type = nova::LightType::Dome;
    desc.color = {1.0f, 1.0f, 1.0f};
    desc.intensity = 1.0f;
    desc.exposure = 0.0f;
    desc.normalize = false;
    desc.shaped = false;
    if (!nova::isFinite(sky.lightColor) || !nova::isFinite(sky.groundColor)) {
        HD_NOVA_WARN("the Karma sky %s has a non-finite color, intensity, exposure or ground color and emits nothing",
                     id.GetText());
        desc.intensity = 0.0f;
        _skyImage.reset();
        return;
    }
    if (!_skyImage || !(_sky == sky) || _skyWidth != width) {
        _skyImage = nova::bakeKarmaSky(sky, width);
        _sky = sky;
        _skyWidth = width;
    }
    desc.texture = _skyImage;
}

void HdNovaLight::_RemoveLights(HdRenderParam *renderParam) {
    if (_lightId == nova::kInvalidId)
        return;
    nova::Scene &scene = static_cast<HdNovaRenderParam *>(renderParam)->AcquireSceneForEdit();
    scene.removeLight(_lightId);
    for (nova::LightId extra : _instanceLightIds)
        scene.removeLight(extra);
    _instanceLightIds.clear();
    _lightId = nova::kInvalidId;
}

HdNovaLight::HdNovaLight(SdfPath const &id, TfToken const &lightType) : HdLight(id), _lightType(lightType) {}

HdDirtyBits HdNovaLight::GetInitialDirtyBitsMask() const { return AllDirty; }

void HdNovaLight::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) {
    const SdfPath &id = GetId();
    if (id.IsEmpty()) { // fallback prim
        *dirtyBits = Clean;
        return;
    }

    // Generic lights: only Karma's sky dome is known.
    const bool karmaSky = _lightType == HdPrimTypeTokens->light && isKarmaSky(sd, id);
    if (_lightType == HdPrimTypeTokens->light && !karmaSky) {
        if (!_warnedUnsupported) {
            _warnedUnsupported = true;
            HD_NOVA_WARN("%s is a light of a type Nova does not support (a generic Hydra light that is not a "
                         "Karma Sky Dome Light); it is ignored",
                         id.GetText());
        }
        _RemoveLights(renderParam);
        *dirtyBits = Clean;
        return;
    }

    nova::Scene &scene = static_cast<HdNovaRenderParam *>(renderParam)->AcquireSceneForEdit();
    if (_lightId == nova::kInvalidId)
        _lightId = scene.createLight();

    nova::LightDesc desc;
    desc.type = lightType(_lightType);

    GfVec3f color = lightParam<GfVec3f>(sd, id, HdLightTokens->color, GfVec3f(1.0f));
    if (lightParam<bool>(sd, id, HdLightTokens->enableColorTemperature, false))
        color = GfCompMult(color, UsdLuxBlackbodyTemperatureAsRgb(
                                      lightParam<float>(sd, id, HdLightTokens->colorTemperature, 6500.0f)));
    desc.color = {color[0], color[1], color[2]};
    desc.intensity = lightParam<float>(sd, id, HdLightTokens->intensity, 1.0f);
    desc.exposure = lightParam<float>(sd, id, HdLightTokens->exposure, 0.0f);
    desc.diffuse = lightParam<float>(sd, id, HdLightTokens->diffuse, 1.0f);
    desc.specular = lightParam<float>(sd, id, HdLightTokens->specular, 1.0f);
    desc.normalize = lightParam<bool>(sd, id, HdLightTokens->normalize, false);
    desc.angleDeg = lightParam<float>(sd, id, HdLightTokens->angle, 0.53f);
    desc.radius = lightParam<float>(sd, id, HdLightTokens->radius, 0.5f);
    desc.width = lightParam<float>(sd, id, HdLightTokens->width, 1.0f);
    desc.height = lightParam<float>(sd, id, HdLightTokens->height, 1.0f);
    desc.length = lightParam<float>(sd, id, HdLightTokens->length, 1.0f);
    desc.treatAsPoint = lightParam<bool>(sd, id, _tokens->treatAsPoint, false);

    // ShapingAPI attributes only exist where the schema is applied.
    const VtValue coneAngle = sd->GetLightParamValue(id, HdLightTokens->shapingConeAngle);
    const VtValue focus = sd->GetLightParamValue(id, HdLightTokens->shapingFocus);
    if (!coneAngle.IsEmpty() || !focus.IsEmpty()) {
        desc.shaped = true;
        desc.coneAngleDeg = lightParam<float>(sd, id, HdLightTokens->shapingConeAngle, 90.0f);
        desc.coneSoftness = lightParam<float>(sd, id, HdLightTokens->shapingConeSoftness, 0.0f);
        desc.focus = lightParam<float>(sd, id, HdLightTokens->shapingFocus, 0.0f);
        const GfVec3f tint = lightParam<GfVec3f>(sd, id, HdLightTokens->shapingFocusTint, GfVec3f(0.0f));
        desc.focusTint = {tint[0], tint[1], tint[2]};
    }
    desc.visible = sd->GetVisible(id);

    // Karma's light properties; unauthored they take Karma's fallbacks.
    desc.cameraVisible = lightParam<bool>(sd, id, _tokens->renderLightGeo, false);
    desc.twoSided = !lightParam<bool>(sd, id, _tokens->singleSided, true);
    desc.rectFlip = rectFlip(lightParam<TfToken>(sd, id, _tokens->rectTextureFlip, _tokens->flipX));
    desc.lpeTag = textParam(sd, id, _tokens->lpeTag);
    // The selection weight (M9 D18): Nova's, else Karma's sampling quality.
    desc.samplingQuality = lightParam<float>(sd, id, _tokens->novaSamplingQuality,
                                             lightParam<float>(sd, id, _tokens->samplingQuality, 1.0f));
    // Equiangular sampling in volumes (M9 D17): Nova's, else Karma's (off).
    desc.equiangular = lightParam<bool>(sd, id, _tokens->novaEquiangular,
                                        lightParam<bool>(sd, id, _tokens->equiangular, false));
    // Background holdouts (docs/design/M11-P2.md): the light lights them and
    // its shadows stay out of the holdout shadow outputs (Karma's, off).
    desc.illumBackground = lightParam<bool>(sd, id, _tokens->illumBackground, false);
    // Light portals (docs/design/M11-P9b.md): the path the portals' dome
    // lists name, and the balance of portal against environment samples
    // (Nova's, else Karma's portalmisbias, 0).
    desc.path = id.GetString();
    desc.portalMisBias = lightParam<float>(sd, id, _tokens->novaPortalMisBias,
                                           lightParam<float>(sd, id, _tokens->portalMisBias, 0.0f));
    // Karma's per-light sampling mode (D18): Uniform (1, "uniform") keeps the
    // light out of the tree; Auto (0) and Light Tree (2) put it in.
    for (TfToken const &key : {_tokens->novaSamplingMode, _tokens->samplingMode}) {
        const VtValue mode = sd->GetLightParamValue(id, key);
        if (mode.IsEmpty())
            continue;
        const std::string text = textParam(sd, id, key);
        desc.outsideTree = text.empty() ? VtValue::Cast<int>(mode).GetWithDefault(0) == 1
                                        : TfStringToLower(text).find("uniform") != std::string::npos;
        break;
    }
    // Karma's legacy light categories and trace sets: superseded by the
    // UsdLux collections, which Nova reads (D15).
    if (!textParam(sd, id, _tokens->legacyCategory).empty() || !textParam(sd, id, _tokens->legacyShadowSet).empty()) {
        static std::once_flag warned;
        std::call_once(warned, [&] {
            TF_WARN("Nova: %s uses Karma's legacy light categories or shadow trace sets, which Nova ignores; "
                    "use light linking (collection:lightLink / shadowLink) instead",
                    id.GetText());
        });
    }
    // Light linking (M9 D15): the categories HdsiLightLinkingSceneIndex gives
    // the light's collections (empty: every object).
    desc.lightLink = lightParam<TfToken>(sd, id, HdTokens->lightLink, TfToken()).GetString();
    desc.shadowLink = lightParam<TfToken>(sd, id, HdTokens->shadowLink, TfToken()).GetString();
    // Light filters (docs/design/M11-P4.md): the paths of `light:filters`,
    // in order; the scene resolves them against the lightFilter sprims.
    {
        const VtValue filters = sd->GetLightParamValue(id, HdTokens->filters);
        if (filters.IsHolding<SdfPathVector>())
            for (SdfPath const &path : filters.UncheckedGet<SdfPathVector>())
                desc.filters.push_back(path.GetString());
    }

    if (_lightType == HdPrimTypeTokens->domeLight || _lightType == HdPrimTypeTokens->rectLight) {
        const std::string path = assetPath(sd->GetLightParamValue(id, HdLightTokens->textureFile));
        if (!path.empty()) {
            if (_lightType == HdPrimTypeTokens->domeLight) {
                const TfToken format = lightParam<TfToken>(sd, id, HdLightTokens->textureFormat, _tokens->automatic);
                if (format != _tokens->latlong && format != _tokens->automatic)
                    HD_NOVA_WARN("dome texture format '%s' of %s is not supported yet; using latlong",
                            format.GetText(), id.GetText());
            }
            desc.texture = static_cast<HdNovaRenderParam *>(renderParam)->GetTextureCache().Load(path);
        }
    }

    GfMatrix4d xf = sd->GetTransform(id);
    if (_lightType == HdPrimTypeTokens->domeLight) {
        // DomeLight_1 publishes its pole-axis alignment separately.
        const VtValue offset = sd->GetLightParamValue(id, HdLightTokens->domeOffset);
        if (offset.IsHolding<GfMatrix4d>())
            xf = offset.UncheckedGet<GfMatrix4d>() * xf;
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            desc.transform.m[i][j] = xf[i][j];
    if (karmaSky)
        _SyncKarmaSky(sd, desc);

    // The core turns non-finite emission off; say why the light is dark.
    const nova::Spectrum emission = desc.color * (desc.intensity * std::exp2(desc.exposure));
    if (!nova::isFinite(emission))
        HD_NOVA_WARN("light %s has a non-finite color, intensity or exposure and emits nothing", id.GetText());
    if (desc.type == nova::LightType::Sphere && !desc.treatAsPoint) {
        const double sx = xf.GetRow3(0).GetLength(), sy = xf.GetRow3(1).GetLength(), sz = xf.GetRow3(2).GetLength();
        if (std::max({sx, sy, sz}) > 1.001 * std::min({sx, sy, sz}))
            HD_NOVA_WARN("sphere light %s is scaled non-uniformly; Nova renders a sphere of the mean radius "
                         "(ellipsoids are not supported yet)",
                         id.GetText());
    }

    // Lights under an instancer (M9 D16): one light per instance, at the
    // instance's transform (it includes the light's own); Karma lights every
    // instance (measured, research lights.md 4.3).
    std::vector<GfMatrix4d> instanceXf;
    const SdfPath instancerId = sd->GetInstancerId(id);
    if (!instancerId.IsEmpty()) {
        HdRenderIndex &index = sd->GetRenderIndex();
        HdInstancer::_SyncInstancerAndParents(index, instancerId);
        if (auto *instancer = static_cast<HdNovaInstancer *>(index.GetInstancer(instancerId))) {
            auto *param = static_cast<HdNovaRenderParam *>(renderParam);
            instancer->SyncShutter(param->GetShutter());
            instancer->ComputeInstanceTransformsAt(id, xf, param->GetShutter().TimeAt(0.5f), instanceXf);
        }
    }
    if (instancerId.IsEmpty()) {
        scene.updateLight(_lightId, desc);
        for (nova::LightId extra : _instanceLightIds)
            scene.removeLight(extra);
        _instanceLightIds.clear();
    } else {
        while (_instanceLightIds.size() + 1 < std::max<size_t>(instanceXf.size(), 1))
            _instanceLightIds.push_back(scene.createLight());
        while (_instanceLightIds.size() + 1 > std::max<size_t>(instanceXf.size(), 1)) {
            scene.removeLight(_instanceLightIds.back());
            _instanceLightIds.pop_back();
        }
        for (size_t k = 0; k < std::max<size_t>(instanceXf.size(), 1); ++k) {
            nova::LightDesc d = desc;
            if (k < instanceXf.size()) {
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                        d.transform.m[i][j] = instanceXf[k][i][j];
            } else {
                d.visible = false; // an instancer without instances
            }
            scene.updateLight(k == 0 ? _lightId : _instanceLightIds[k - 1], d);
        }
    }
    *dirtyBits = Clean;
}

void HdNovaLight::Finalize(HdRenderParam *renderParam) {
    _RemoveLights(renderParam);
    _skyImage.reset();
}

PXR_NAMESPACE_CLOSE_SCOPE
