#include "sphere.h"

#include "diagnostics.h"
#include "instancer.h"
#include "primvarUtil.h"
#include "renderParam.h"

#include "core/geometry/dicing.h"
#include "core/geometry/displace.h"
#include "core/geometry/subdiv.h"

#include <pxr/base/tf/debug.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sphereSchema.h>
#include <pxr/imaging/hd/tokens.h>

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

// The sphere's radius at uniform shutter steps (one value if it does not
// change within the shutter).
std::vector<float> sampleRadius(HdSceneDelegate *sd, SdfPath const &id, HdNovaShutter const &shutter,
                                unsigned maxSteps) {
    const HdSceneIndexBaseRefPtr si = sd->GetRenderIndex().GetTerminalSceneIndex();
    HdDoubleDataSourceHandle ds;
    if (si)
        ds = HdSphereSchema::GetFromParent(si->GetPrim(id).dataSource).GetRadius();
    if (!ds)
        return {1.0f}; // UsdGeomSphere's fallback
    std::vector<HdSampledDataSource::Time> times;
    if (shutter.IsBlurred() && ds->GetContributingSampleTimesForInterval(shutter.open, shutter.close, &times) &&
        times.size() > 1) {
        const unsigned n = std::clamp(unsigned(times.size()), 2u, std::max(maxSteps, 2u));
        std::vector<float> radii(n);
        for (unsigned k = 0; k < n; ++k)
            radii[k] = float(ds->GetTypedValue(shutter.TimeAt(float(k) / float(n - 1))));
        if (std::any_of(radii.begin(), radii.end(), [&](float r) { return r != radii[0]; }))
            return radii;
    }
    return {float(ds->GetTypedValue(shutter.FrameOffset()))};
}

// Constant and uniform primvars (a sphere has one face).
void buildAttributes(HdSceneDelegate *sd, SdfPath const &id, nova::MeshDesc &desc) {
    for (const HdInterpolation interp : {HdInterpolationConstant, HdInterpolationUniform}) {
        for (const HdPrimvarDescriptor &pv : sd->GetPrimvarDescriptors(id, interp)) {
            if (HdNovaIsGeometryPrimvar(pv.name))
                continue;
            int width = 0;
            std::vector<float> data;
            if (!HdNovaPrimvarToFloats(sd->Get(id, pv.name), &width, &data) || data.size() < size_t(width) ||
                width < 1 || width > 4)
                continue;
            data.resize(size_t(width));
            desc.setAttribute({pv.name.GetString(), nova::AttributeRate::Constant, uint8_t(width), std::move(data)});
        }
    }
}

// The sphere as a displaced mesh (M4.6): a unit cube cage diced at `rate`
// segments per edge, projected onto the sphere (radius per motion step),
// st as the analytic sphere's per triangle corner (seams unwrapped), then
// displaced along the radial normals.
bool buildDisplacedSphere(std::vector<float> const &radii, int rate, nova::DisplacementShader const &shader,
                          nova::DisplacementContext const &ctx, nova::MeshDesc &desc) {
    nova::SubdivMeshDesc cube;
    cube.scheme = nova::SubdivScheme::Bilinear;
    cube.points = {{-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1}, {-1, 1, -1}, {1, 1, -1}, {-1, -1, -1}, {1, -1, -1}};
    cube.faceVertexCounts = {4, 4, 4, 4, 4, 4};
    cube.faceVertexIndices = {0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4};
    nova::MeshDesc unit;
    if (!nova::tessellateSubdiv(cube, std::clamp(rate, 1, 1024), unit))
        return false;
    const size_t np = unit.pointCount(), nt = unit.triangleCount();
    std::vector<nova::Vec3f> dirs(np);
    for (size_t i = 0; i < np; ++i)
        dirs[i] = nova::normalize(unit.points[i]);
    const uint32_t steps = uint32_t(radii.size());
    desc = nova::MeshDesc{};
    desc.pointSteps = steps;
    desc.indices = unit.indices;
    desc.points.resize(np * steps);
    desc.normals.resize(np * steps);
    for (uint32_t s = 0; s < steps; ++s)
        for (size_t i = 0; i < np; ++i) {
            desc.points[s * np + i] = dirs[i] * radii[s];
            desc.normals[s * np + i] = dirs[i];
        }
    // st: longitude around +Y (from +X towards -Z), latitude from the bottom pole.
    nova::MeshAttribute st;
    st.name = "st";
    st.rate = nova::AttributeRate::Corner;
    st.width = 2;
    st.data.resize(nt * 6);
    for (size_t t = 0; t < nt; ++t) {
        float u[3], v[3];
        bool pole[3];
        for (int c = 0; c < 3; ++c) {
            const nova::Vec3f d = dirs[desc.indices[t * 3 + size_t(c)]];
            pole[c] = d.x * d.x + d.z * d.z < 1e-10f;
            const float a = std::atan2(-d.z, d.x) * (0.5f / nova::kPi);
            u[c] = a < 0.0f ? a + 1.0f : a;
            v[c] = std::acos(std::clamp(-d.y, -1.0f, 1.0f)) / nova::kPi;
        }
        // Unwrap the seam, then give poles the mean longitude of the others.
        const float hi = std::max({pole[0] ? 0.0f : u[0], pole[1] ? 0.0f : u[1], pole[2] ? 0.0f : u[2]});
        for (int c = 0; c < 3; ++c)
            if (!pole[c] && hi - u[c] > 0.5f)
                u[c] += 1.0f;
        float sum = 0.0f;
        int n = 0;
        for (int c = 0; c < 3; ++c)
            if (!pole[c]) {
                sum += u[c];
                ++n;
            }
        for (int c = 0; c < 3; ++c) {
            if (pole[c])
                u[c] = n ? sum / float(n) : 0.0f;
            st.data[t * 6 + size_t(c) * 2] = u[c];
            st.data[t * 6 + size_t(c) * 2 + 1] = v[c];
        }
    }
    desc.attributes.push_back(std::move(st));
    nova::DisplacementInput input;
    input.shaders = {&shader};
    input.triangleShader.assign(nt, 0);
    input.directions = dirs;
    nova::displaceMesh(desc, input, ctx);
    return true;
}

} // namespace

HdNovaSphere::HdNovaSphere(SdfPath const &id) : HdRprim(id) {}

HdDirtyBits HdNovaSphere::GetInitialDirtyBitsMask() const {
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyInstancer |
           HdChangeTracker::DirtyPrimID | HdChangeTracker::DirtyInstanceIndex | HdChangeTracker::DirtyExtent |
           HdChangeTracker::DirtyCategories;
}

TfTokenVector const &HdNovaSphere::GetBuiltinPrimvarNames() const {
    static const TfTokenVector names;
    return names;
}

void HdNovaSphere::_InitRepr(TfToken const &reprToken, HdDirtyBits *) {
    auto it = std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken));
    if (it == _reprs.end())
        _reprs.emplace_back(reprToken, std::make_shared<HdRepr>());
}

void HdNovaSphere::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits, TfToken const &) {
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

    // A medium bounded by the sphere (docs/design/M11-P5.md): the Fog Box's
    // sphere shape. The radius at the shutter's middle (transform motion is
    // the volume's).
    if (HdNovaIsMedium(sd, id, GetMaterialId(), param->GetMaterials())) {
        if (_meshId != nova::kInvalidId) {
            param->AcquireSceneForEdit().removeMesh(_meshId);
            _meshId = nova::kInvalidId;
        }
        param->ForgetDisplacedMesh(id);
        param->GetMaterials().BindMesh(id, {GetMaterialId()});
        // Instanced: one volume per instance (HdNovaMedium).
        const bool fresh = !_medium.Active();
        nova::MediumShellPtr shell;
        if (fresh || newShutter || (bits & (HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
                                             HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyExtent))) {
            const std::vector<float> radii = sampleRadius(sd, id, shutter, maxSteps);
            shell = nova::makeSphereShell(nova::Vec3f{0.0f, 0.0f, 0.0f}, radii[(radii.size() - 1) / 2]);
        }
        _medium.Sync(sd, *param, id, GetMaterialId(), IsVisible(), GetPrimId(), shell,
                     fresh || newShutter ||
                         (bits & (HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyInstancer |
                                  HdChangeTracker::DirtyInstanceIndex)),
                     shutter, GetInstancerId());
        _sampledShutter = shutter;
        _sampledMaxSteps = maxSteps;
        *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
        return;
    }
    if (_medium.Active())
        _medium.Remove(*param);
    const bool geometryDirty =
        first || newShutter ||
        (bits & (HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyPrimvar |
                 HdChangeTracker::DirtyExtent));
    const bool instancesDirty =
        first || newShutter ||
        (bits & (HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex));
    const bool visibilityDirty = bits & HdChangeTracker::DirtyVisibility;
    const bool primIdDirty = bits & HdChangeTracker::DirtyPrimID;
    const bool materialsDirty = first || (bits & HdChangeTracker::DirtyMaterialId);
    const bool categoriesDirty = first || (bits & (HdChangeTracker::DirtyCategories | HdChangeTracker::DirtyInstancer |
                                                   HdChangeTracker::DirtyInstanceIndex));
    // Displacement (M4.6): a displacing material dices the sphere once the
    // dicing view is known.
    HdNovaMaterialRegistry &registry = param->GetMaterials();
    const HdNovaDisplacementPtr displacement =
        GetMaterialId().IsEmpty() ? nullptr : registry.GetDisplacement(GetMaterialId());
    const nova::DicingView view = param->GetDicingView();
    const bool waiting = displacement && !view.valid;
    const bool readsTime = displacement && HdNovaDisplacementReadsTime(*displacement);
    uint64_t signature = 0;
    if (displacement && !waiting) {
        float q[2] = {param->GetDicingQualityScale(), param->GetOffscreenQuality()};
        uint32_t bits32[2];
        std::memcpy(bits32, q, sizeof(q));
        signature = ((displacement->hash * 1099511628211ull) ^ bits32[0] ^ (uint64_t(bits32[1]) << 32)) | 1;
        // The Time and Frame globals are evaluated at the build.
        if (readsTime) {
            const float t[2] = {param->GetFrame(), param->GetFps()};
            std::memcpy(bits32, t, sizeof(t));
            signature = ((signature * 1099511628211ull) ^ bits32[0] ^ (uint64_t(bits32[1]) << 32)) | 1;
        }
    }
    const bool displacementChanged = signature != _builtDisplacement;
    const bool rebuild = geometryDirty || displacementChanged;
    if (materialsDirty)
        registry.BindMesh(id, {GetMaterialId()});
    if (displacement)
        param->NoteDisplacedMesh(id, waiting, readsTime);
    else
        param->ForgetDisplacedMesh(id);
    if (!(rebuild || instancesDirty || visibilityDirty || primIdDirty || materialsDirty || categoriesDirty)) {
        *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
        return;
    }

    // Hydra-side work first, while the render keeps running.
    nova::MeshDesc desc;
    if (rebuild) {
        const std::vector<float> radii = sampleRadius(sd, id, shutter, maxSteps);
        const GfMatrix4d xf = sd->GetTransform(id);
        bool displaced = false;
        if (signature) {
            // Segments per cube edge: a quarter circle of the projected radius.
            const GfVec3d c = xf.Transform(GfVec3d(0.0));
            const double scale = std::max({xf.GetRow3(0).GetLength(), xf.GetRow3(1).GetLength(), xf.GetRow3(2).GetLength()});
            const float r = float(radii[0] * scale);
            const nova::Vec3f center{float(c[0]), float(c[1]), float(c[2])};
            const nova::Vec3f side = nova::normalize(nova::cross(view.forward, nova::Vec3f{0.0f, 1.0f, 0.0f})) * r;
            const nova::Vec3f across = nova::length(side) > 0.0f ? side : nova::Vec3f{r, 0.0f, 0.0f};
            float quality = 0.5f * param->GetDicingQualityScale();
            if (!nova::boxInView(view, center - nova::Vec3f{r, r, r}, center + nova::Vec3f{r, r, r}))
                quality *= param->GetOffscreenQuality();
            const float pixels = nova::edgePixels(view, center - across, center + across) * (nova::kPi / 4.0f);
            int rate = nova::dicingRate(pixels, quality);
            const uint64_t budget = param->GetSubdivMaxTriangles();
            if (uint64_t(rate) * uint64_t(rate) * 12 > budget) {
                rate = std::max(1, int(std::sqrt(double(budget) / 12.0)));
                HD_NOVA_WARN("%s: displacement dicing exceeds nova:subdivMaxTriangles; dicing coarser", id.GetText());
            }
            nova::DisplacementContext ctx;
            ctx.textures = &param->GetSurfaceTextures();
            ctx.objectToWorld = toNova(xf);
            ctx.frame = param->GetFrame();
            ctx.fps = param->GetFps();
            displaced = buildDisplacedSphere(radii, rate, displacement->shader, ctx, desc);
            if (displaced) // constant and uniform primvars on top of st
                buildAttributes(sd, id, desc);
        }
        if (!displaced) {
            desc = nova::MeshDesc{};
            desc.shape = nova::MeshShape::Sphere;
            desc.sphereRadius = radii;
            desc.points.assign(desc.sphereRadius.size(), nova::Vec3f{0.0f, 0.0f, 0.0f});
            desc.pointSteps = uint32_t(desc.points.size()); // one center per step
            buildAttributes(sd, id, desc);
        }
        _builtDisplacement = displaced ? signature : 0;
        desc.transform = toNova(xf);
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
        scene.updateMesh(_meshId, std::move(desc));
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
        // Per-object ray limits and shading properties (Nova's, then Karma's
        // constant primvars; docs/design/M11-P1.md).
        scene.setMeshRayLimits(_meshId, HdNovaObjectOverrides(sd, id));
        HdNovaWarnPortalPrim(sd, id, "spheres"); // light portals are meshes only (M11 P9b)
        scene.setMeshLpeTag(_meshId, HdNovaObjectLpeTag(sd, id));
        scene.setMeshVisibility(_meshId, HdNovaObjectVisibility(sd, id)); // M11 P2
        scene.setMeshOutline(_meshId, HdNovaObjectOutline(sd, id));       // M11 P9a
    }
    // Cryptomatte ids (D10); per-instance names follow the instance indices.
    if (rebuild || (bits & (HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex))) {
        HdNovaCryptoNames &names = param->GetCryptoNames();
        scene.setMeshCrypto(_meshId, names.Note("__name", HdNovaCryptoObjectName(sd, id, GetInstancerId())),
                            HdNovaCryptoAttributes(sd, id, names, GetInstancerId()));
    }
    if (categoriesDirty)
        HdNovaSyncCategories(sd, id, GetInstancerId(), scene, _meshId); // light linking (M9 D15)
    if (materialsDirty || rebuild) { // a displayOpacity primvar that came or went rebuilds
        nova::MaterialId material = param->GetMaterials().Acquire(GetMaterialId(), scene);
        // Without a material but with a displayOpacity: see-through by it, as in Karma.
        if (material == nova::kDefaultMaterialId && HdNovaHasDisplayOpacity(sd, id))
            material = param->GetMaterials().AcquireOpacityDefault(scene);
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

void HdNovaSphere::Finalize(HdRenderParam *renderParam) {
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    param->GetMaterials().BindMesh(GetId(), {});
    param->ForgetDisplacedMesh(GetId());
    if (_meshId != nova::kInvalidId) {
        param->AcquireSceneForEdit().removeMesh(_meshId);
        _meshId = nova::kInvalidId;
    }
    _medium.Remove(*param);
}

PXR_NAMESPACE_CLOSE_SCOPE
