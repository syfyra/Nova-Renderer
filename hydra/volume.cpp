#include "volume.h"

#include "diagnostics.h"
#include "field.h"
#include "instancer.h"
#include "medium.h"
#include "primvarUtil.h"
#include "renderParam.h"
#include "velocitySceneIndex.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/xformSchema.h>

#include <algorithm>
#include <cmath>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

nova::Mat4 toNova(GfMatrix4d const &m) {
    nova::Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = m[i][j];
    return r;
}

// GfMatrix4d's row-vector affine map as images of the axes and origin.
nova::Affine3f toAffine(GfMatrix4d const &m) {
    nova::Affine3f a;
    a.x = {float(m[0][0]), float(m[0][1]), float(m[0][2])};
    a.y = {float(m[1][0]), float(m[1][1]), float(m[1][2])};
    a.z = {float(m[2][0]), float(m[2][1]), float(m[2][2])};
    a.t = {float(m[3][0]), float(m[3][1]), float(m[3][2])};
    return a;
}

// A field prim's world transform from the terminal scene index (flattened
// there, so always current: the field bprim is not re-synced when only an
// ancestor moves); the bprim's last value if the scene index has none.
GfMatrix4d fieldWorld(HdSceneDelegate *sd, SdfPath const &field, GfMatrix4d const &fallback) {
    if (const HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex())
        if (HdMatrixDataSourceHandle m = HdXformSchema::GetFromParent(si->GetPrim(field).dataSource).GetMatrix())
            return m->GetTypedValue(0.0f);
    return fallback;
}

// Whether a field of this binding or field name is one the volume shader
// reads (`needed`), also as a vector component (docs/design/M8.md D5).
bool needsField(std::string const &binding, std::string const &fieldName, std::vector<std::string> const &needed) {
    const auto matches = [](std::string const &name, std::string const &n) {
        if (name == n)
            return true;
        return name.size() == n.size() + 2 && name.compare(0, n.size(), n) == 0 &&
               (name[n.size()] == '.' || name[n.size()] == '_') && std::strchr("xyzw", name[n.size() + 1]);
    };
    for (std::string const &n : needed)
        if (matches(binding, n) || matches(fieldName, n))
            return true;
    return false;
}

// A constant float primvar (negative if not authored or not a number). Also
// as an array of one element, the form Houdini's LOPs author constant
// primvars in (float[] karma:object:volumelimit = [2]).
float constantFloat(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name) {
    if (const std::optional<double> v = HdNovaNumber(HdNovaConstantPrimvar(sd, id, name)); v && std::isfinite(*v))
        return float(std::max(*v, 0.0));
    return -1.0f;
}

// The Volume ray limit at the volume's events (D11): Nova's own setting,
// else Karma's (Render Geometry Settings author it as a constant primvar).
float volumeLimitOf(HdSceneDelegate *sd, SdfPath const &id) {
    static const TfToken nova("nova:object:volumeLimit"), karma("karma:object:volumelimit");
    const float own = constantFloat(sd, id, nova);
    return own >= 0.0f ? own : constantFloat(sd, id, karma);
}

// A light source (D13): nova:object:volumeLight, else Karma's
// treat_as_lightsource (0 no, 1 yes, 2 when the material emits: Nova's
// volume lights need emission anyway), with Karma's light multipliers.
struct VolumeLightSettings {
    bool light = false;
    float diffuse = 1.0f, specular = 1.0f;
    bool operator==(const VolumeLightSettings &) const = default;
};
// Karma's volume filter other than box of width 1 (D16): Nova always reads
// fields trilinearly, which is what Karma's default gives (research H 4.2).
bool otherVolumeFilter(HdSceneDelegate *sd, SdfPath const &id) {
    static const TfToken filter("karma:object:volumefilter"), width("karma:object:volumefilterwidth");
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant))
        if (d.name == filter) {
            const VtValue v = sd->Get(id, filter);
            const std::string name = v.IsHolding<std::string>() ? v.UncheckedGet<std::string>()
                                     : v.IsHolding<TfToken>()    ? v.UncheckedGet<TfToken>().GetString()
                                                                 : std::string("box");
            if (name != "box")
                return true;
        }
    const float w = constantFloat(sd, id, width);
    return w >= 0.0f && std::fabs(w - 1.0f) > 1e-6f;
}

// Velocity blur (D17): Nova's nova:object:velocityBlur, else Karma's
// karma:object:vblur, else the render settings' karma:object:vblur (Karma
// blurs volumes by it too, measured; D19's defaults of the render index),
// 1 velocity, 2 acceleration (velocity here); Karma's volumevblurscale.
// Returns the seconds the fields move per frame of shutter (scale / fps),
// 0 if off.
float velocityBlurOf(HdSceneDelegate *sd, SdfPath const &id, float fps) {
    static const TfToken nova("nova:object:velocityBlur"), vblur("karma:object:vblur"),
        scale("karma:object:volumevblurscale");
    float mode = constantFloat(sd, id, nova);
    if (!(mode >= 0.0f))
        mode = constantFloat(sd, id, vblur);
    if (!(mode >= 0.0f))
        if (HdNovaVelocityModeSceneIndexRefPtr velocity = HdNovaVelocityModeSceneIndex::Find(&sd->GetRenderIndex()))
            mode = float(velocity->GetDefaults().velocityBlur);
    if (!(mode >= 0.5f))
        return 0.0f;
    const float s = constantFloat(sd, id, scale);
    return (s >= 0.0f ? s : 1.0f) / (fps > 0.0f ? fps : 24.0f);
}

VolumeLightSettings volumeLightOf(HdSceneDelegate *sd, SdfPath const &id, HdNovaObjectDefaults const &defaults) {
    // Nova's light source setting (meshes and volumes, M9 D14), its M8 name,
    // else Karma's, else the render settings' Karma values
    // (docs/design/M11-P1.md).
    static const TfToken nova("nova:object:lightSource"), legacy("nova:object:volumeLight"),
        karma("karma:object:treat_as_lightsource"), diffuse("karma:object:lightsource_diffusescale"),
        specular("karma:object:lightsource_specularscale");
    VolumeLightSettings s;
    int mode = -1;
    for (TfToken const &name : {nova, legacy, karma}) {
        mode = HdNovaMenuIndex(HdNovaConstantPrimvar(sd, id, name), {"No", "Yes", "Auto"});
        if (mode >= 0)
            break;
    }
    if (mode < 0)
        mode = defaults.lightSource;
    s.light = mode >= 1;
    const float d = constantFloat(sd, id, diffuse), sp = constantFloat(sd, id, specular);
    s.diffuse = d >= 0.0f ? d : defaults.lightDiffuse >= 0.0f ? defaults.lightDiffuse : 1.0f;
    s.specular = sp >= 0.0f ? sp : defaults.lightSpecular >= 0.0f ? defaults.lightSpecular : 1.0f;
    return s;
}

} // namespace

void HdNovaVolumeObjectSettings(HdSceneDelegate *sd, SdfPath const &id, nova::VolumeDesc &desc,
                                HdNovaObjectDefaults const &defaults) {
    desc.volumeLimit = volumeLimitOf(sd, id);
    desc.lpeTag = HdNovaObjectLpeTag(sd, id);
    const VolumeLightSettings light = volumeLightOf(sd, id, defaults);
    desc.light = light.light;
    desc.lightDiffuse = light.diffuse;
    desc.lightSpecular = light.specular;
}

std::vector<std::string> HdNovaVolumeCategories(HdSceneDelegate *sd, SdfPath const &id, SdfPath const &instancerId) {
    std::vector<std::string> categories;
    for (TfToken const &t : sd->GetCategories(id))
        categories.push_back(t.GetString());
    if (!instancerId.IsEmpty())
        for (TfToken const &t : sd->GetCategories(instancerId))
            categories.push_back(t.GetString());
    return categories;
}

HdNovaVolume::HdNovaVolume(SdfPath const &id) : HdVolume(id) {}

HdDirtyBits HdNovaVolume::GetInitialDirtyBitsMask() const {
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtyPrimID | HdChangeTracker::DirtyVolumeField | HdChangeTracker::DirtyInstancer |
           HdChangeTracker::DirtyInstanceIndex | HdChangeTracker::DirtyCategories;
}

void HdNovaVolume::_InitRepr(TfToken const &reprToken, HdDirtyBits *) {
    auto it = std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken));
    if (it == _reprs.end())
        _reprs.emplace_back(reprToken, std::make_shared<HdRepr>());
}

void HdNovaVolume::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits, TfToken const &) {
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    const SdfPath &id = GetId();
    const HdDirtyBits bits = *dirtyBits;
    const bool first = _volumeId == nova::kInvalidId;

    if (bits & HdChangeTracker::DirtyMaterialId)
        SetMaterialId(sd->GetMaterialId(id));
    _UpdateVisibility(sd, dirtyBits);
    // The instancer chain first: the instances' transforms come from it.
    _UpdateInstancer(sd, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sd->GetRenderIndex(), GetInstancerId());
    if (first || (bits & HdChangeTracker::DirtyVolumeField))
        _fields = sd->GetVolumeFieldDescriptors(id);
    // The fields the material's volume shader reads (D5); the default
    // volume reads density (D6). A change of that list re-syncs the volume.
    HdNovaMaterialRegistry &registry = param->GetMaterials();
    const bool materialDirty = first || (bits & HdChangeTracker::DirtyMaterialId);
    if (materialDirty)
        registry.BindMesh(id, GetMaterialId().IsEmpty() ? std::vector<SdfPath>{} : std::vector<SdfPath>{GetMaterialId()});
    std::vector<std::string> needed = GetMaterialId().IsEmpty() ? std::vector<std::string>{}
                                                                 : registry.GetVolumeFields(GetMaterialId());
    if (needed.empty())
        needed = {"density"};
    // Velocity blur reads the velocity field (Houdini's vel, or v) as well.
    bool blurDirty = false;
    if (first || (bits & HdChangeTracker::DirtyPrimvar)) {
        const float blur = velocityBlurOf(sd, id, param->GetFps());
        blurDirty = blur != _velocityBlur;
        _velocityBlur = blur;
    }
    // The frame alone with Karma's per-object motion blur off.
    const HdNovaShutter shutterNow = HdNovaObjectShutter(sd, id, *param);
    const bool velocityBlur = _velocityBlur > 0.0f && shutterNow.IsBlurred();
    if (velocityBlur) {
        needed.push_back("vel");
        needed.push_back("v");
    }

    // The field bprims and their versions: a field edit reloads (a SOP
    // recook dirties only the field).
    HdRenderIndex &index = sd->GetRenderIndex();
    std::vector<HdNovaField *> fieldPrims(_fields.size(), nullptr);
    std::vector<uint64_t> versions(_fields.size(), 0);
    for (size_t i = 0; i < _fields.size(); ++i) {
        fieldPrims[i] = dynamic_cast<HdNovaField *>(index.GetBprim(_fields[i].fieldPrimType, _fields[i].fieldId));
        versions[i] = fieldPrims[i] ? fieldPrims[i]->GetVersion() : 0;
    }

    const HdNovaShutter shutter = shutterNow;
    const unsigned maxSteps = param->GetMaxMotionSteps();
    const bool newShutter = !(shutter == _sampledShutter) || maxSteps != _sampledMaxSteps;
    const bool transformDirty = first || newShutter || (bits & HdChangeTracker::DirtyTransform);
    // Instancing (audit V-0): the instances' transforms are sampled again when
    // the instancer or the prim's instances change; the grids stay as they are.
    const bool instanced = !GetInstancerId().IsEmpty();
    const bool resample = transformDirty || instanced != _instanced ||
                          (bits & (HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex));
    const bool fieldsDirty = first || (bits & HdChangeTracker::DirtyVolumeField) || versions != _fieldVersions;
    // Per-object settings (constant primvars).
    bool settingsDirty = blurDirty;
    bool visibilitySettingsDirty = false;
    if (first || (bits & HdChangeTracker::DirtyPrimvar)) {
        // Ray visibility and holdouts (docs/design/M11-P2.md D15): no rebuild.
        const nova::ObjectVisibility visibility = HdNovaObjectVisibility(sd, id);
        visibilitySettingsDirty = !(visibility == _visibility);
        _visibility = visibility;
        const float limit = volumeLimitOf(sd, id);
        std::string tag = HdNovaObjectLpeTag(sd, id);
        const VolumeLightSettings light = volumeLightOf(sd, id, param->GetObjectDefaults());
        settingsDirty |= limit != _volumeLimit || tag != _lpeTag || light.light != _light ||
                         light.diffuse != _lightDiffuse || light.specular != _lightSpecular;
        if (!_warnedFilter && otherVolumeFilter(sd, id)) {
            HD_NOVA_WARN("volume %s: Nova reads volume fields trilinearly; karma:object:volumefilter other than box "
                         "of width 1 is ignored",
                         id.GetText());
            _warnedFilter = true;
        }
        _volumeLimit = limit;
        _lpeTag = std::move(tag);
        _light = light.light;
        _lightDiffuse = light.diffuse;
        _lightSpecular = light.specular;
    }
    const bool rebuild = fieldsDirty || transformDirty || materialDirty || needed != _neededFields ||
                         settingsDirty || (bits & HdChangeTracker::DirtyPrimID);
    const bool visibilityDirty = bits & HdChangeTracker::DirtyVisibility;
    const bool categoriesDirty = first || (bits & (HdChangeTracker::DirtyCategories | HdChangeTracker::DirtyInstancer));
    if (!(rebuild || resample || visibilityDirty || visibilitySettingsDirty || categoriesDirty)) {
        *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
        return;
    }

    // Hydra-side work first, while the render keeps running: the fields
    // placed in the volume's object space.
    nova::VolumeDesc desc;
    std::vector<GfMatrix4d> sampled;
    if (rebuild) {
        const GfMatrix4d toObject = sd->GetTransform(id).GetInverse();
        for (size_t i = 0; i < _fields.size(); ++i) {
            HdNovaField *field = fieldPrims[i];
            const std::string binding = _fields[i].fieldName.GetString();
            if (!field || !needsField(binding, field->GetFieldName(), needed))
                continue;
            const HdNovaField::Grids grids = field->Load();
            const nova::Affine3f fieldToObject =
                toAffine(fieldWorld(sd, _fields[i].fieldId, grids.transform) * toObject);
            if (grids.grids.size() == 1) {
                desc.fields.push_back({binding, grids.fieldName, grids.grids[0], fieldToObject});
            } else if (grids.grids.size() == 3) {
                static const char *suffix[3] = {".x", ".y", ".z"};
                for (int c = 0; c < 3; ++c)
                    desc.fields.push_back(
                        {binding + suffix[c], grids.fieldName + suffix[c], grids.grids[size_t(c)], fieldToObject});
            }
        }
        desc.name = id.GetString();
        desc.visible = IsVisible();
        desc.userId = GetPrimId();
        desc.volumeLimit = _volumeLimit;
        desc.lpeTag = _lpeTag;
        desc.light = _light;
        desc.lightDiffuse = _lightDiffuse;
        desc.lightSpecular = _lightSpecular;
        if (velocityBlur) {
            for (const char *name : {"vel", "v"}) {
                const std::string n(name);
                if (desc.findField(n + ".x") || desc.findField(n + "_x")) {
                    desc.velocity = n;
                    break;
                }
            }
            // Seconds from the fields' time (the frame) at shutter open and close.
            desc.velocityTime[0] = shutterNow.open * _velocityBlur;
            desc.velocityTime[1] = shutterNow.close * _velocityBlur;
        }
        if (desc.fields.empty())
            HD_NOVA_WARN("volume %s has none of the fields its material reads (%s)", id.GetText(),
                         TfStringJoin(needed, ", ").c_str());
        TF_DEBUG(HDNOVA_VOLUME).Msg("[Nova] volume %s: %zu bindings, %zu fields loaded\n", id.GetText(),
                                    _fields.size(), desc.fields.size());
    }
    // The transforms over the shutter, per scene volume: the prim's own, or
    // one set per instance.
    std::vector<std::vector<nova::Mat4>> steps;
    if (resample) {
        _instanced = instanced;
        _noInstances = false;
        if (instanced) {
            if (auto *instancer = static_cast<HdNovaInstancer *>(sd->GetRenderIndex().GetInstancer(GetInstancerId())))
                steps = HdNovaInstanceSteps(sd, id, *instancer, shutter, maxSteps);
            _noInstances = steps.empty();
        } else {
            HdNovaSampleTransform(sd, id, shutter, maxSteps, &sampled);
            steps.emplace_back();
            for (GfMatrix4d const &m : sampled)
                steps.back().push_back(toNova(m));
        }
    }
    const size_t count = resample ? std::max<size_t>(steps.size(), 1) : _instanceVolumeIds.size() + 1;
    _sampledShutter = shutter;
    _sampledMaxSteps = maxSteps;
    _fieldVersions = versions;
    _neededFields = needed;

    nova::Scene &scene = param->AcquireSceneForEdit();
    // Stable keys, not the sync order: the path, and the instance's index
    // behind the first.
    if (first)
        _volumeId = scene.createVolume(nova::objectKey(id.GetString()));
    std::vector<nova::VolumeId> added;
    while (_instanceVolumeIds.size() + 1 < count) {
        _instanceVolumeIds.push_back(scene.createVolume(nova::objectKey(id.GetString(), _instanceVolumeIds.size() + 1)));
        added.push_back(_instanceVolumeIds.back());
    }
    while (_instanceVolumeIds.size() + 1 > count) {
        scene.removeVolume(_instanceVolumeIds.back());
        _instanceVolumeIds.pop_back();
    }
    const bool visible = IsVisible() && !_noInstances;
    if (rebuild) {
        desc.material = registry.Acquire(GetMaterialId(), scene);
        _desc = std::move(desc);
        added.assign(_instanceVolumeIds.begin(), _instanceVolumeIds.end());
        added.push_back(_volumeId);
    }
    // New volumes and, after a rebuild, all of them: the shared description
    // (the grids are shared, each volume builds its own bounds).
    for (nova::VolumeId volume : added) {
        nova::VolumeDesc d = _desc;
        d.visible = visible;
        scene.updateVolume(volume, std::move(d));
    }
    const std::vector<std::string> categories = HdNovaVolumeCategories(sd, id, GetInstancerId());
    for (size_t k = 0; k < count; ++k) {
        const nova::VolumeId volume = k == 0 ? _volumeId : _instanceVolumeIds[k - 1];
        if (!rebuild && (visibilityDirty || resample))
            scene.setVolumeVisible(volume, visible);
        scene.setVolumeVisibility(volume, _visibility); // a no-op while unchanged
        scene.setVolumeCategories(volume, categories);  // light and shadow linking; the same
        if (k < steps.size() && !steps[k].empty())
            scene.updateVolumeMotion(volume, steps[k]);
    }
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

void HdNovaVolume::Finalize(HdRenderParam *renderParam) {
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    param->GetMaterials().BindMesh(GetId(), {});
    if (_volumeId != nova::kInvalidId) {
        nova::Scene &scene = param->AcquireSceneForEdit();
        scene.removeVolume(_volumeId);
        for (nova::VolumeId volume : _instanceVolumeIds)
            scene.removeVolume(volume);
        _volumeId = nova::kInvalidId;
        _instanceVolumeIds.clear();
    }
    _desc = nova::VolumeDesc{};
}

PXR_NAMESPACE_CLOSE_SCOPE
