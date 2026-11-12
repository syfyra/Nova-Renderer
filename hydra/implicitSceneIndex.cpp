#include "implicitSceneIndex.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/geomUtil/capsuleMeshGenerator.h>
#include <pxr/imaging/geomUtil/coneMeshGenerator.h>
#include <pxr/imaging/geomUtil/cylinderMeshGenerator.h>
#include <pxr/imaging/hd/capsuleSchema.h>
#include <pxr/imaging/hd/coneSchema.h>
#include <pxr/imaging/hd/cylinderSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/pxOsd/meshTopology.h>
#include <pxr/imaging/pxOsd/tokens.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

enum class Shape { None, Cylinder, Cone, Capsule };

Shape shapeOf(TfToken const &type) {
    if (type == HdPrimTypeTokens->cylinder)
        return Shape::Cylinder;
    if (type == HdPrimTypeTokens->cone)
        return Shape::Cone;
    if (type == HdPrimTypeTokens->capsule)
        return Shape::Capsule;
    return Shape::None;
}

TfToken const &schemaOf(Shape shape) {
    return shape == Shape::Cylinder ? HdCylinderSchemaTokens->cylinder
                                    : shape == Shape::Cone ? HdConeSchemaTokens->cone : HdCapsuleSchemaTokens->capsule;
}

// The generators build along z; the frame that puts their z on the prim's
// axis (as UsdImaging's own conversion does).
GfMatrix4d frameOf(TfToken const &axis) {
    GfVec4d u = GfVec4d::XAxis(), v = GfVec4d::YAxis(), spine = GfVec4d::ZAxis();
    if (axis == HdCylinderSchemaTokens->X) {
        u = GfVec4d::YAxis(), v = GfVec4d::ZAxis(), spine = GfVec4d::XAxis();
    } else if (axis == HdCylinderSchemaTokens->Y) {
        u = GfVec4d::ZAxis(), v = GfVec4d::XAxis(), spine = GfVec4d::YAxis();
    }
    GfMatrix4d frame;
    frame.SetRow(0, u);
    frame.SetRow(1, v);
    frame.SetRow(2, spine);
    frame.SetRow(3, GfVec4d::WAxis());
    return frame;
}

// The prim's parameters. The three schemas name them alike; a cylinder and a
// capsule may have a radius at each end.
struct Parameters {
    HdSampledDataSourceHandle height, radius, radiusBottom, radiusTop, axis;

    Parameters(Shape shape, HdContainerDataSourceHandle const &prim) {
        const HdContainerDataSourceHandle c = prim ? HdContainerDataSource::Cast(prim->Get(schemaOf(shape))) : nullptr;
        if (!c)
            return;
        height = HdSampledDataSource::Cast(c->Get(HdCylinderSchemaTokens->height));
        radius = HdSampledDataSource::Cast(c->Get(HdCylinderSchemaTokens->radius));
        axis = HdSampledDataSource::Cast(c->Get(HdCylinderSchemaTokens->axis));
        if (shape != Shape::Cone) {
            radiusBottom = HdSampledDataSource::Cast(c->Get(HdCylinderSchemaTokens->radiusBottom));
            radiusTop = HdSampledDataSource::Cast(c->Get(HdCylinderSchemaTokens->radiusTop));
        }
    }
    static float number(HdSampledDataSourceHandle const &ds, HdSampledDataSource::Time t, float fallback) {
        const VtValue v = ds ? ds->GetValue(t) : VtValue();
        return v.IsHolding<double>() ? float(v.UncheckedGet<double>())
                                     : v.IsHolding<float>() ? v.UncheckedGet<float>() : fallback;
    }
};

// The mesh's points or its normals at a time, from the parameters then.
class GeneratedDataSource final : public HdVec3fArrayDataSource {
public:
    HD_DECLARE_DATASOURCE(GeneratedDataSource);

    VtValue GetValue(Time t) override { return VtValue(GetTypedValue(t)); }

    VtVec3fArray GetTypedValue(Time t) override {
        const Parameters p(_shape, _prim);
        // UsdGeom's fallbacks: height 2 (a capsule's 1), radius 1 (a capsule's 0.5), along z.
        const bool capsule = _shape == Shape::Capsule;
        const float height = Parameters::number(p.height, t, capsule ? 1.0f : 2.0f);
        const float radius = Parameters::number(p.radius, t, capsule ? 0.5f : 1.0f);
        const float bottom = Parameters::number(p.radiusBottom, t, radius);
        const float top = Parameters::number(p.radiusTop, t, radius);
        const VtValue axisValue = p.axis ? p.axis->GetValue(t) : VtValue();
        const GfMatrix4d frame =
            frameOf(axisValue.IsHolding<TfToken>() ? axisValue.UncheckedGet<TfToken>() : TfToken());
        const size_t sides = HdNovaImplicitSceneIndex::kSides, rings = HdNovaImplicitSceneIndex::kCapRings;
        VtVec3fArray out;
        switch (_shape) {
        case Shape::Cylinder:
            out.resize(GeomUtilCylinderMeshGenerator::ComputeNumPoints(sides));
            if (_normals)
                GeomUtilCylinderMeshGenerator::GenerateNormals(out.begin(), sides, bottom, top, height, &frame);
            else
                GeomUtilCylinderMeshGenerator::GeneratePoints(out.begin(), sides, bottom, top, height, &frame);
            break;
        case Shape::Cone:
            out.resize(GeomUtilConeMeshGenerator::ComputeNumPoints(sides));
            if (_normals)
                GeomUtilConeMeshGenerator::GenerateNormals(out.begin(), sides, radius, height, &frame);
            else
                GeomUtilConeMeshGenerator::GeneratePoints(out.begin(), sides, radius, height, &frame);
            break;
        default:
            out.resize(GeomUtilCapsuleMeshGenerator::ComputeNumPoints(sides, rings));
            if (_normals)
                GeomUtilCapsuleMeshGenerator::GenerateNormals(out.begin(), sides, rings, bottom, top, height, &frame);
            else
                GeomUtilCapsuleMeshGenerator::GeneratePoints(out.begin(), sides, rings, bottom, top, height, &frame);
            break;
        }
        return out;
    }

    bool GetContributingSampleTimesForInterval(Time start, Time end, std::vector<Time> *times) override {
        const Parameters p(_shape, _prim);
        std::vector<HdSampledDataSourceHandle> sources;
        for (HdSampledDataSourceHandle const &ds : {p.height, p.radius, p.radiusBottom, p.radiusTop, p.axis})
            if (ds)
                sources.push_back(ds);
        return !sources.empty() &&
               HdGetMergedContributingSampleTimesForInterval(sources.size(), sources.data(), start, end, times);
    }

private:
    GeneratedDataSource(Shape shape, HdContainerDataSourceHandle prim, bool normals)
        : _shape(shape), _prim(std::move(prim)), _normals(normals) {}

    Shape _shape;
    HdContainerDataSourceHandle _prim;
    bool _normals;
};

// The mesh of a shape: its topology depends on the counts alone.
HdContainerDataSourceHandle meshOf(Shape shape) {
    const auto build = [](PxOsdMeshTopology const &topology) {
        return HdMeshSchema::Builder()
            .SetTopology(HdMeshTopologySchema::Builder()
                             .SetFaceVertexCounts(
                                 HdRetainedTypedSampledDataSource<VtIntArray>::New(topology.GetFaceVertexCounts()))
                             .SetFaceVertexIndices(
                                 HdRetainedTypedSampledDataSource<VtIntArray>::New(topology.GetFaceVertexIndices()))
                             .SetOrientation(HdRetainedTypedSampledDataSource<TfToken>::New(
                                 HdMeshTopologySchemaTokens->rightHanded))
                             .Build())
            .SetSubdivisionScheme(HdRetainedTypedSampledDataSource<TfToken>::New(PxOsdOpenSubdivTokens->none))
            .Build();
    };
    const size_t sides = HdNovaImplicitSceneIndex::kSides, rings = HdNovaImplicitSceneIndex::kCapRings;
    static const HdContainerDataSourceHandle cylinder = build(GeomUtilCylinderMeshGenerator::GenerateTopology(sides));
    static const HdContainerDataSourceHandle cone = build(GeomUtilConeMeshGenerator::GenerateTopology(sides));
    static const HdContainerDataSourceHandle capsule =
        build(GeomUtilCapsuleMeshGenerator::GenerateTopology(sides, rings));
    return shape == Shape::Cylinder ? cylinder : shape == Shape::Cone ? cone : capsule;
}

HdDataSourceBaseHandle primvarOf(Shape shape, HdContainerDataSourceHandle const &prim, bool normals) {
    return HdPrimvarSchema::Builder()
        .SetPrimvarValue(GeneratedDataSource::New(shape, prim, normals))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->vertex))
        .SetRole(HdPrimvarSchema::BuildRoleDataSource(normals ? HdPrimvarSchemaTokens->normal
                                                              : HdPrimvarSchemaTokens->point))
        .Build();
}

} // namespace

HdNovaImplicitSceneIndexRefPtr HdNovaImplicitSceneIndex::New(HdSceneIndexBaseRefPtr const &input) {
    return TfCreateRefPtr(new HdNovaImplicitSceneIndex(input));
}

HdNovaImplicitSceneIndex::HdNovaImplicitSceneIndex(HdSceneIndexBaseRefPtr const &input)
    : HdSingleInputFilteringSceneIndexBase(input) {}

HdSceneIndexPrim HdNovaImplicitSceneIndex::GetPrim(SdfPath const &primPath) const {
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    const Shape shape = shapeOf(prim.primType);
    if (shape == Shape::None || !prim.dataSource)
        return prim;
    // The prim's own schema stays: HdNovaMesh knows such a mesh by it.
    const HdContainerDataSourceHandle input = prim.dataSource;
    prim.primType = HdPrimTypeTokens->mesh;
    prim.dataSource = HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(
            HdMeshSchema::GetSchemaToken(), meshOf(shape), HdPrimvarsSchema::GetSchemaToken(),
            HdRetainedContainerDataSource::New(HdPrimvarsSchemaTokens->points, primvarOf(shape, input, false),
                                               HdPrimvarsSchemaTokens->normals, primvarOf(shape, input, true))),
        input);
    return prim;
}

SdfPathVector HdNovaImplicitSceneIndex::GetChildPrimPaths(SdfPath const &primPath) const {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdNovaImplicitSceneIndex::_PrimsAdded(HdSceneIndexBase const &,
                                           HdSceneIndexObserver::AddedPrimEntries const &entries) {
    bool any = false;
    for (HdSceneIndexObserver::AddedPrimEntry const &e : entries)
        any = any || shapeOf(e.primType) != Shape::None;
    if (!any) {
        _SendPrimsAdded(entries);
        return;
    }
    HdSceneIndexObserver::AddedPrimEntries out(entries);
    for (HdSceneIndexObserver::AddedPrimEntry &e : out)
        if (shapeOf(e.primType) != Shape::None)
            e.primType = HdPrimTypeTokens->mesh;
    _SendPrimsAdded(out);
}

void HdNovaImplicitSceneIndex::_PrimsRemoved(HdSceneIndexBase const &,
                                             HdSceneIndexObserver::RemovedPrimEntries const &entries) {
    _SendPrimsRemoved(entries);
}

void HdNovaImplicitSceneIndex::_PrimsDirtied(HdSceneIndexBase const &,
                                             HdSceneIndexObserver::DirtiedPrimEntries const &entries) {
    // A parameter of the shape moves the mesh's points and normals.
    static const HdDataSourceLocatorSet shapes{HdCylinderSchema::GetDefaultLocator(), HdConeSchema::GetDefaultLocator(),
                                               HdCapsuleSchema::GetDefaultLocator()};
    std::vector<size_t> touched;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].dirtyLocators.Intersects(shapes))
            touched.push_back(i);
    if (touched.empty()) {
        _SendPrimsDirtied(entries);
        return;
    }
    HdSceneIndexObserver::DirtiedPrimEntries out(entries);
    for (size_t i : touched) {
        out[i].dirtyLocators.insert(HdPrimvarsSchema::GetPointsLocator());
        out[i].dirtyLocators.insert(HdPrimvarsSchema::GetNormalsLocator());
    }
    _SendPrimsDirtied(out);
}

PXR_NAMESPACE_CLOSE_SCOPE
