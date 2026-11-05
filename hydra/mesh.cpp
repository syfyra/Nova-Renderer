#include "mesh.h"

#include "diagnostics.h"
#include "instancer.h"
#include "meshSubdiv.h"
#include "primvarUtil.h"
#include "renderParam.h"

#include "core/geometry/dicing.h"
#include "core/geometry/displace.h"
#include "core/geometry/subdiv.h"

#include <pxr/base/arch/hash.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/debug.h>
#include <pxr/base/work/loops.h>
#include <pxr/imaging/hd/capsuleSchema.h>
#include <pxr/imaging/hd/coneSchema.h>
#include <pxr/imaging/hd/cylinderSchema.h>
#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/smoothNormals.h>
#include <pxr/imaging/hd/sphereSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/vertexAdjacency.h>
#include <pxr/imaging/pxOsd/tokens.h>

#include <algorithm>
#include <cmath>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfDebug) {
    TF_DEBUG_ENVIRONMENT_SYMBOL(HDNOVA_SUBDIV, "Nova: subdivision level chosen for each mesh");
    TF_DEBUG_ENVIRONMENT_SYMBOL(HDNOVA_MOTION, "Nova: shutter and motion steps of the camera and the meshes");
    TF_DEBUG_ENVIRONMENT_SYMBOL(HDNOVA_MATERIAL, "Nova: MaterialX libraries, material networks and translation");
    TF_DEBUG_ENVIRONMENT_SYMBOL(HDNOVA_AOV, "Nova: the AOV bindings the host asks for and what they resolve to");
    TF_DEBUG_ENVIRONMENT_SYMBOL(HDNOVA_VOLUME, "Nova: volume fields loaded and volumes built");
    TF_DEBUG_ENVIRONMENT_SYMBOL(HDNOVA_SSS, "Nova: subsurface walks run and killed at the segment cap");
}

TF_DEFINE_PRIVATE_TOKENS(_meshTokens, ((subdivLevel, "nova:subdivLevel"))((diffuseLimit, "nova:object:diffuseLimit"))
                         ((reflectLimit, "nova:object:reflectLimit"))((refractLimit, "nova:object:refractLimit"))
                         ((sssLimit, "nova:object:sssLimit"))((volumeLimit, "nova:object:volumeLimit"))
                         ((thinWallAtRefractLimit, "nova:object:thinWallAtRefractLimit"))
                         ((fakeCaustics, "nova:object:fakeCaustics"))((dicingQuality, "nova:dicingQuality"))
                         ((dicingDepth, "nova:dicingDepth"))((displacement, "nova:displacement"))
                         ((displacementDirection, "__nova:displacementDirection")));

namespace {

// HdDisplayStyle::refineLevel from husk --complexity (Houdini 22, measured):
// low 0, medium 2, high 4, veryhigh 8 (the default); a number N gives
// min(N, 8). Karma renders the control hull below 3 and the limit surface
// from 3 on; Nova switches at the same level so both renderers agree.
constexpr int kSubdivMinRefineLevel = 3;
constexpr int kMaxSubdivLevel = 6;

nova::Mat4 toNova(GfMatrix4d const &m) {
    nova::Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = m[i][j];
    return r;
}

nova::Vec3f toNova(GfVec3f const &v) { return {v[0], v[1], v[2]}; }

// Returns the interpolation of a primvar, or false if it is not authored.
bool findPrimvar(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name, HdInterpolation &interp) {
    for (int i = 0; i < HdInterpolationCount; ++i) {
        for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolation(i))) {
            if (d.name == name) {
                interp = HdInterpolation(i);
                return true;
            }
        }
    }
    return false;
}

// Meshes that HdsiImplicitSurfaceSceneIndex made from UsdGeom implicit prims
// still list their sphere/cylinder/cone/capsule schema (blocked, so only the
// name is left). Their generated catmullClark cage stands in for the
// analytic shape, and its limit surface would shrink it further (a unit
// sphere to radius 0.92..0.985), so they stay polygonal (with smooth
// normals) until Nova has analytic primitives.
bool isImplicitSurface(HdSceneDelegate *sd, SdfPath const &id) {
    const HdSceneIndexBaseRefPtr sceneIndex = sd->GetRenderIndex().GetTerminalSceneIndex();
    if (!sceneIndex)
        return false;
    const HdContainerDataSourceHandle ds = sceneIndex->GetPrim(id).dataSource;
    if (!ds)
        return false;
    for (TfToken const &name : ds->GetNames())
        if (name == HdSphereSchemaTokens->sphere || name == HdCylinderSchemaTokens->cylinder ||
            name == HdConeSchemaTokens->cone || name == HdCapsuleSchemaTokens->capsule)
            return true;
    return false;
}

// The shading primvars of a prim (every interpolation but instance).
struct PrimvarRef {
    TfToken name;
    HdInterpolation interp;
};
std::vector<PrimvarRef> shadingPrimvars(HdSceneDelegate *sd, SdfPath const &id) {
    std::vector<PrimvarRef> out;
    for (int i = 0; i < HdInterpolationCount; ++i) {
        if (HdInterpolation(i) == HdInterpolationInstance)
            continue;
        for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolation(i)))
            if (!HdNovaIsGeometryPrimvar(d.name))
                out.push_back({d.name, HdInterpolation(i)});
    }
    return out;
}

nova::PrimvarInterp toNovaInterp(HdInterpolation interp) {
    switch (interp) {
    case HdInterpolationUniform:
        return nova::PrimvarInterp::Uniform;
    case HdInterpolationVarying:
        return nova::PrimvarInterp::Varying;
    case HdInterpolationVertex:
        return nova::PrimvarInterp::Vertex;
    case HdInterpolationFaceVarying:
        return nova::PrimvarInterp::FaceVarying;
    default:
        return nova::PrimvarInterp::Constant;
    }
}

// The primvars of a subdivision surface on the control mesh. faceVarying data
// stays indexed so OpenSubdiv sees its true topology (seams).
void gatherSubdivPrimvars(HdSceneDelegate *sd, SdfPath const &id, std::vector<nova::SubdivPrimvar> &out) {
    for (const PrimvarRef &pv : shadingPrimvars(sd, id)) {
        VtIntArray indices;
        const VtValue value = pv.interp == HdInterpolationFaceVarying ? sd->GetIndexedPrimvar(id, pv.name, &indices)
                                                                      : sd->Get(id, pv.name);
        nova::SubdivPrimvar p;
        int width = 0;
        if (!HdNovaPrimvarToFloats(value, &width, &p.values) || p.values.empty())
            continue;
        p.name = pv.name.GetString();
        p.width = uint8_t(width);
        p.interp = toNovaInterp(pv.interp);
        p.indices.assign(indices.cbegin(), indices.cend());
        out.push_back(std::move(p));
    }
}


// A content hash of the sampled point and normal steps (counts and float
// bytes) that _GeometryUnchanged() compares over a blurred shutter.
uint64_t motionHash(const std::vector<VtVec3fArray> &points, const std::vector<VtVec3fArray> &normals) {
    uint64_t h = 0x6e6f7661u; // seed
    for (const std::vector<VtVec3fArray> *steps : {&points, &normals}) {
        const uint64_t n = steps->size();
        h = ArchHash64(reinterpret_cast<const char *>(&n), sizeof n, h);
        for (const VtVec3fArray &a : *steps) {
            const uint64_t count = a.size();
            h = ArchHash64(reinterpret_cast<const char *>(&count), sizeof count, h);
            if (count)
                h = ArchHash64(reinterpret_cast<const char *>(a.cdata()), size_t(count) * sizeof(GfVec3f), h);
        }
    }
    return h;
}
} // namespace

HdNovaMesh::HdNovaMesh(SdfPath const &id) : HdMesh(id) {}

HdDirtyBits HdNovaMesh::GetInitialDirtyBitsMask() const {
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyDoubleSided |
           HdChangeTracker::DirtySubdivTags | HdChangeTracker::DirtyDisplayStyle | HdChangeTracker::DirtyPrimID |
           HdChangeTracker::DirtyInstanceIndex | HdChangeTracker::DirtyCategories;
}

void HdNovaMesh::_InitRepr(TfToken const &reprToken, HdDirtyBits *) {
    // Nova does not use draw items, but Hydra expects a (non-null) repr entry.
    auto it = std::find_if(_reprs.begin(), _reprs.end(), _ReprComparator(reprToken));
    if (it == _reprs.end())
        _reprs.emplace_back(reprToken, std::make_shared<HdRepr>());
}

namespace {

// The shading primvars as attributes: constant, one per triangle (uniform),
// one per point (vertex, varying) or one per triangle corner (faceVarying).
// `deindexed`: the mesh has one point per triangle corner, so per-point data
// goes per corner. Inconsistent data is skipped.
void buildAttributes(HdSceneDelegate *sd, SdfPath const &id, HdMeshUtil const &meshUtil,
                     VtVec3iArray const &triangles, VtIntArray const &primitiveParams, size_t pointCount,
                     bool deindexed, nova::MeshDesc &desc) {
    const size_t numTris = triangles.size();
    for (const PrimvarRef &pv : shadingPrimvars(sd, id)) {
        int width = 0;
        std::vector<float> data;
        if (!HdNovaPrimvarToFloats(sd->Get(id, pv.name), &width, &data) || data.empty())
            continue;
        const size_t w = size_t(width), count = data.size() / w;
        nova::MeshAttribute a;
        a.name = pv.name.GetString();
        a.width = uint8_t(width);
        bool valid = true;
        switch (pv.interp) {
        case HdInterpolationUniform: // one per authored face
            a.rate = nova::AttributeRate::Uniform;
            a.data.resize(numTris * w);
            for (size_t t = 0; t < numTris && valid; ++t) {
                const int face = t < primitiveParams.size()
                                     ? HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(primitiveParams[t])
                                     : -1;
                valid = face >= 0 && size_t(face) < count;
                if (valid)
                    std::copy_n(&data[size_t(face) * w], w, &a.data[t * w]);
            }
            break;
        case HdInterpolationVertex:
        case HdInterpolationVarying: // one per point
            if (count < pointCount) {
                valid = false;
            } else if (!deindexed) {
                a.rate = nova::AttributeRate::Vertex;
                data.resize(pointCount * w);
                a.data = std::move(data);
            } else {
                a.rate = nova::AttributeRate::Corner;
                a.data.resize(numTris * 3 * w);
                for (size_t t = 0; t < numTris && valid; ++t)
                    for (int c = 0; c < 3 && valid; ++c) {
                        const int pi = triangles[t][c];
                        valid = pi >= 0 && size_t(pi) < count;
                        if (valid)
                            std::copy_n(&data[size_t(pi) * w], w, &a.data[(t * 3 + size_t(c)) * w]);
                    }
            }
            break;
        case HdInterpolationFaceVarying: { // one per face-vertex
            static const HdType types[4] = {HdTypeFloat, HdTypeFloatVec2, HdTypeFloatVec3, HdTypeFloatVec4};
            VtValue triangulated;
            const HdMeshComputationResult result =
                meshUtil.ComputeTriangulatedFaceVaryingPrimvar(data.data(), int(count), types[w - 1], &triangulated);
            std::vector<float> corners;
            int cw = 0;
            if (result == HdMeshComputationResult::Error)
                valid = false;
            else if (result == HdMeshComputationResult::Success)
                valid = HdNovaPrimvarToFloats(triangulated, &cw, &corners) && size_t(cw) == w;
            else // unchanged: already one per triangle corner
                corners = std::move(data);
            valid = valid && corners.size() >= numTris * 3 * w;
            if (valid) {
                corners.resize(numTris * 3 * w);
                a.rate = nova::AttributeRate::Corner;
                a.data = std::move(corners);
            }
            break;
        }
        default: // constant
            a.rate = nova::AttributeRate::Constant;
            data.resize(w);
            a.data = std::move(data);
            break;
        }
        if (valid)
            desc.attributes.push_back(std::move(a));
    }
}

} // namespace

// The instances of an instanced mesh: static transforms in `instances`, or,
// if an instancer level or the prototype moves, `steps` uniform SRT keys per
// instance over the shutter in `keys` (step-major).
void HdNovaSyncCategories(HdSceneDelegate *sd, SdfPath const &id, SdfPath const &instancerId, nova::Scene &scene,
                          nova::MeshId mesh) {
    std::vector<std::string> categories;
    for (TfToken const &t : sd->GetCategories(id))
        categories.push_back(t.GetString());
    std::vector<std::vector<std::string>> perInstance;
    if (!instancerId.IsEmpty()) {
        for (TfToken const &t : sd->GetCategories(instancerId))
            categories.push_back(t.GetString());
        // One instancer level: the flattened instances follow the prototype's
        // instance indices (nested instancers keep the prim's categories only).
        const std::vector<VtArray<TfToken>> instances = sd->GetInstanceCategories(instancerId);
        if (!instances.empty() && sd->GetInstancerId(instancerId).IsEmpty()) {
            const VtIntArray indices = sd->GetInstanceIndices(instancerId, id);
            perInstance.resize(indices.size());
            for (size_t k = 0; k < indices.size(); ++k)
                if (indices[k] >= 0 && size_t(indices[k]) < instances.size())
                    for (TfToken const &t : instances[size_t(indices[k])])
                        perInstance[k].push_back(t.GetString());
        }
    }
    scene.setMeshCategories(mesh, std::move(categories), std::move(perInstance));
    // Karma's legacy object side of light categories and trace sets (D18):
    // superseded by the UsdLux collections.
    static const TfToken legacy[] = {TfToken("karma:object:lightcategories"), TfToken("karma:object:tracesets")};
    for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolationConstant))
        if (d.name == legacy[0] || d.name == legacy[1]) {
            static std::once_flag warned;
            std::call_once(warned, [&] {
                TF_WARN("Nova: %s uses Karma's legacy light categories or trace sets, which Nova ignores; use light "
                        "linking (collection:lightLink / shadowLink) instead",
                        id.GetText());
            });
            break;
        }
}
void HdNovaComputeInstances(HdSceneDelegate *sd, SdfPath const &id, HdNovaInstancer const &instancer,
                      HdNovaShutter const &shutter, unsigned maxSteps, std::vector<nova::Xform34> &instances,
                      std::vector<nova::SrtTransform> &keys, uint32_t &steps) {
    HdNovaTransformSamples prototypeXf;
    HdNovaSampleTransformSamples(sd, id, shutter, &prototypeXf);
    const unsigned n = std::max(instancer.GetMotionStepCount(maxSteps), prototypeXf.StepCount(shutter, maxSteps));
    steps = 1;
    if (n > 1) {
        std::vector<GfMatrix4d> xf;
        size_t count = 0;
        bool moving = false;
        for (unsigned s = 0; s < n; ++s) {
            const float t = shutter.TimeAt(float(s) / float(n - 1));
            instancer.ComputeInstanceTransformsAt(id, prototypeXf.At(t), t, xf);
            if (s == 0) {
                count = xf.size();
                keys.resize(count * n);
            } else if (xf.size() != count) {
                count = 0; // the instance count changes within the shutter
                break;
            }
            nova::SrtTransform *dst = keys.data() + size_t(s) * count;
            WorkParallelForN(count, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i)
                    dst[i] = nova::decomposeSrt(HdNovaToMat4(xf[i]));
            });
            // The decomposition is deterministic, so equal transforms give
            // bit-identical keys.
            moving = moving || std::memcmp(dst, keys.data(), count * sizeof(nova::SrtTransform)) != 0;
        }
        if (count > 0 && moving) {
            steps = n;
            return;
        }
        keys.clear();
    }
    instancer.ComputeInstanceTransforms(id, prototypeXf.At(shutter.FrameOffset()), instances);
}

int HdNovaMesh::_ResolveSubdivLevel(HdNovaRenderParam const &param) const {
    const TfToken &scheme = _topology.GetScheme();
    // "none" is a polygon mesh; bilinear subdivision of planar faces is the
    // polygon itself (revisit with displacement in M3).
    if (scheme != PxOsdOpenSubdivTokens->catmullClark && scheme != PxOsdOpenSubdivTokens->loop)
        return 0;
    if (_implicitSurface || _refineLevel < kSubdivMinRefineLevel)
        return 0;
    int level = _primSubdivLevel >= 0 ? _primSubdivLevel : param.GetSubdivLevel();
    if (level < 0) // automatic: hdArnold's mapping of refineLevel to iterations (high 4 -> 2, veryhigh 8 -> 3)
        level = int(std::floor(std::log2(double(_refineLevel))));
    return std::clamp(level, 0, kMaxSubdivLevel);
}

int HdNovaMesh::_ResolveSubdivRate(int subdivLevel, uint64_t budget) const {
    if (subdivLevel <= 0)
        return 0;
    const TfToken &scheme = _topology.GetScheme();
    return nova::chooseSubdivRate(scheme == PxOsdOpenSubdivTokens->loop ? nova::SubdivScheme::Loop
                                                                         : nova::SubdivScheme::CatmullClark,
                                  _topology.GetFaceVertexCounts().size(), _topology.GetFaceVertexIndices().size(),
                                  subdivLevel, budget);
}

bool HdNovaMesh::_BuildSubdivMesh(HdSceneDelegate *sd, std::vector<VtVec3fArray> const &pointSteps, int subdivLevel,
                                  int rate, nova::MeshDesc &desc) {
    const SdfPath &id = GetId();
    // The budget (nova:subdivMaxTriangles) lowers the level, down to the
    // control mesh if even level 1 exceeds it.
    if (rate == 0) {
        HD_NOVA_WARN("%s exceeds nova:subdivMaxTriangles even at subdivision level 1, rendering its control mesh",
                     id.GetText());
        return false;
    }
    if (rate != (1 << subdivLevel))
        HD_NOVA_WARN("%s exceeds nova:subdivMaxTriangles at subdivision level %d, using level %d", id.GetText(),
                     subdivLevel, int(std::log2(double(rate))));

    nova::SubdivMeshDesc sdesc;
    if (!HdNovaToSubdivDesc(_topology, _subdivTags, pointSteps, id, &sdesc))
        return false;
    gatherSubdivPrimvars(sd, id, sdesc.primvars);

    // Authored normals are ignored on subdivision surfaces (UsdGeomMesh, Karma).
    std::string error;
    nova::SubdivStats stats;
    if (!nova::tessellateSubdiv(sdesc, rate, desc, &error, &stats, &_triangleFaces)) {
        HD_NOVA_WARN("cannot subdivide %s (%s), rendering its control mesh", id.GetText(), error.c_str());
        desc = nova::MeshDesc{};
        return false;
    }
    if (stats.droppedFaces > 0)
        HD_NOVA_WARN("%s: ignoring %u face(s) with fewer than 3 vertices", id.GetText(), stats.droppedFaces);
    return true;
}

bool HdNovaMesh::_BuildMesh(HdSceneDelegate *sd, HdNovaRenderParam &param, int subdivLevel, int subdivRate,
                            std::vector<HdNovaDisplacementPtr> const *displacements,
                            std::vector<uint16_t> const &faceSlots, nova::MeshDesc &desc) {
    const SdfPath &id = GetId();
    _srcPoints = sd->Get(id, HdTokens->points);
    _srcNormals = sd->Get(id, HdTokens->normals);
    _srcPrimvars.clear();
    for (const PrimvarRef &pv : shadingPrimvars(sd, id))
        _srcPrimvars.emplace_back(pv.name, sd->Get(id, pv.name));

    // Positions at uniform shutter steps (one step if they do not move).
    std::vector<VtVec3fArray> pointSteps;
    const bool sampled = HdNovaSampleVec3fArray(sd, id, HdTokens->points, param.GetShutter(),
                                                param.GetMaxMotionSteps(), &pointSteps);
    // Over a blurred shutter _GeometryUnchanged() also compares the motion.
    _srcMotionHash = 0;
    if (param.GetShutter().IsBlurred()) {
        std::vector<VtVec3fArray> normalSteps;
        HdNovaSampleVec3fArray(sd, id, HdTokens->normals, param.GetShutter(), param.GetMaxMotionSteps(),
                               &normalSteps);
        _srcMotionHash = motionHash(pointSteps, normalSteps);
    }
    if (!sampled || pointSteps.empty())
        return false;
    const size_t pointCount = pointSteps[0].size();
    const uint32_t steps = uint32_t(pointSteps.size());

    // Inconsistent data (e.g. points and indices from different time samples)
    // would make the topology helpers read past the points array.
    if (_topology.GetNumPoints() > int(pointCount)) {
        HD_NOVA_WARN("mesh %s references %d points but has %zu, skipping it", id.GetText(),
                _topology.GetNumPoints(), pointCount);
        return false;
    }

    // Displaced meshes are diced from the control mesh (subdivision or not).
    if (displacements && _BuildDisplacedMesh(sd, param, pointSteps, *displacements, faceSlots, desc))
        return true;
    // Subdivision surfaces: the limit surface for every motion step; the
    // control mesh below is the hull and the fallback.
    if (subdivLevel > 0 && _BuildSubdivMesh(sd, pointSteps, subdivLevel, subdivRate, desc))
        return true;
    const HdNovaShutter shutter = param.GetShutter();
    const unsigned maxSteps = param.GetMaxMotionSteps();

    // Triangulation follows the topology's orientation, so the triangle
    // winding is consistent for left- and right-handed meshes.
    HdMeshUtil meshUtil(&_topology, id);
    VtVec3iArray triangles;
    VtIntArray primitiveParams;
    meshUtil.ComputeTriangleIndices(&triangles, &primitiveParams);
    if (triangles.empty())
        return false;
    _triangleFaces.resize(triangles.size());
    for (size_t t = 0; t < triangles.size(); ++t)
        _triangleFaces[t] =
            t < primitiveParams.size() ? HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(primitiveParams[t]) : -1;

    // Authored normals: sampled like the points, or one set for all steps.
    HdInterpolation nInterp = HdInterpolationConstant;
    const bool hasNormals = findPrimvar(sd, id, HdTokens->normals, nInterp);
    std::vector<VtVec3fArray> normalSteps;
    if (hasNormals && HdNovaSampleVec3fArray(sd, id, HdTokens->normals, shutter, maxSteps, &normalSteps) &&
        normalSteps.size() != steps)
        normalSteps.resize(1);
    desc.pointSteps = steps;

    if (hasNormals && nInterp == HdInterpolationFaceVarying && !normalSteps.empty() && !normalSteps[0].empty()) {
        // Houdini vertex normals arrive as faceVarying: de-index the mesh so
        // every triangle corner carries its own normal.
        std::vector<VtVec3fArray> cornerNormals;
        for (VtVec3fArray const &normals : normalSteps) {
            VtValue triangulated;
            const HdMeshComputationResult result = meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                normals.cdata(), int(normals.size()), HdTypeFloatVec3, &triangulated);
            if (result == HdMeshComputationResult::Error)
                return false;
            // "Unchanged" means the mesh is already triangulated: the input is the result.
            if (result == HdMeshComputationResult::Success && !triangulated.IsHolding<VtVec3fArray>())
                return false;
            cornerNormals.push_back(result == HdMeshComputationResult::Success
                                        ? triangulated.UncheckedGet<VtVec3fArray>()
                                        : normals);
        }

        buildAttributes(sd, id, meshUtil, triangles, primitiveParams, pointCount, /*deindexed=*/true, desc);
        const size_t numCorners = triangles.size() * 3;
        desc.points.resize(numCorners * steps);
        desc.normals.resize(numCorners * cornerNormals.size());
        desc.indices.resize(numCorners);
        for (size_t t = 0; t < triangles.size(); ++t) {
            for (int c = 0; c < 3; ++c) {
                const size_t k = t * 3 + size_t(c);
                const int pi = triangles[t][c];
                for (uint32_t s = 0; s < steps; ++s)
                    desc.points[s * numCorners + k] =
                        (pi >= 0 && size_t(pi) < pointCount) ? toNova(pointSteps[s][pi]) : nova::Vec3f{};
                for (size_t s = 0; s < cornerNormals.size(); ++s)
                    desc.normals[s * numCorners + k] =
                        k < cornerNormals[s].size() ? toNova(cornerNormals[s][k]) : nova::Vec3f{};
                desc.indices[k] = uint32_t(k);
            }
        }
        return true;
    }

    buildAttributes(sd, id, meshUtil, triangles, primitiveParams, pointCount, /*deindexed=*/false, desc);
    desc.points.resize(pointCount * steps);
    for (uint32_t s = 0; s < steps; ++s)
        for (size_t i = 0; i < pointCount; ++i)
            desc.points[s * pointCount + i] = toNova(pointSteps[s][i]);

    desc.indices.resize(triangles.size() * 3);
    for (size_t t = 0; t < triangles.size(); ++t) {
        for (int c = 0; c < 3; ++c) {
            const int pi = triangles[t][c];
            desc.indices[t * 3 + size_t(c)] = (pi >= 0 && size_t(pi) < pointCount) ? uint32_t(pi) : 0u;
        }
    }

    const bool perVertex = nInterp == HdInterpolationVertex || nInterp == HdInterpolationVarying;
    const TfToken &scheme = _topology.GetScheme();
    if (hasNormals && perVertex && !normalSteps.empty() && normalSteps[0].size() == pointCount) {
        desc.normals.resize(pointCount * normalSteps.size());
        for (size_t s = 0; s < normalSteps.size(); ++s)
            for (size_t i = 0; i < pointCount; ++i)
                desc.normals[s * pointCount + i] = toNova(normalSteps[s][i]);
    } else if (scheme == PxOsdOpenSubdivTokens->catmullClark || scheme == PxOsdOpenSubdivTokens->loop) {
        // Smooth subdivision surfaces without authored normals get smooth
        // vertex normals (per motion step) until OpenSubdiv refinement.
        // Bilinear and "none" meshes are flat-faceted and use the geometric
        // normal.
        Hd_VertexAdjacency adjacency;
        adjacency.BuildAdjacencyTable(&_topology);
        // Unreferenced trailing points get no normal; keep one per point.
        desc.normals.assign(pointCount * steps, nova::Vec3f{0.0f, 0.0f, 1.0f});
        for (uint32_t s = 0; s < steps; ++s) {
            const VtVec3fArray smooth =
                Hd_SmoothNormals::ComputeSmoothNormals(&adjacency, int(pointCount), pointSteps[s].cdata());
            for (size_t i = 0; i < smooth.size() && i < pointCount; ++i)
                desc.normals[s * pointCount + i] = toNova(smooth[i]);
        }
    }
    return true;
}

std::vector<nova::Vec3f> HdNovaMesh::_ControlNormals(HdSceneDelegate *sd, VtVec3fArray const &points) const {
    const SdfPath &id = GetId();
    const size_t n = points.size();
    std::vector<nova::Vec3f> normals(n, nova::Vec3f{0.0f, 0.0f, 0.0f});
    VtIntArray const &counts = _topology.GetFaceVertexCounts();
    VtIntArray const &indices = _topology.GetFaceVertexIndices();
    HdInterpolation interp;
    if (findPrimvar(sd, id, HdTokens->normals, interp)) {
        const VtValue v = sd->Get(id, HdTokens->normals);
        if (v.IsHolding<VtVec3fArray>()) {
            VtVec3fArray const &authored = v.UncheckedGet<VtVec3fArray>();
            if ((interp == HdInterpolationVertex || interp == HdInterpolationVarying) && authored.size() >= n) {
                for (size_t i = 0; i < n; ++i)
                    normals[i] = toNova(authored[i]);
                return normals;
            }
            if (interp == HdInterpolationFaceVarying && authored.size() >= indices.size()) {
                for (size_t k = 0; k < indices.size(); ++k)
                    if (indices[k] >= 0 && size_t(indices[k]) < n)
                        normals[size_t(indices[k])] += nova::normalize(toNova(authored[k]));
                return normals;
            }
        }
    }
    // Angle-weighted face normals (Newell), oriented like the triangles.
    const float sign = _topology.GetOrientation() == HdTokens->leftHanded ? -1.0f : 1.0f;
    size_t o = 0;
    for (int c : counts) {
        if (c < 3 || o + size_t(c) > indices.size()) {
            o += size_t(std::max(c, 0));
            continue;
        }
        nova::Vec3f fn{0.0f, 0.0f, 0.0f};
        for (int k = 0; k < c; ++k) {
            const int a = indices[o + size_t(k)], b = indices[o + size_t((k + 1) % c)];
            if (a < 0 || b < 0 || size_t(a) >= n || size_t(b) >= n)
                continue;
            const nova::Vec3f pa = toNova(points[size_t(a)]), pb = toNova(points[size_t(b)]);
            fn += nova::Vec3f{(pa.y - pb.y) * (pa.z + pb.z), (pa.z - pb.z) * (pa.x + pb.x), (pa.x - pb.x) * (pa.y + pb.y)};
        }
        const float l = nova::length(fn);
        if (l > 0.0f) {
            fn = fn * (sign / l);
            for (int k = 0; k < c; ++k) {
                const int prev = indices[o + size_t((k + c - 1) % c)], cur = indices[o + size_t(k)],
                          next = indices[o + size_t((k + 1) % c)];
                if (prev < 0 || cur < 0 || next < 0 || size_t(prev) >= n || size_t(cur) >= n || size_t(next) >= n)
                    continue;
                const nova::Vec3f e0 = toNova(points[size_t(prev)]) - toNova(points[size_t(cur)]),
                                  e1 = toNova(points[size_t(next)]) - toNova(points[size_t(cur)]);
                const float l0 = nova::length(e0), l1 = nova::length(e1);
                if (l0 > 0.0f && l1 > 0.0f)
                    normals[size_t(cur)] += fn * std::acos(std::clamp(nova::dot(e0, e1) / (l0 * l1), -1.0f, 1.0f));
            }
        }
        o += size_t(c);
    }
    return normals;
}

bool HdNovaMesh::_BuildDisplacedMesh(HdSceneDelegate *sd, HdNovaRenderParam &param,
                                     std::vector<VtVec3fArray> const &pointSteps,
                                     std::vector<HdNovaDisplacementPtr> const &displacements,
                                     std::vector<uint16_t> const &faceSlots, nova::MeshDesc &desc) {
    const SdfPath &id = GetId();
    nova::SubdivScheme scheme = nova::SubdivScheme::Bilinear;
    const bool smooth = HdNovaGetSubdivScheme(_topology.GetScheme(), &scheme) && scheme != nova::SubdivScheme::Bilinear;
    nova::SubdivMeshDesc sdesc;
    if (smooth) {
        if (!HdNovaToSubdivDesc(_topology, _subdivTags, pointSteps, id, &sdesc))
            return false;
    } else { // polygon meshes: bilinear patches on the authored faces
        const HdMeshTopology bilinear(PxOsdOpenSubdivTokens->bilinear, _topology.GetOrientation(),
                                      _topology.GetFaceVertexCounts(), _topology.GetFaceVertexIndices(),
                                      _topology.GetHoleIndices());
        if (!HdNovaToSubdivDesc(bilinear, PxOsdSubdivTags(), pointSteps, id, &sdesc))
            return false;
    }
    gatherSubdivPrimvars(sd, id, sdesc.primvars);
    if (!smooth) {
        nova::SubdivPrimvar dir;
        dir.name = _meshTokens->displacementDirection.GetString();
        dir.interp = nova::PrimvarInterp::Varying;
        dir.width = 3;
        for (const nova::Vec3f &n : _ControlNormals(sd, pointSteps[0]))
            dir.values.insert(dir.values.end(), {n.x, n.y, n.z});
        sdesc.primvars.push_back(std::move(dir));
    }

    // Rates: ceil(pixels x quality) per control edge from the dicing view,
    // or 2^depth; meshes outside the view at the off-screen quality; within
    // the triangle budget.
    const nova::DicingView view = param.GetDicingView();
    std::vector<GfMatrix4d> xfSteps;
    HdNovaSampleTransform(sd, id, param.GetShutter(), param.GetMaxMotionSteps(), &xfSteps);
    const GfMatrix4d xf = xfSteps.empty() ? sd->GetTransform(id) : xfSteps.front(); // shutter open (Karma)
    std::vector<nova::Vec3f> world(pointSteps[0].size());
    nova::Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < world.size(); ++i) {
        world[i] = toNova(GfVec3f(xf.Transform(GfVec3d(pointSteps[0][i]))));
        lo = nova::Vec3f{std::min(lo.x, world[i].x), std::min(lo.y, world[i].y), std::min(lo.z, world[i].z)};
        hi = nova::Vec3f{std::max(hi.x, world[i].x), std::max(hi.y, world[i].y), std::max(hi.z, world[i].z)};
    }
    const int fixedRate = _dicingDepth >= 0 ? (1 << std::clamp(_dicingDepth, 0, 10)) : 0;
    float quality = std::max(_dicingQuality, 1e-3f) * param.GetDicingQualityScale();
    if (view.valid && !world.empty() && !nova::boxInView(view, lo, hi))
        quality *= param.GetOffscreenQuality();
    float fixedScale = 1.0f; // the budget's reduction of a fixed rate
    auto rateOf = [&](int32_t a, int32_t b, float q) {
        if (fixedRate)
            return std::max(1, int(float(fixedRate) * fixedScale));
        if (a < 0 || b < 0 || size_t(a) >= world.size() || size_t(b) >= world.size())
            return 1;
        return nova::dicingRate(nova::edgePixels(view, world[size_t(a)], world[size_t(b)]), q);
    };
    // Budget: about (mean edge rate)^2 x (corners / 2) triangles per face.
    double estimate = 0.0;
    size_t o = 0;
    for (int32_t c : sdesc.faceVertexCounts) {
        double sum = 0.0;
        for (int32_t k = 0; k < c; ++k)
            sum += rateOf(sdesc.faceVertexIndices[o + size_t(k)], sdesc.faceVertexIndices[o + size_t((k + 1) % c)], quality);
        if (c > 0)
            estimate += double(c) * 0.5 * (sum / c) * (sum / c);
        o += size_t(std::max(c, 0));
    }
    const double budget = double(param.GetSubdivMaxTriangles());
    if (estimate > budget) {
        const double scale = std::sqrt(budget / estimate);
        HD_NOVA_WARN("%s: displacement dicing needs about %.0f triangles, more than nova:subdivMaxTriangles; dicing "
                     "%.2fx coarser",
                     id.GetText(), estimate, 1.0 / scale);
        quality *= float(scale);
        fixedScale = float(scale);
    }
    nova::TessellateOptions options;
    options.edgeRate = [&](int32_t a, int32_t b) { return rateOf(a, b, quality); };
    options.triangleFaces = &_triangleFaces;
    std::vector<std::pair<int32_t, int32_t>> copies;
    options.copies = &copies;
    std::string error;
    nova::SubdivStats stats;
    if (!nova::tessellateSubdiv(sdesc, 1, desc, options, &error, &stats)) {
        HD_NOVA_WARN("cannot dice %s for displacement (%s), rendering it undisplaced", id.GetText(), error.c_str());
        desc = nova::MeshDesc{};
        return false;
    }

    nova::DisplacementInput input;
    if (!smooth) {
        // The interpolated control normals are the directions, not a primvar.
        for (auto it = desc.attributes.begin(); it != desc.attributes.end(); ++it)
            if (it->name == _meshTokens->displacementDirection.GetString()) {
                input.directions.resize(desc.pointCount());
                for (size_t i = 0; i < input.directions.size() && i * 3 + 2 < it->data.size(); ++i)
                    input.directions[i] = {it->data[i * 3], it->data[i * 3 + 1], it->data[i * 3 + 2]};
                desc.attributes.erase(it);
                break;
            }
    }
    input.copies = std::move(copies);
    input.triangleFaces = _triangleFaces;
    for (HdNovaDisplacementPtr const &d : displacements)
        input.shaders.push_back(d ? &d->shader : nullptr);
    input.triangleShader.resize(desc.triangleCount());
    for (size_t t = 0; t < input.triangleShader.size(); ++t) {
        const int32_t f = t < _triangleFaces.size() ? _triangleFaces[t] : -1;
        const size_t slot = f >= 0 && size_t(f) < faceSlots.size() ? faceSlots[size_t(f)] : 0;
        input.triangleShader[t] = slot < displacements.size() && displacements[slot] ? int32_t(slot) : -1;
    }
    nova::DisplacementContext ctx;
    ctx.textures = &param.GetSurfaceTextures();
    ctx.objectToWorld = toNova(xf);
    ctx.frame = param.GetFrame();
    ctx.fps = param.GetFps();
    nova::displaceMesh(desc, input, ctx);
    TF_DEBUG(HDNOVA_SUBDIV)
        .Msg("Nova: %s displaced: %u triangles (%s, quality %g)\n", id.GetText(), stats.triangles,
             fixedRate ? "fixed rate" : "dicing view", double(quality));
    return true;
}

void HdNovaMesh::_FaceSlots(std::vector<SdfPath> &slots, std::vector<uint16_t> &faceSlots) const {
    slots.assign(1, GetMaterialId());
    faceSlots.clear();
    const HdGeomSubsets &subsets = _topology.GetGeomSubsets();
    if (subsets.empty())
        return;
    std::vector<uint16_t> faceSlot(size_t(std::max(_topology.GetNumFaces(), 0)), 0);
    bool any = false;
    for (HdGeomSubset const &s : subsets) {
        if (s.type != HdGeomSubset::TypeFaceSet || s.materialId.IsEmpty())
            continue;
        const auto found = std::find(slots.begin(), slots.end(), s.materialId);
        const uint16_t slot = uint16_t(found - slots.begin());
        if (found == slots.end())
            slots.push_back(s.materialId);
        for (int f : s.indices)
            if (f >= 0 && size_t(f) < faceSlot.size()) {
                faceSlot[size_t(f)] = slot;
                any = any || slot != 0;
            }
    }
    if (any)
        faceSlots = std::move(faceSlot);
}

void HdNovaMesh::_MaterialSlots(std::vector<SdfPath> &slots, std::vector<uint16_t> &triangleSlots) const {
    std::vector<uint16_t> faceSlot;
    _FaceSlots(slots, faceSlot);
    triangleSlots.clear();
    if (faceSlot.empty())
        return;
    triangleSlots.resize(_triangleFaces.size());
    for (size_t t = 0; t < _triangleFaces.size(); ++t) {
        const int32_t f = _triangleFaces[t];
        triangleSlots[t] = f >= 0 && size_t(f) < faceSlot.size() ? faceSlot[size_t(f)] : 0;
    }
}

bool HdNovaMesh::_GeometryUnchanged(HdSceneDelegate *sd, HdNovaShutter const &shutter, unsigned maxSteps) const {
    // VtValue/VtArray equality short-circuits on shared buffers (IsIdentical),
    // which is what Hydra 2 hands back when only the instancer changed.
    const SdfPath &id = GetId();
    if (!(sd->Get(id, HdTokens->points) == _srcPoints && sd->Get(id, HdTokens->normals) == _srcNormals))
        return false;
    // Over a blurred shutter the values at the frame are not enough: after a
    // time change they can be the previous frame's while the motion within
    // the shutter differs (a mesh starting or stopping to move).
    if (shutter.IsBlurred()) {
        std::vector<VtVec3fArray> pointSteps, normalSteps;
        HdNovaSampleVec3fArray(sd, id, HdTokens->points, shutter, maxSteps, &pointSteps);
        HdNovaSampleVec3fArray(sd, id, HdTokens->normals, shutter, maxSteps, &normalSteps);
        if (motionHash(pointSteps, normalSteps) != _srcMotionHash)
            return false;
    }
    const std::vector<PrimvarRef> current = shadingPrimvars(sd, id);
    if (current.size() != _srcPrimvars.size())
        return false;
    for (size_t i = 0; i < current.size(); ++i)
        if (current[i].name != _srcPrimvars[i].first || !(sd->Get(id, current[i].name) == _srcPrimvars[i].second))
            return false;
    return true;
}

void HdNovaMesh::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits,
                      TfToken const &) {
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    const SdfPath &id = GetId();
    const HdDirtyBits bits = *dirtyBits;
    const bool first = _meshId == nova::kInvalidId;

    if (bits & HdChangeTracker::DirtyMaterialId)
        SetMaterialId(sd->GetMaterialId(id));

    // Display style first: the topology keeps the refine level it was built with.
    if (bits & HdChangeTracker::DirtyDisplayStyle)
        _refineLevel = GetDisplayStyle(sd).refineLevel;
    // The render pass's display-style resync (new subdivision settings)
    // re-dirties topology, points and primvars of every mesh; compare the
    // topology so meshes whose tessellation rate stays keep their BLAS.
    bool topologyChanged = false;
    if (bits & HdChangeTracker::DirtyTopology) {
        HdMeshTopology topology(GetMeshTopology(sd), _refineLevel);
        topologyChanged = first || !(topology.GetPxOsdMeshTopology() == _topology.GetPxOsdMeshTopology());
        _topology = std::move(topology);
        _implicitSurface = isImplicitSurface(sd, id);
    }
    // Subdivision tags come separately from the topology; fetch them whenever
    // either changes (hdEmbree misses them on the first sync).
    if (bits & (HdChangeTracker::DirtyTopology | HdChangeTracker::DirtySubdivTags))
        _subdivTags = _topology.GetScheme() != PxOsdOpenSubdivTokens->none ? GetSubdivTags(sd) : PxOsdSubdivTags();
    if (bits & HdChangeTracker::DirtyPrimvar) {
        _primSubdivLevel = -1;
        HdInterpolation interp;
        if (findPrimvar(sd, id, _meshTokens->subdivLevel, interp) && interp == HdInterpolationConstant) {
            const VtValue v = VtValue::Cast<int>(sd->Get(id, _meshTokens->subdivLevel));
            if (v.IsHolding<int>())
                _primSubdivLevel = v.UncheckedGet<int>();
        }
        // Per-object ray limits (constant primvars; Karma's pattern).
        _rayLimits = nova::RayLimitOverrides{};
        const TfToken limits[nova::kLimitClasses] = {_meshTokens->diffuseLimit, _meshTokens->reflectLimit,
                                                     _meshTokens->refractLimit, _meshTokens->sssLimit,
                                                     _meshTokens->volumeLimit};
        for (int k = 0; k < nova::kLimitClasses; ++k)
            if (findPrimvar(sd, id, limits[k], interp) && interp == HdInterpolationConstant) {
                const VtValue v = VtValue::Cast<float>(sd->Get(id, limits[k]));
                if (v.IsHolding<float>() && std::isfinite(v.UncheckedGet<float>()))
                    _rayLimits.limit[k] = std::max(v.UncheckedGet<float>(), 0.0f);
            }
        if (findPrimvar(sd, id, _meshTokens->thinWallAtRefractLimit, interp) && interp == HdInterpolationConstant) {
            const VtValue v = VtValue::Cast<bool>(sd->Get(id, _meshTokens->thinWallAtRefractLimit));
            if (v.IsHolding<bool>())
                _rayLimits.thinWallAtRefractLimit = v.UncheckedGet<bool>() ? 1 : 0;
        }
        if (findPrimvar(sd, id, _meshTokens->fakeCaustics, interp) && interp == HdInterpolationConstant) {
            const VtValue v = VtValue::Cast<bool>(sd->Get(id, _meshTokens->fakeCaustics));
            if (v.IsHolding<bool>())
                _rayLimits.fakeCaustics = v.UncheckedGet<bool>() ? 1 : 0;
        }
        _lpeTag = HdNovaObjectLpeTag(sd, id); // light path expressions
        _lightSource = HdNovaMeshLightSource(sd, id); // emissive meshes as lights (M9 D14)
        // Displacement dicing (M4.6).
        _dicingQuality = 0.5f;
        _dicingDepth = -1;
        _displacementEnabled = true;
        if (findPrimvar(sd, id, _meshTokens->dicingQuality, interp) && interp == HdInterpolationConstant) {
            const VtValue v = VtValue::Cast<float>(sd->Get(id, _meshTokens->dicingQuality));
            if (v.IsHolding<float>() && std::isfinite(v.UncheckedGet<float>()) && v.UncheckedGet<float>() > 0.0f)
                _dicingQuality = v.UncheckedGet<float>();
        }
        if (findPrimvar(sd, id, _meshTokens->dicingDepth, interp) && interp == HdInterpolationConstant) {
            const VtValue v = VtValue::Cast<int>(sd->Get(id, _meshTokens->dicingDepth));
            if (v.IsHolding<int>())
                _dicingDepth = v.UncheckedGet<int>();
        }
        if (findPrimvar(sd, id, _meshTokens->displacement, interp) && interp == HdInterpolationConstant) {
            const VtValue v = VtValue::Cast<bool>(sd->Get(id, _meshTokens->displacement));
            if (v.IsHolding<bool>())
                _displacementEnabled = v.UncheckedGet<bool>();
        }
    }
    const bool limitsDirty = first || !(_rayLimits == _appliedRayLimits);
    const int subdivLevel = _ResolveSubdivLevel(*param);
    const int subdivRate = _ResolveSubdivRate(subdivLevel, param->GetSubdivMaxTriangles());
    TF_DEBUG(HDNOVA_SUBDIV).Msg("Nova: %s refineLevel %d -> subdivision level %d (rate %d)\n", id.GetText(),
                                _refineLevel, subdivLevel, subdivRate);

    _UpdateVisibility(sd, dirtyBits);
    _UpdateInstancer(sd, dirtyBits);
    // The render index never syncs instancers: every rprim syncs its chain
    // (locked per instancer, a no-op when clean) before reading from it.
    HdInstancer::_SyncInstancerAndParents(sd->GetRenderIndex(), GetInstancerId());

    // Motion is sampled over the render camera's shutter; a new shutter
    // resamples everything (the render pass dirties the prims for it).
    const HdNovaShutter shutter = param->GetShutter();
    const unsigned maxSteps = param->GetMaxMotionSteps();
    const bool newShutter = !(shutter == _sampledShutter) || maxSteps != _sampledMaxSteps;

    // A budget change only matters where it changes the tessellation rate.
    const bool subdivChanged = subdivLevel != _builtSubdivLevel || subdivRate != _builtSubdivRate ||
                               (bits & HdChangeTracker::DirtySubdivTags);
    bool geometryDirty = first || subdivChanged ||
                         (bits & (HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyPoints |
                                  HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyPrimvar));
    // Hydra 2 flattens primvar inheritance, so editing an instance primvar of
    // a point instancer (positions, orientations, ...) also marks every
    // prototype Points|Normals|Primvar dirty, and a display-style resync marks
    // Topology as well. Keep the BLAS if nothing changed.
    // Displacement (M4.6): the slots' materials' displacement and the dicing
    // state; a change rebuilds the geometry.
    std::vector<SdfPath> slotPaths;
    std::vector<uint16_t> faceSlots;
    _FaceSlots(slotPaths, faceSlots);
    HdNovaMaterialRegistry &registry = param->GetMaterials();
    std::vector<HdNovaDisplacementPtr> displacements(slotPaths.size());
    bool displaces = false, readsTime = false;
    if (_displacementEnabled)
        for (size_t i = 0; i < slotPaths.size(); ++i) {
            displacements[i] = slotPaths[i].IsEmpty() ? nullptr : registry.GetDisplacement(slotPaths[i]);
            displaces = displaces || displacements[i] != nullptr;
            readsTime = readsTime || (displacements[i] && HdNovaDisplacementReadsTime(*displacements[i]));
        }
    const nova::DicingView dicingView = param->GetDicingView();
    const bool waiting = displaces && !dicingView.valid && _dicingDepth < 0; // diced once the view is known
    uint64_t displacementSignature = 0;
    if (displaces && !waiting) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        auto mixFloat = [&mix](float x) {
            uint32_t bits;
            std::memcpy(&bits, &x, sizeof(bits));
            mix(bits);
        };
        for (HdNovaDisplacementPtr const &d : displacements)
            mix(d ? d->hash : 0);
        // Which faces displace with what (GeomSubsets): a membership edit
        // keeps the slots but moves the displacement between faces.
        mix(faceSlots.size());
        for (const uint16_t slot : faceSlots)
            mix(slot < displacements.size() && displacements[slot] ? displacements[slot]->hash : 0);
        mixFloat(_dicingQuality);
        mixFloat(param->GetDicingQualityScale());
        mixFloat(param->GetOffscreenQuality());
        mix(uint64_t(int64_t(_dicingDepth)));
        // The Time and Frame globals are evaluated here, not per sample.
        if (readsTime) {
            mixFloat(param->GetFrame());
            mixFloat(param->GetFps());
        }
        displacementSignature = h | 1;
    }
    const bool displacementChanged = displacementSignature != _builtDisplacement;
    geometryDirty = geometryDirty || displacementChanged;
    if (geometryDirty && !first && !newShutter && !subdivChanged && !topologyChanged && !displacementChanged &&
        _GeometryUnchanged(sd, shutter, maxSteps))
        geometryDirty = false;
    const bool instancesDirty =
        first || (bits & (HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyInstancer |
                          HdChangeTracker::DirtyInstanceIndex));
    const bool visibilityDirty = bits & HdChangeTracker::DirtyVisibility;
    const bool primIdDirty = bits & HdChangeTracker::DirtyPrimID;
    // A GeomSubset rebind arrives as topology dirtiness without a topology change.
    const bool materialsDirty =
        first || geometryDirty || (bits & (HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyTopology));
    if (materialsDirty)
        registry.BindMesh(id, slotPaths);
    if (displaces)
        param->NoteDisplacedMesh(id, waiting, readsTime);
    else
        param->ForgetDisplacedMesh(id);

    const bool categoriesDirty = bits & HdChangeTracker::DirtyCategories;
    if (!(geometryDirty || instancesDirty || visibilityDirty || primIdDirty || materialsDirty || limitsDirty ||
          categoriesDirty)) {
        *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
        return; // nothing that affects the image: keep rendering
    }

    // Heavy Hydra-side work first, while the render keeps running.
    const GfMatrix4d xf = sd->GetTransform(id);
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
                    instanceAttributes.clear(); // inconsistent data: none
        }
    } else if (instancesDirty) {
        std::vector<GfMatrix4d> sampled;
        HdNovaSampleTransform(sd, id, shutter, maxSteps, &sampled);
        TF_DEBUG(HDNOVA_MOTION).Msg("Nova: %s shutter [%g, %g]: %zu transform step(s)\n", id.GetText(),
                                    shutter.open, shutter.close, sampled.size());
        for (GfMatrix4d const &m : sampled)
            xfSteps.push_back(toNova(m));
    }
    nova::MeshDesc desc;
    bool built = false;
    if (geometryDirty) {
        _builtSubdivLevel = subdivLevel;
        _builtSubdivRate = subdivRate;
        built = _BuildMesh(sd, *param, subdivLevel, subdivRate, displacementSignature ? &displacements : nullptr,
                           faceSlots, desc);
        _builtDisplacement = built ? displacementSignature : 0;
        TF_DEBUG(HDNOVA_SUBDIV).Msg("Nova: %s geometry built (subdivision rate %d)\n", id.GetText(), subdivRate);
        if (built) {
            desc.transform = toNova(xf); // the single instance of a non-instanced mesh
            desc.visible = IsVisible();
            desc.userId = GetPrimId();
            desc.triangleFaces = _triangleFaces; // the face-index AOV (Karma's ray:primid)
        }
    }
    _sampledShutter = shutter;
    _sampledMaxSteps = maxSteps;

    nova::Scene &scene = param->AcquireSceneForEdit();
    if (first)
        _meshId = scene.createMesh();
    if (geometryDirty)
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
    if (materialsDirty) {
        std::vector<SdfPath> slotPaths;
        std::vector<uint16_t> triangleSlots;
        _MaterialSlots(slotPaths, triangleSlots);
        std::vector<nova::MaterialId> slots;
        for (SdfPath const &path : slotPaths)
            slots.push_back(param->GetMaterials().Acquire(path, scene));
        if (triangleSlots.empty() && slots.size() == 1 && slots[0] == nova::kDefaultMaterialId)
            slots.clear();
        scene.setMeshMaterials(_meshId, std::move(slots), std::move(triangleSlots));
    }
    if (limitsDirty) {
        scene.setMeshRayLimits(_meshId, _rayLimits);
        _appliedRayLimits = _rayLimits;
    }
    if (first || _lpeTag != _appliedLpeTag) {
        scene.setMeshLpeTag(_meshId, _lpeTag);
        _appliedLpeTag = _lpeTag;
    }
    if (first || !(_lightSource == _appliedLightSource)) {
        scene.setMeshLightSource(_meshId, _lightSource);
        _appliedLightSource = _lightSource;
    }
    if (first || (bits & (HdChangeTracker::DirtyCategories | HdChangeTracker::DirtyInstancer |
                          HdChangeTracker::DirtyInstanceIndex)))
        HdNovaSyncCategories(sd, id, GetInstancerId(), scene, _meshId); // light linking (M9 D15)
    if (first || (bits & (HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyInstancer))) {
        HdNovaCryptoNames &names = param->GetCryptoNames(); // cryptomatte ids (D10)
        scene.setMeshCrypto(_meshId, names.Note("__name", HdNovaCryptoObjectName(sd, id, GetInstancerId())),
                            HdNovaCryptoAttributes(sd, id, names));
    }
    if (!geometryDirty) { // updateMesh() already carried these
        if (visibilityDirty)
            scene.setMeshVisible(_meshId, IsVisible());
        if (primIdDirty)
            scene.setMeshUserId(_meshId, GetPrimId());
    }

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

void HdNovaMesh::Finalize(HdRenderParam *renderParam) {
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    param->GetMaterials().BindMesh(GetId(), {});
    param->ForgetDisplacedMesh(GetId());
    if (_meshId != nova::kInvalidId) {
        param->AcquireSceneForEdit().removeMesh(_meshId);
        _meshId = nova::kInvalidId;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
