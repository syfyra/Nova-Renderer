#pragma once

#include "medium.h"
#include "motion.h"
#include "material/registry.h"

#include "core/scene/mesh.h"
#include "core/scene/outline.h"
#include "core/scene/ray_limits.h"
#include "core/scene/visibility.h"

#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/pxOsd/subdivTags.h>
#include <pxr/pxr.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaRenderParam;

class HdNovaMesh final : public HdMesh {
public:
    explicit HdNovaMesh(SdfPath const &id);
    ~HdNovaMesh() override = default;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;

    void Finalize(HdRenderParam *renderParam) override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override;

private:
    // 0 renders the control mesh; > 0 is the uniform subdivision level.
    int _ResolveSubdivLevel(HdNovaRenderParam const &param) const;
    // Tessellation rate for the level within the triangle budget (0: level 0,
    // or the budget leaves only the control mesh).
    int _ResolveSubdivRate(int subdivLevel, uint64_t budget) const;
    bool _BuildMesh(HdSceneDelegate *sceneDelegate, HdNovaRenderParam &param, int subdivLevel, int subdivRate,
                    std::vector<HdNovaDisplacementPtr> const *displacements, std::vector<uint16_t> const &faceSlots,
                    nova::MeshDesc &desc);
    // Displacement (M4.6): the control mesh diced (its subdivision scheme,
    // polygon meshes bilinear) at the dicing view's rates and displaced.
    bool _BuildDisplacedMesh(HdSceneDelegate *sceneDelegate, HdNovaRenderParam &param,
                             std::vector<VtVec3fArray> const &pointSteps,
                             std::vector<HdNovaDisplacementPtr> const &displacements,
                             std::vector<uint16_t> const &faceSlots, nova::MeshDesc &desc);
    // Smooth control-vertex normals of a polygon mesh at step 0: the
    // authored ones (vertex, varying, faceVarying averaged), else the
    // angle-weighted face normals. The displacement direction.
    std::vector<nova::Vec3f> _ControlNormals(HdSceneDelegate *sceneDelegate, VtVec3fArray const &points) const;
    bool _BuildSubdivMesh(HdSceneDelegate *sceneDelegate, std::vector<VtVec3fArray> const &pointSteps,
                          int subdivLevel, int subdivRate, nova::MeshDesc &desc);
    // Whether the sources of the built geometry are unchanged (the shutter
    // and step limit it was built with: over a blurred one, also the motion).
    bool _GeometryUnchanged(HdSceneDelegate *sceneDelegate, HdNovaShutter const &shutter, unsigned maxSteps) const;
    // Material per slot (slot 0: the prim's binding, then its GeomSubsets)
    // and the slot of every authored face / triangle (empty: all slot 0).
    void _FaceSlots(std::vector<SdfPath> &slots, std::vector<uint16_t> &faceSlots) const;
    void _MaterialSlots(std::vector<SdfPath> &slots, std::vector<uint16_t> &triangleSlots) const;

    nova::MeshId _meshId = nova::kInvalidId;
    HdMeshTopology _topology;
    PxOsdSubdivTags _subdivTags;   // GetSubdivTags() comes separately from the topology
    int _refineLevel = 0;          // HdDisplayStyle::refineLevel (viewport LOD, husk --complexity)
    int _primSubdivLevel = -1;     // primvars:nova:subdivLevel (constant int), -1 = not authored
    nova::RayLimitOverrides _rayLimits;        // primvars:nova:object:*Limit (constant)
    nova::RayLimitOverrides _appliedRayLimits; // as last given to the scene
    std::string _lpeTag;                       // primvars:karma:object:lpetag (constant)
    std::string _appliedLpeTag;
    // Emissive meshes as lights (M9 D14): karma:object:treat_as_lightsource
    // (nova:object:lightSource) and lightsource_* (constant).
    nova::MeshLightSource _lightSource;
    nova::MeshLightSource _appliedLightSource;
    // Ray visibility and holdout mode (M11 P2): karma:object:rendervisibility
    // and holdoutmode (constant).
    nova::ObjectVisibility _visibility;
    nova::OutlineOverrides _outline; // Karma's outlines (M11 P9a)
    int _builtSubdivLevel = -1;    // level of the geometry in the scene
    int _builtSubdivRate = -1;     // tessellation rate it was built with (0: control mesh)
    bool _implicitSurface = false; // converted from a UsdGeom implicit prim (sphere, ...)
    // Source values of the last BLAS build, to detect Hydra 2's spurious
    // Points|Normals|Primvar dirtiness after point-instancer primvar edits.
    VtValue _srcPoints, _srcNormals;
    std::vector<std::pair<TfToken, VtValue>> _srcPrimvars; // shading primvars, in descriptor order
    // Over a blurred shutter: a hash of the points and normals as sampled
    // for it (HdNovaSampleVec3fArray; 0 otherwise). A hash, not the steps:
    // deforming meshes would keep a copy of every step for their lifetime.
    uint64_t _srcMotionHash = 0;
    // Authored face of every triangle of the built geometry (material slots).
    std::vector<int32_t> _triangleFaces;
    // Shutter the motion was sampled with; the one of the current sync
    // (HdNovaObjectShutter) for the build helpers.
    HdNovaShutter _sampledShutter;
    HdNovaShutter _shutter;
    unsigned _sampledMaxSteps = 0;
    // Displacement (M4.6): primvars:nova:dicingQuality, :dicingDepth,
    // :displacement, and the signature of what the scene has (0: undisplaced).
    float _dicingQuality = 0.5f;
    int _dicingDepth = -1;
    bool _displacementEnabled = true;
    uint64_t _builtDisplacement = 0;
    // A medium bounded by the mesh (docs/design/M11-P5.md) instead of a
    // surface: its scene volume, and the shell (the control mesh triangulated,
    // at the shutter's middle step).
    nova::MediumShellPtr _BuildShell(HdSceneDelegate *sceneDelegate, HdNovaRenderParam &param);
    HdNovaMedium _medium;
    bool _warnedMediumMotion = false;
    // A light portal (docs/design/M11-P9b.md) instead of a surface: whether
    // the mesh is one (nova:object:isPortal, karma:object:isportal, the
    // render settings'), the dome lights it serves, and whether the scene
    // holds it (Scene::setPortal under the prim's path).
    bool _portal = false;
    std::string _portalDomes = "*";
    bool _portalActive = false;
    bool _SyncPortal(HdSceneDelegate *sceneDelegate, HdNovaRenderParam &param, HdDirtyBits bits);
};

PXR_NAMESPACE_CLOSE_SCOPE
