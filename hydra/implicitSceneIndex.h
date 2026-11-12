#pragma once

// Cylinders, cones and capsules.
//
// A renderer without such shapes gets a UsdGeom implicit prim as a mesh:
// pxr's HdsiImplicitSurfaceSceneIndex makes a cage of ten sides with the
// Catmull-Clark scheme. Subdivided (Karma) the cage's limit surface is smooth
// and 6 % narrower than the prim. As a polygon mesh with smooth normals
// (Nova until 0.1.5) it has the prim's size at its ten edges and shows them
// at every silhouette.
//
// This index makes the mesh before pxr's index sees the prim: the same
// generators (GeomUtil) with 96 sides and their own normals, as a polygon
// mesh. It is the prim to 0.05 % of its radius. Spheres are exact
// (hydra/sphere.h); cubes and planes are polygons and stay with pxr's index.

#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DECLARE_WEAK_AND_REF_PTRS(HdNovaImplicitSceneIndex);

class HdNovaImplicitSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static HdNovaImplicitSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input);

    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const &primPath) const override;

    // The sides of a cylinder, cone or capsule, and the rings of a capsule's cap.
    static constexpr size_t kSides = 96, kCapRings = 24;

protected:
    void _PrimsAdded(HdSceneIndexBase const &sender, HdSceneIndexObserver::AddedPrimEntries const &entries) override;
    void _PrimsRemoved(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::RemovedPrimEntries const &entries) override;
    void _PrimsDirtied(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::DirtiedPrimEntries const &entries) override;

private:
    explicit HdNovaImplicitSceneIndex(HdSceneIndexBaseRefPtr const &input);
};

PXR_NAMESPACE_CLOSE_SCOPE
