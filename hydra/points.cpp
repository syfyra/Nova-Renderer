#include "points.h"

#include "diagnostics.h"
#include "instancer.h"
#include "primvarUtil.h"
#include "renderParam.h"

#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/tokens.h>

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

nova::Mat4 toNova(GfMatrix4d const &m) {
    nova::Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = m[i][j];
    return r;
}
// Constant float primvars of the per-object settings.
float constantPrimvar(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name, float fallback) {
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant))
        if (d.name == name) {
            const VtValue v = VtValue::Cast<float>(sd->Get(id, name));
            if (v.IsHolding<float>() && std::isfinite(v.UncheckedGet<float>()))
                return v.UncheckedGet<float>();
        }
    return fallback;
}

// A per-object setting (docs/design/M9.md D1, D2): the prim's constant
// primvar (Nova's name first, then Karma's), else `fallback` (the render
// settings' Karma value, docs/design/M11-P1.md, or the default).
float objectSetting(HdSceneDelegate *sd, SdfPath const &id, TfToken const &nova, TfToken const &karma, float fallback) {
    const float unset = -1e30f;
    float v = constantPrimvar(sd, id, nova, unset);
    if (v == unset)
        v = constantPrimvar(sd, id, karma, unset);
    return v != unset ? v : fallback;
}

// The point style menu (Karma's karma:object:point_style): an index or, as
// Karma's nodes may author it, a label.
int pointStyleOf(HdSceneDelegate *sd, SdfPath const &id, int fallback) {
    static const TfToken nova("nova:object:pointStyle"), karma("karma:object:point_style");
    for (TfToken const &name : {nova, karma}) {
        const int style = HdNovaMenuIndex(HdNovaConstantPrimvar(sd, id, name), {"Discs", "Spheres", "Oriented Discs"});
        if (style >= 0)
            return style;
    }
    return fallback;
}

} // namespace

HdNovaPoints::HdNovaPoints(SdfPath const &id) : HdPoints(id) {}

HdDirtyBits HdNovaPoints::GetInitialDirtyBitsMask() const {
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyWidths | HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyPrimID | HdChangeTracker::DirtyInstanceIndex | HdChangeTracker::DirtyCategories;
}

void HdNovaPoints::_InitRepr(TfToken const &reprToken, HdDirtyBits *) {
    auto it = std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken));
    if (it == _reprs.end())
        _reprs.emplace_back(reprToken, std::make_shared<HdRepr>());
}

bool HdNovaPoints::_Build(HdSceneDelegate *sd, HdNovaShutter const &shutter, unsigned maxSteps,
                          HdNovaObjectDefaults const &defaults, nova::MeshDesc &desc) const {
    const SdfPath &id = GetId();
    std::vector<VtVec3fArray> steps;
    if (!HdNovaSampleVec3fArray(sd, id, HdTokens->points, shutter, maxSteps, &steps) || steps.empty() ||
        steps[0].empty())
        return false;
    const size_t n = steps[0].size();
    desc = nova::MeshDesc{};
    desc.shape = nova::MeshShape::Points;
    desc.pointSteps = uint32_t(steps.size());
    for (VtVec3fArray const &s : steps) {
        if (s.size() != n)
            return false;
        for (GfVec3f const &p : s)
            desc.points.push_back({p[0], p[1], p[2]});
    }

    // Widths (diameters, D1): constant or per point; USD's fallback width 1.
    static const TfToken novaWidthScale("nova:object:widthScale"), karmaWidthScale("karma:object:width_scale");
    const float widthScale = std::max(
        objectSetting(sd, id, novaWidthScale, karmaWidthScale, defaults.widthScale >= 0.0f ? defaults.widthScale : 1.0f),
        0.0f);
    desc.pointRadius.assign(n, 0.5f * widthScale);
    for (int i = 0; i < HdInterpolationCount; ++i)
        for (const HdPrimvarDescriptor &pv : sd->GetPrimvarDescriptors(id, HdInterpolation(i)))
            if (pv.name == HdTokens->widths) {
                int width = 0;
                std::vector<float> widths;
                if (!HdNovaPrimvarToFloats(sd->Get(id, HdTokens->widths), &width, &widths) || width != 1 ||
                    widths.empty())
                    break;
                if (widths.size() >= n && i != HdInterpolationConstant && i != HdInterpolationUniform)
                    for (size_t k = 0; k < n; ++k)
                        desc.pointRadius[k] = std::max(0.5f * widths[k] * widthScale, 0.0f);
                else
                    desc.pointRadius.assign(n, std::max(0.5f * widths[0] * widthScale, 0.0f));
                break;
            }

    // Karma's karma:object:point_style: 0 discs facing the ray, 1 spheres
    // (the default), 2 discs facing the normals.
    const int style = pointStyleOf(sd, id, defaults.pointStyle >= 0 ? defaults.pointStyle : 1);
    desc.pointStyle = style == 0 ? nova::PointStyle::Disc : nova::PointStyle::Sphere;
    if (style == 2) {
        const VtValue v = sd->Get(id, HdTokens->normals);
        if (v.IsHolding<VtVec3fArray>() && v.UncheckedGet<VtVec3fArray>().size() >= n) {
            for (size_t k = 0; k < n; ++k) {
                const GfVec3f &no = v.UncheckedGet<VtVec3fArray>()[k];
                desc.normals.push_back({no[0], no[1], no[2]});
            }
            desc.pointStyle = nova::PointStyle::OrientedDisc;
        } else {
            desc.pointStyle = nova::PointStyle::Disc;
        }
    }

    // Shading primvars: per point (vertex, varying), else constant.
    for (int i = 0; i < HdInterpolationCount; ++i) {
        const HdInterpolation interp = HdInterpolation(i);
        for (const HdPrimvarDescriptor &pv : sd->GetPrimvarDescriptors(id, interp)) {
            if (HdNovaIsGeometryPrimvar(pv.name) || pv.name == HdTokens->widths)
                continue;
            int w = 0;
            std::vector<float> data;
            if (!HdNovaPrimvarToFloats(sd->Get(id, pv.name), &w, &data) || w < 1 || w > 4 || data.empty())
                continue;
            nova::MeshAttribute a;
            a.name = pv.name.GetString();
            a.width = uint8_t(w);
            if (interp == HdInterpolationConstant || interp == HdInterpolationUniform || data.size() < n * size_t(w)) {
                a.rate = nova::AttributeRate::Constant;
                data.resize(size_t(w));
            } else {
                a.rate = nova::AttributeRate::Uniform; // one value per point (primitive)
                data.resize(n * size_t(w));
            }
            a.data = std::move(data);
            desc.setAttribute(std::move(a));
        }
    }
    return true;
}
void HdNovaPoints::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits,
                             TfToken const &) {
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    const SdfPath &id = GetId();
    const HdDirtyBits bits = *dirtyBits;
    const bool first = _meshId == nova::kInvalidId;

    if (bits & HdChangeTracker::DirtyMaterialId)
        SetMaterialId(sd->GetMaterialId(id));
    _UpdateVisibility(sd, dirtyBits);
    _UpdateInstancer(sd, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sd->GetRenderIndex(), GetInstancerId());

    const HdNovaShutter shutter = HdNovaObjectShutter(sd, id, *param); // Karma's per-object mblur
    const unsigned maxSteps = param->GetMaxMotionSteps();
    const bool newShutter = !(shutter == _sampledShutter) || maxSteps != _sampledMaxSteps;
    const bool rebuild = first || newShutter ||
                         (bits & (HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
                                  HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyWidths |
                                  HdChangeTracker::DirtyNormals));
    const bool instancesDirty =
        first || newShutter ||
        (bits & (HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex));
    const bool visibilityDirty = bits & HdChangeTracker::DirtyVisibility;
    const bool primIdDirty = bits & HdChangeTracker::DirtyPrimID;
    const bool materialsDirty = first || (bits & HdChangeTracker::DirtyMaterialId);
    const bool categoriesDirty = first || (bits & (HdChangeTracker::DirtyCategories | HdChangeTracker::DirtyInstancer |
                                                   HdChangeTracker::DirtyInstanceIndex));
    HdNovaMaterialRegistry &registry = param->GetMaterials();
    if (materialsDirty)
        registry.BindMesh(id, {GetMaterialId()});
    if (!(rebuild || instancesDirty || visibilityDirty || primIdDirty || materialsDirty || categoriesDirty)) {
        *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
        return;
    }

    // Hydra-side work first, while the render keeps running.
    nova::MeshDesc desc;
    bool built = false;
    if (rebuild) {
        built = _Build(sd, shutter, maxSteps, param->GetObjectDefaults(), desc);
        desc.transform = toNova(sd->GetTransform(id));
        desc.visible = IsVisible();
        desc.userId = GetPrimId();
    }
    std::vector<nova::Xform34> instances;
    std::vector<nova::SrtTransform> instanceKeys;
    std::vector<nova::InstanceAttribute> instanceAttributes;
    uint32_t instanceSteps = 1;
    std::vector<nova::Mat4> xfSteps;
    const bool instanced = !GetInstancerId().IsEmpty();
    if (instancesDirty && instanced) {
        auto *instancer = static_cast<HdNovaInstancer *>(sd->GetRenderIndex().GetInstancer(GetInstancerId()));
        if (TF_VERIFY(instancer)) {
            instancer->SyncShutter(shutter);
            HdNovaComputeInstances(sd, id, *instancer, shutter, maxSteps, instances, instanceKeys, instanceSteps);
            const size_t count = instanceSteps > 1 ? instanceKeys.size() / instanceSteps : instances.size();
            instancer->ComputeInstanceAttributes(id, instanceAttributes);
            for (const nova::InstanceAttribute &a : instanceAttributes)
                if (a.data.size() != count * a.width)
                    instanceAttributes.clear();
        }
    } else if (instancesDirty) {
        std::vector<GfMatrix4d> sampled;
        HdNovaSampleTransform(sd, id, shutter, maxSteps, &sampled);
        for (GfMatrix4d const &m : sampled)
            xfSteps.push_back(toNova(m));
    }
    _sampledShutter = shutter;
    _sampledMaxSteps = maxSteps;

    nova::Scene &scene = param->AcquireSceneForEdit();
    if (first)
        _meshId = scene.createMesh();
    if (rebuild)
        scene.updateMesh(_meshId, built ? std::move(desc) : nova::MeshDesc{});
    if (instancesDirty) {
        if (instanced && instanceSteps > 1)
            scene.setMeshInstanceMotion(_meshId, std::move(instanceKeys), instanceSteps);
        else if (instanced)
            scene.setMeshInstances(_meshId, std::move(instances));
        else
            scene.updateMeshMotion(_meshId, xfSteps);
        if (instanced)
            scene.setMeshInstanceAttributes(_meshId, std::move(instanceAttributes));
    }
    if (rebuild) { // DirtyPrimvar rebuilds
        scene.setMeshRayLimits(_meshId, HdNovaObjectOverrides(sd, id));
        scene.setMeshLpeTag(_meshId, HdNovaObjectLpeTag(sd, id));
        scene.setMeshVisibility(_meshId, HdNovaObjectVisibility(sd, id)); // M11 P2
        HdNovaCryptoNames &names = param->GetCryptoNames(); // cryptomatte ids (M6 D10)
        scene.setMeshCrypto(_meshId, names.Note("__name", HdNovaCryptoObjectName(sd, id, GetInstancerId())),
                            HdNovaCryptoAttributes(sd, id, names, GetInstancerId()));
    }
    if (categoriesDirty)
        HdNovaSyncCategories(sd, id, GetInstancerId(), scene, _meshId); // light linking (M9 D15)
    if (materialsDirty) {
        const nova::MaterialId material = registry.Acquire(GetMaterialId(), scene);
        scene.setMeshMaterials(_meshId, material == nova::kDefaultMaterialId ? std::vector<nova::MaterialId>{}
                                                                             : std::vector<nova::MaterialId>{material});
    }
    if (!rebuild) {
        if (visibilityDirty)
            scene.setMeshVisible(_meshId, IsVisible());
        if (primIdDirty)
            scene.setMeshUserId(_meshId, GetPrimId());
    }
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

void HdNovaPoints::Finalize(HdRenderParam *renderParam) {
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    param->GetMaterials().BindMesh(GetId(), {});
    if (_meshId != nova::kInvalidId) {
        param->AcquireSceneForEdit().removeMesh(_meshId);
        _meshId = nova::kInvalidId;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
