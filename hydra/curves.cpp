#include "curves.h"

#include "diagnostics.h"
#include "instancer.h"
#include "primvarUtil.h"
#include "renderParam.h"

#include "core/geometry/curves.h"

#include <pxr/imaging/hd/basisCurvesTopology.h>
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

nova::CurveBasis basisOf(HdBasisCurvesTopology const &topology) {
    if (topology.GetCurveType() != HdTokens->cubic)
        return nova::CurveBasis::Linear;
    const TfToken &b = topology.GetCurveBasis();
    if (b == HdTokens->bezier)
        return nova::CurveBasis::Bezier;
    if (b == HdTokens->catmullRom)
        return nova::CurveBasis::CatmullRom;
    return nova::CurveBasis::BSpline; // bspline (and anything unknown)
}

// How the authored control points of each curve become an open curve's:
// periodic curves repeat their first points (as many as one segment
// needs), pinned B-spline and Catmull-Rom curves get a phantom point at
// each end (2 p0 - p1, USD's rule) so they pass through their end points.
struct CurveLayout {
    std::vector<uint32_t> counts; // control points per curve after expansion
    // Per expanded control point: the authored point, or for a phantom the
    // pair (first, second) it reflects (second != kNone).
    std::vector<uint32_t> source, reflect;
    static constexpr uint32_t kNone = ~0u;
};

CurveLayout layoutOf(HdBasisCurvesTopology const &topology, nova::CurveBasis basis) {
    CurveLayout l;
    const VtIntArray &counts = topology.GetCurveVertexCounts();
    const TfToken &wrap = topology.GetCurveWrap();
    const bool periodic = wrap == HdTokens->periodic;
    const bool pinned = wrap == HdTokens->pinned &&
                        (basis == nova::CurveBasis::BSpline || basis == nova::CurveBasis::CatmullRom);
    const uint32_t repeat = basis == nova::CurveBasis::Linear || basis == nova::CurveBasis::Bezier ? 1u : 3u;
    uint32_t base = 0;
    for (int c : counts) {
        const uint32_t n = c > 0 ? uint32_t(c) : 0u;
        uint32_t added = 0;
        if (pinned && n >= 2) {
            l.source.push_back(base);
            l.reflect.push_back(base + 1);
            ++added;
        }
        for (uint32_t i = 0; i < n; ++i) {
            l.source.push_back(base + i);
            l.reflect.push_back(CurveLayout::kNone);
        }
        if (periodic && n > 0) {
            for (uint32_t i = 0; i < std::min(repeat, n); ++i) {
                l.source.push_back(base + i);
                l.reflect.push_back(CurveLayout::kNone);
            }
            added += std::min(repeat, n);
        }
        if (pinned && n >= 2) {
            l.source.push_back(base + n - 1);
            l.reflect.push_back(base + n - 2);
            ++added;
        }
        l.counts.push_back(n + added);
        base += n;
    }
    return l;
}

// The expanded values of a per-control-point array of `width` floats.
std::vector<float> expand(CurveLayout const &l, const float *data, size_t width) {
    std::vector<float> out(l.source.size() * width);
    for (size_t i = 0; i < l.source.size(); ++i)
        for (size_t k = 0; k < width; ++k) {
            const float a = data[size_t(l.source[i]) * width + k];
            out[i * width + k] = l.reflect[i] == CurveLayout::kNone ? a : 2.0f * a - data[size_t(l.reflect[i]) * width + k];
        }
    return out;
}

std::vector<nova::Vec3f> expandPoints(CurveLayout const &l, VtVec3fArray const &points) {
    std::vector<nova::Vec3f> out(l.source.size());
    for (size_t i = 0; i < l.source.size(); ++i) {
        const GfVec3f &a = points[l.source[i]];
        nova::Vec3f p{a[0], a[1], a[2]};
        if (l.reflect[i] != CurveLayout::kNone) {
            const GfVec3f &b = points[l.reflect[i]];
            p = p * 2.0f - nova::Vec3f{b[0], b[1], b[2]};
        }
        out[i] = p;
    }
    return out;
}

// Per expanded curve: the authored values of one curve's segment ends
// (varying rate) mapped onto its segment ends after expansion. Periodic
// curves close their varying values with the first one.
std::vector<float> expandVarying(HdBasisCurvesTopology const &topology, CurveLayout const &l,
                                 nova::CurveBasis basis, const float *data, size_t count, size_t width) {
    std::vector<float> out;
    const bool periodic = topology.GetCurveWrap() == HdTokens->periodic;
    const VtIntArray &counts = topology.GetCurveVertexCounts();
    size_t offset = 0;
    for (size_t c = 0; c < counts.size(); ++c) {
        const uint32_t segments = nova::curveSegmentCount(basis, l.counts[c]);
        const size_t authored = periodic ? segments : segments + 1;
        if (offset + authored > count)
            return {};
        for (size_t j = 0; j <= segments && segments > 0; ++j) {
            const size_t src = offset + (j < authored ? j : 0);
            for (size_t k = 0; k < width; ++k)
                out.push_back(data[src * width + k]);
        }
        offset += authored;
    }
    return out;
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

// A per-object curve setting (docs/design/M9.md D2): the prim's constant
// primvar (Nova's name first, then Karma's), else `fallback` (the render
// settings' Karma value, docs/design/M11-P1.md, or the default).
float curveSetting(HdSceneDelegate *sd, SdfPath const &id, TfToken const &nova, TfToken const &karma, float fallback) {
    const float unset = -1e30f;
    float v = constantPrimvar(sd, id, nova, unset);
    if (v == unset)
        v = constantPrimvar(sd, id, karma, unset);
    return v != unset ? v : fallback;
}

// The curve style menu (Karma's karma:object:curve_style): an index or, as
// Karma's nodes may author it, a label.
int curveStyleOf(HdSceneDelegate *sd, SdfPath const &id, int fallback) {
    static const TfToken nova("nova:object:curveStyle"), karma("karma:object:curve_style");
    for (TfToken const &name : {nova, karma}) {
        const int style = HdNovaMenuIndex(HdNovaConstantPrimvar(sd, id, name),
                                          {"Flat Ribbons", "Rounded Curves", "Oriented Ribbons", "USD Default Behaviour"});
        if (style >= 0)
            return style;
    }
    return fallback;
}

} // namespace

HdNovaBasisCurves::HdNovaBasisCurves(SdfPath const &id) : HdBasisCurves(id) {}

HdDirtyBits HdNovaBasisCurves::GetInitialDirtyBitsMask() const {
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyWidths | HdChangeTracker::DirtyNormals |
           HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyPrimID |
           HdChangeTracker::DirtyInstanceIndex | HdChangeTracker::DirtyCategories;
}

void HdNovaBasisCurves::_InitRepr(TfToken const &reprToken, HdDirtyBits *) {
    auto it = std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken));
    if (it == _reprs.end())
        _reprs.emplace_back(reprToken, std::make_shared<HdRepr>());
}

bool HdNovaBasisCurves::_Build(HdSceneDelegate *sd, HdNovaShutter const &shutter, unsigned maxSteps,
                               HdNovaObjectDefaults const &defaults, nova::MeshDesc &desc) const {
    const SdfPath &id = GetId();
    const HdBasisCurvesTopology topology = sd->GetBasisCurvesTopology(id);
    const nova::CurveBasis basis = basisOf(topology);
    CurveLayout layout = layoutOf(topology, basis);
    // The points the curves need: one per curve vertex, in order.
    size_t authored = 0;
    for (int c : topology.GetCurveVertexCounts())
        authored += c > 0 ? size_t(c) : 0;
    // Indexed curves (Hydra's curveIndices): curve vertex i is point
    // indices[i], and so are its vertex-rate primvars (widths, normals,
    // shading primvars), which stay per point; the curves need the points up
    // to the largest index. Varying and uniform values stay per curve.
    if (topology.HasIndices()) {
        const VtIntArray &indices = topology.GetCurveIndices();
        bool valid = indices.size() >= authored;
        size_t points = 0;
        for (size_t i = 0; valid && i < authored; ++i) {
            valid = indices[i] >= 0;
            points = std::max(points, size_t(std::max(indices[i], 0)) + 1);
        }
        if (!valid) {
            HD_NOVA_WARN("%s: the curve indices do not cover the curve vertex counts (or are negative)", id.GetText());
            return false;
        }
        for (uint32_t &s : layout.source)
            s = uint32_t(indices[s]);
        for (uint32_t &r : layout.reflect)
            if (r != CurveLayout::kNone)
                r = uint32_t(indices[r]);
        authored = points;
    }

    std::vector<VtVec3fArray> steps;
    if (!HdNovaSampleVec3fArray(sd, id, HdTokens->points, shutter, maxSteps, &steps) || steps.empty() ||
        steps[0].size() < authored)
        return false;
    desc = nova::MeshDesc{};
    desc.shape = nova::MeshShape::Curves;
    desc.curveBasis = basis;
    desc.curveVertexCounts = layout.counts;
    desc.pointSteps = uint32_t(steps.size());
    for (VtVec3fArray const &s : steps) {
        const std::vector<nova::Vec3f> p = expandPoints(layout, s);
        desc.points.insert(desc.points.end(), p.begin(), p.end());
    }
    const size_t n = layout.source.size();

    // Widths (diameters) as radii at the control points.
    HdInterpolation widthInterp = HdInterpolationConstant;
    std::vector<float> widths;
    int width = 0;
    bool haveWidths = false;
    for (int i = 0; i < HdInterpolationCount && !haveWidths; ++i)
        for (const HdPrimvarDescriptor &pv : sd->GetPrimvarDescriptors(id, HdInterpolation(i)))
            if (pv.name == HdTokens->widths) {
                widthInterp = HdInterpolation(i);
                haveWidths = HdNovaPrimvarToFloats(sd->Get(id, HdTokens->widths), &width, &widths) && width == 1 &&
                             !widths.empty();
                break;
            }
    desc.curveRadius.assign(n, 0.5f); // USD gives no fallback; Hydra's renderers draw width 1
    if (haveWidths) {
        switch (widthInterp) {
        case HdInterpolationConstant:
            desc.curveRadius.assign(n, 0.5f * widths[0]);
            break;
        case HdInterpolationUniform: {
            size_t i = 0;
            for (size_t c = 0; c < layout.counts.size() && c < widths.size(); ++c)
                for (uint32_t k = 0; k < layout.counts[c]; ++k)
                    desc.curveRadius[i++] = 0.5f * widths[c];
            break;
        }
        case HdInterpolationVarying: {
            // Segment ends onto control points: segment j starts near point
            // j + 1 of a B-spline or Catmull-Rom curve, j * 3 of a Bezier.
            const std::vector<float> v =
                expandVarying(topology, layout, basis, widths.data(), widths.size(), 1);
            size_t i = 0, o = 0;
            for (size_t c = 0; c < layout.counts.size(); ++c) {
                const uint32_t count = layout.counts[c];
                const uint32_t segments = nova::curveSegmentCount(basis, count);
                for (uint32_t k = 0; k < count; ++k) {
                    int j = basis == nova::CurveBasis::Bezier ? int(k + 1) / 3
                            : basis == nova::CurveBasis::Linear ? int(k)
                                                                : int(k) - 1;
                    j = std::clamp(j, 0, int(segments));
                    desc.curveRadius[i++] = segments > 0 && o + size_t(j) < v.size() ? 0.5f * v[o + size_t(j)] : 0.5f;
                }
                o += segments > 0 ? segments + 1 : 0;
            }
            break;
        }
        default: // vertex, faceVarying
            if (widths.size() >= authored) {
                desc.curveRadius = expand(layout, widths.data(), 1);
                for (float &r : desc.curveRadius)
                    r = std::max(0.5f * r, 0.0f);
            }
            break;
        }
    }

    // Karma's width scale multiplies the widths (D2).
    static const TfToken novaWidthScale("nova:object:widthScale"), karmaWidthScale("karma:object:width_scale");
    const float widthScale = std::max(
        curveSetting(sd, id, novaWidthScale, karmaWidthScale, defaults.widthScale >= 0.0f ? defaults.widthScale : 1.0f),
        0.0f);
    if (widthScale != 1.0f)
        for (float &r : desc.curveRadius)
            r *= widthScale;

    // Style (D2, Karma's karma:object:curve_style): 0 camera-facing ribbons,
    // 1 round tubes (the default: normals are ignored), 2 ribbons oriented
    // to the normals, 3 USD's rule (oriented where normals are authored,
    // else tubes). Linear curves cannot be oriented: camera-facing.
    const int style = curveStyleOf(sd, id, defaults.curveStyle >= 0 ? defaults.curveStyle : 1);
    desc.curveStyle = style == 0 ? nova::CurveStyle::Flat : nova::CurveStyle::Round;
    if (style == 2 || style == 3) {
        bool oriented = false;
        for (int i = 0; i < HdInterpolationCount && !oriented; ++i)
            for (const HdPrimvarDescriptor &pv : sd->GetPrimvarDescriptors(id, HdInterpolation(i)))
                if (pv.name == HdTokens->normals && (i == HdInterpolationVertex || i == HdInterpolationVarying)) {
                    const VtValue v = sd->Get(id, HdTokens->normals);
                    if (v.IsHolding<VtVec3fArray>() && v.UncheckedGet<VtVec3fArray>().size() >= authored) {
                        desc.normals = expandPoints(layout, v.UncheckedGet<VtVec3fArray>());
                        oriented = true;
                    }
                    break;
                }
        if (oriented)
            desc.curveStyle = basis == nova::CurveBasis::Linear ? nova::CurveStyle::Flat : nova::CurveStyle::Oriented;
        if (desc.curveStyle != nova::CurveStyle::Oriented)
            desc.normals.clear();
    }

    // Shading primvars at their curve rates.
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
            switch (interp) {
            case HdInterpolationConstant:
                a.rate = nova::AttributeRate::Constant;
                data.resize(size_t(w));
                a.data = std::move(data);
                break;
            case HdInterpolationUniform:
                a.rate = nova::AttributeRate::Uniform;
                a.data = std::move(data);
                break;
            case HdInterpolationVarying:
                a.rate = nova::AttributeRate::Varying;
                a.data = expandVarying(topology, layout, basis, data.data(), data.size() / size_t(w), size_t(w));
                break;
            default: // vertex, faceVarying
                if (data.size() < authored * size_t(w))
                    continue;
                a.rate = nova::AttributeRate::Vertex;
                a.data = expand(layout, data.data(), size_t(w));
                break;
            }
            desc.setAttribute(std::move(a));
        }
    }
    if (!nova::prepareCurves(desc) || desc.curveSegments.empty()) {
        HD_NOVA_WARN("%s: curves without a renderable segment", id.GetText());
        return false;
    }
    return true;
}

void HdNovaBasisCurves::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits,
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
        _meshId = scene.createMesh(nova::objectKey(id.GetString())); // stable, not the sync order
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
        HdNovaWarnPortalPrim(sd, id, "curves"); // light portals are meshes only (M11 P9b)
        scene.setMeshLpeTag(_meshId, HdNovaObjectLpeTag(sd, id));
        scene.setMeshVisibility(_meshId, HdNovaObjectVisibility(sd, id)); // M11 P2
        scene.setMeshOutline(_meshId, HdNovaObjectOutline(sd, id));       // M11 P9a
    }
    // Cryptomatte ids (M6 D10); per-instance names follow the instance indices.
    if (rebuild || (bits & (HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex))) {
        HdNovaCryptoNames &names = param->GetCryptoNames();
        scene.setMeshCrypto(_meshId, names.Note("__name", HdNovaCryptoObjectName(sd, id, GetInstancerId())),
                            HdNovaCryptoAttributes(sd, id, names, GetInstancerId()));
    }
    if (categoriesDirty)
        HdNovaSyncCategories(sd, id, GetInstancerId(), scene, _meshId); // light linking (M9 D15)
    if (materialsDirty || rebuild) { // a displayOpacity primvar that came or went rebuilds
        nova::MaterialId material = registry.Acquire(GetMaterialId(), scene);
        // Without a material but with a displayOpacity: see-through by it, as in Karma.
        if (material == nova::kDefaultMaterialId && HdNovaHasDisplayOpacity(sd, id))
            material = registry.AcquireOpacityDefault(scene);
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

void HdNovaBasisCurves::Finalize(HdRenderParam *renderParam) {
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    param->GetMaterials().BindMesh(GetId(), {});
    if (_meshId != nova::kInvalidId) {
        param->AcquireSceneForEdit().removeMesh(_meshId);
        _meshId = nova::kInvalidId;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
