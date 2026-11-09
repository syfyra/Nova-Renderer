#include "medium.h"

#include "diagnostics.h"
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
                        HdNovaShutter const &shutter) {
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
    std::vector<GfMatrix4d> sampled;
    const bool first = _volumeId == nova::kInvalidId;
    if (first || transformDirty)
        HdNovaSampleTransform(sd, id, shutter, param.GetMaxMotionSteps(), &sampled);

    nova::Scene &scene = param.AcquireSceneForEdit();
    if (first)
        _volumeId = scene.createVolume(nova::objectKey(id.GetString())); // stable, not the sync order
    desc.material = registry.Acquire(material, scene);
    scene.updateVolume(_volumeId, std::move(desc));
    // Ray visibility and holdouts (docs/design/M11-P2.md D15), as a surface's.
    scene.setVolumeVisibility(_volumeId, HdNovaObjectVisibility(sd, id));
    if (!sampled.empty()) {
        std::vector<nova::Mat4> steps;
        for (GfMatrix4d const &m : sampled)
            steps.push_back(toNova(m));
        scene.updateVolumeMotion(_volumeId, steps);
    }
}

void HdNovaMedium::Remove(HdNovaRenderParam &param) {
    if (_volumeId != nova::kInvalidId) {
        param.AcquireSceneForEdit().removeVolume(_volumeId);
        _volumeId = nova::kInvalidId;
    }
    _shell.reset();
}

PXR_NAMESPACE_CLOSE_SCOPE
