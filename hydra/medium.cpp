#include "medium.h"

#include "diagnostics.h"
#include "instancer.h"
#include "motion.h"
#include "primvarUtil.h"
#include "renderParam.h"
#include "volume.h"

#include <pxr/base/tf/token.h>

#include <algorithm>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

nova::Mat4 toNova(GfMatrix4d const &m) {
    nova::Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = m[i][j];
    return r;
}

// The first value of a numeric constant primvar (scalar or array: the Fog
// Box authors one-element arrays), if it has one.
bool constantNumber(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name, float *value) {
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant))
        if (d.name == name) {
            int width = 0;
            std::vector<float> data;
            if (HdNovaPrimvarToFloats(sd->Get(id, name), &width, &data) && !data.empty()) {
                *value = data[0];
                return true;
            }
        }
    return false;
}

// The constant primvars a medium's shader reads (by name), as its constants.
std::vector<nova::VolumeConstant> constantsOf(HdSceneDelegate *sd, SdfPath const &id,
                                              std::vector<std::string> const &reads) {
    std::vector<nova::VolumeConstant> out;
    if (reads.empty())
        return out;
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant)) {
        if (std::find(reads.begin(), reads.end(), d.name.GetString()) == reads.end())
            continue;
        int width = 0;
        std::vector<float> data;
        if (!HdNovaPrimvarToFloats(sd->Get(id, d.name), &width, &data) || width < 1 || width > 4 ||
            data.size() < size_t(width))
            continue;
        data.resize(size_t(width));
        out.push_back({d.name.GetString(), std::move(data)});
    }
    return out;
}

} // namespace

std::vector<std::vector<nova::Mat4>> HdNovaInstanceSteps(HdSceneDelegate *sd, SdfPath const &id,
                                                         HdNovaInstancer const &instancer, HdNovaShutter const &shutter,
                                                         unsigned maxSteps) {
    HdNovaTransformSamples prototype;
    HdNovaSampleTransformSamples(sd, id, shutter, &prototype);
    const unsigned n = std::max(instancer.GetMotionStepCount(shutter, maxSteps), prototype.StepCount(shutter, maxSteps));
    std::vector<std::vector<nova::Mat4>> out;
    std::vector<GfMatrix4d> xf;
    bool sampled = n > 1;
    for (unsigned s = 0; s < n && sampled; ++s) {
        const float t = shutter.TimeAt(float(s) / float(n - 1));
        instancer.ComputeInstanceTransformsAt(id, prototype.At(t), shutter, t, xf);
        if (s == 0)
            out.assign(xf.size(), std::vector<nova::Mat4>());
        else if (xf.size() != out.size())
            sampled = false;
        for (size_t k = 0; sampled && k < xf.size(); ++k)
            out[k].push_back(toNova(xf[k]));
    }
    if (!sampled) {
        const float t = shutter.FrameOffset();
        instancer.ComputeInstanceTransformsAt(id, prototype.At(t), shutter, t, xf);
        out.assign(xf.size(), std::vector<nova::Mat4>());
        for (size_t k = 0; k < xf.size(); ++k)
            out[k].push_back(toNova(xf[k]));
    }
    // An instance at rest keeps its exact transform (no keys).
    for (std::vector<nova::Mat4> &steps : out)
        if (steps.size() > 1 && std::all_of(steps.begin() + 1, steps.end(), [&](nova::Mat4 const &m) {
                return std::equal(&m.m[0][0], &m.m[0][0] + 16, &steps[0].m[0][0]);
            }))
            steps.resize(1);
    return out;
}

bool HdNovaIsMedium(HdSceneDelegate *sd, SdfPath const &id, SdfPath const &material,
                    HdNovaMaterialRegistry const &registry) {
    static const TfToken uniformToken("karma:object:volumeuniform");
    const HdNovaMaterialKind kind = material.IsEmpty() ? HdNovaMaterialKind{} : registry.GetMaterialKind(material);
    float flag = 0.0f;
    const bool uniform = kind.volumeUniform || (constantNumber(sd, id, uniformToken, &flag) && flag != 0.0f);
    if (kind.volume)
        return uniform || !kind.surface; // volumeuniform makes even a material with a surface a medium
    // No volume shader: a uniform prim without a material is Karma's white
    // medium of density 1; one with a surface material stays a surface.
    return uniform && !kind.surface && material.IsEmpty();
}

void HdNovaMedium::Sync(HdSceneDelegate *sd, HdNovaRenderParam &param, SdfPath const &id, SdfPath const &material,
                        bool visible, int primId, nova::MediumShellPtr shell, bool transformDirty,
                        HdNovaShutter const &shutter, SdfPath const &instancerId) {
    if (shell)
        _shell = std::move(shell);
    if (!_shell) { // nothing inside: no volume
        Remove(param);
        return;
    }
    if (_shell->openEdges > 0 && !_warnedOpen) {
        HD_NOVA_WARN("medium %s: its surface is not closed (%u open edges); its inside is taken per ray as an odd "
                     "number of crossings ahead, as if its holes were closed by its bounding box",
                     id.GetText(), _shell->openEdges);
        _warnedOpen = true;
    }
    HdNovaMaterialRegistry &registry = param.GetMaterials();
    nova::VolumeDesc desc;
    desc.name = id.GetString();
    desc.visible = visible;
    desc.userId = primId;
    desc.shell = _shell;
    HdNovaVolumeObjectSettings(sd, id, desc, param.GetObjectDefaults());
    desc.constants = constantsOf(sd, id, material.IsEmpty() ? std::vector<std::string>{} : registry.GetVolumeFields(material));
    // The transforms over the shutter, per volume: the gprim's own, or one
    // set per instance (Karma renders every instance of a uniform volume,
    // measured: tests/husk/check_karma_fogbox.py `instances`).
    const bool first = _volumeId == nova::kInvalidId;
    const bool instanced = !instancerId.IsEmpty();
    const bool resample = first || transformDirty || instanced != _instanced;
    std::vector<std::vector<nova::Mat4>> steps;
    if (resample) {
        _instanced = instanced;
        _noInstances = false;
        if (instanced) {
            if (auto *instancer = static_cast<HdNovaInstancer *>(sd->GetRenderIndex().GetInstancer(instancerId)))
                steps = HdNovaInstanceSteps(sd, id, *instancer, shutter, param.GetMaxMotionSteps());
            _noInstances = steps.empty();
        } else {
            std::vector<GfMatrix4d> sampled;
            HdNovaSampleTransform(sd, id, shutter, param.GetMaxMotionSteps(), &sampled);
            steps.emplace_back();
            for (GfMatrix4d const &m : sampled)
                steps.back().push_back(toNova(m));
        }
    }
    const size_t count = resample ? std::max<size_t>(steps.size(), 1) : _instanceVolumeIds.size() + 1;

    nova::Scene &scene = param.AcquireSceneForEdit();
    // Stable keys, not the sync order: the path, and the instance's index
    // behind the first.
    if (first)
        _volumeId = scene.createVolume(nova::objectKey(id.GetString()));
    while (_instanceVolumeIds.size() + 1 < count)
        _instanceVolumeIds.push_back(scene.createVolume(nova::objectKey(id.GetString(), _instanceVolumeIds.size() + 1)));
    while (_instanceVolumeIds.size() + 1 > count) {
        scene.removeVolume(_instanceVolumeIds.back());
        _instanceVolumeIds.pop_back();
    }
    desc.material = registry.Acquire(material, scene);
    if (_noInstances)
        desc.visible = false;
    // Ray visibility and holdouts (docs/design/M11-P2.md D15), as a surface's.
    const nova::ObjectVisibility visibility = HdNovaObjectVisibility(sd, id);
    // Light and shadow linking, as a surface's (the whole prim).
    const std::vector<std::string> categories = HdNovaVolumeCategories(sd, id, instancerId);
    for (size_t k = 0; k < count; ++k) {
        const nova::VolumeId volume = k == 0 ? _volumeId : _instanceVolumeIds[k - 1];
        scene.updateVolume(volume, k + 1 < count ? desc : std::move(desc));
        scene.setVolumeVisibility(volume, visibility);
        scene.setVolumeCategories(volume, categories);
        if (k < steps.size() && !steps[k].empty())
            scene.updateVolumeMotion(volume, steps[k]);
    }
}

void HdNovaMedium::Remove(HdNovaRenderParam &param) {
    if (_volumeId != nova::kInvalidId) {
        nova::Scene &scene = param.AcquireSceneForEdit();
        scene.removeVolume(_volumeId);
        for (nova::VolumeId volume : _instanceVolumeIds)
            scene.removeVolume(volume);
        _volumeId = nova::kInvalidId;
        _instanceVolumeIds.clear();
    }
    _instanced = _noInstances = false;
    _shell.reset();
}

PXR_NAMESPACE_CLOSE_SCOPE
