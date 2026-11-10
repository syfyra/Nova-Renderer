// Indexed basis curves through Hydra: a curve's vertices are
// points[curveIndices[i]] (HdBasisCurvesTopology::GetCurveIndices), and its
// vertex-rate primvars (widths, shading primvars) are indexed the same way.
// UsdGeomBasisCurves has no indices, so the prim comes from a retained scene
// index, as from any scene index that authors them. Before, the indices were
// ignored with a warning (the points taken in order).

#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include <pxr/imaging/hd/basisCurvesSchema.h>
#include <pxr/imaging/hd/basisCurvesTopologySchema.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;

namespace {

class SyncTask final : public HdTask {
public:
    SyncTask(HdRenderPassSharedPtr pass) : HdTask(SdfPath::EmptyPath()), _pass(std::move(pass)) {}
    void Sync(HdSceneDelegate *, HdTaskContext *, HdDirtyBits *dirtyBits) override {
        _pass->Sync();
        *dirtyBits = HdChangeTracker::Clean;
    }
    void Prepare(HdTaskContext *, HdRenderIndex *) override {}
    void Execute(HdTaskContext *) override {}
    const TfTokenVector &GetRenderTags() const override { return _tags; }

private:
    HdRenderPassSharedPtr _pass;
    TfTokenVector _tags{HdRenderTagTokens->geometry, HdRenderTagTokens->render};
};

template <class T>
HdDataSourceBaseHandle primvar(T value, TfToken const &interpolation, TfToken const &role = TfToken()) {
    return HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<T>::New(std::move(value)))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
        .SetRole(HdPrimvarSchema::BuildRoleDataSource(role))
        .Build();
}

struct Curves {
    VtIntArray counts, indices;
    TfToken type = HdTokens->linear, basis = HdTokens->bspline, wrap = HdTokens->nonperiodic;
    VtVec3fArray points;
    VtFloatArray widths, ids; // vertex rate (per point)
};

struct Driver {
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    HdRetainedSceneIndexRefPtr retained = HdRetainedSceneIndex::New();
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    explicit Driver(Curves const &c) {
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        index->InsertSceneIndex(retained, SdfPath::AbsoluteRootPath(), false);
        pass = delegate->CreateRenderPass(index.get(),
                                          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        tasks.push_back(std::make_shared<SyncTask>(pass));

        HdBasisCurvesTopologySchema::Builder topology;
        topology.SetCurveVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New(c.counts))
            .SetType(HdRetainedTypedSampledDataSource<TfToken>::New(c.type))
            .SetBasis(HdRetainedTypedSampledDataSource<TfToken>::New(c.basis))
            .SetWrap(HdRetainedTypedSampledDataSource<TfToken>::New(c.wrap));
        if (!c.indices.empty())
            topology.SetCurveIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(c.indices));
        std::vector<TfToken> names{HdPrimvarsSchemaTokens->points};
        std::vector<HdDataSourceBaseHandle> values{
            primvar(c.points, HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->point)};
        if (!c.widths.empty()) {
            names.push_back(HdPrimvarsSchemaTokens->widths);
            values.push_back(primvar(c.widths, HdPrimvarSchemaTokens->vertex));
        }
        if (!c.ids.empty()) {
            names.push_back(TfToken("id"));
            values.push_back(primvar(c.ids, HdPrimvarSchemaTokens->vertex));
        }
        retained->AddPrims({{SdfPath("/curves"), HdPrimTypeTokens->basisCurves,
                             HdRetainedContainerDataSource::New(
                                 HdBasisCurvesSchemaTokens->basisCurves,
                                 HdBasisCurvesSchema::Builder().SetTopology(topology.Build()).Build(),
                                 HdPrimvarsSchemaTokens->primvars,
                                 HdRetainedContainerDataSource::New(names.size(), names.data(), values.data()))}});
        engine.Execute(index.get(), &tasks);
        scene().commit();
    }
    ~Driver() {
        tasks.clear();
        pass.reset();
        index.reset();
        delegate.reset();
    }
    nova::Scene &scene() { return static_cast<HdNovaRenderParam *>(delegate->GetRenderParam())->GetScene(); }
    const nova::MeshDesc *curves() {
        for (nova::MeshId i = 0; i < scene().meshCount(); ++i)
            if (scene().meshRecord(i).alive && scene().meshRecord(i).desc.isCurves())
                return &scene().meshRecord(i).desc;
        return nullptr;
    }
};

VtVec3fArray fivePoints() { return {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}}; }

} // namespace

TEST_CASE("Indexed curves take their points and vertex primvars through the indices", "[hydra][curves]") {
    Curves c;
    c.counts = {3, 2};
    c.indices = {4, 0, 2, 3, 1};
    c.points = fivePoints();
    c.widths = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    c.ids = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
    Driver d(c);
    const nova::MeshDesc *desc = d.curves();
    REQUIRE(desc);
    REQUIRE(desc->curveVertexCounts == std::vector<uint32_t>{3, 2});
    REQUIRE(desc->points.size() == 5);
    const float x[5] = {4, 0, 2, 3, 1};
    for (int i = 0; i < 5; ++i) {
        CHECK_THAT(desc->points[size_t(i)].x, WithinAbs(x[i], 1e-6));
        // The width of the point the vertex indexes (a radius: half of it).
        CHECK_THAT(desc->curveRadius[size_t(i)], WithinAbs(0.05 * (x[i] + 1.0), 1e-6));
    }
    // The shading primvar per segment (start, end, end), of the same points.
    const nova::MeshAttribute *id = desc->findAttribute("id");
    REQUIRE(id);
    REQUIRE(id->data.size() == 9); // 3 segments
    const float expected[9] = {4, 0, 0, 0, 2, 2, 3, 1, 1};
    for (int i = 0; i < 9; ++i)
        CHECK_THAT(id->data[size_t(i)], WithinAbs(expected[i], 1e-6));
}

TEST_CASE("Indexed curves need only the points they index", "[hydra][curves]") {
    // Two of five points, in reverse: fewer points than a curve of the same
    // counts in order would need is fine, more points than indexed too.
    Curves c;
    c.counts = {2};
    c.indices = {3, 1};
    c.points = fivePoints();
    Driver d(c);
    const nova::MeshDesc *desc = d.curves();
    REQUIRE(desc);
    REQUIRE(desc->points.size() == 2);
    CHECK_THAT(desc->points[0].x, WithinAbs(3.0, 1e-6));
    CHECK_THAT(desc->points[1].x, WithinAbs(1.0, 1e-6));
}

TEST_CASE("Pinned indexed curves reflect their indexed end points", "[hydra][curves]") {
    Curves c;
    c.counts = {4};
    c.indices = {3, 2, 1, 0};
    c.type = HdTokens->cubic;
    c.basis = HdTokens->bspline;
    c.wrap = HdTokens->pinned;
    c.points = {{0, 0, 0}, {1, 0, 0}, {3, 0, 0}, {7, 0, 0}};
    Driver d(c);
    const nova::MeshDesc *desc = d.curves();
    REQUIRE(desc);
    // Phantom, 7, 3, 1, 0, phantom: 2 p0 - p1 at each end of the indexed curve.
    REQUIRE(desc->points.size() == 6);
    const float x[6] = {11, 7, 3, 1, 0, -1};
    for (int i = 0; i < 6; ++i)
        CHECK_THAT(desc->points[size_t(i)].x, WithinAbs(x[i], 1e-6));
}

TEST_CASE("Curve indices that do not match the counts render nothing", "[hydra][curves]") {
    SECTION("too few indices") {
        Curves c;
        c.counts = {3, 2};
        c.indices = {4, 0, 2};
        c.points = fivePoints();
        Driver d(c);
        CHECK(d.curves() == nullptr);
    }
    SECTION("an index past the points") {
        Curves c;
        c.counts = {3};
        c.indices = {0, 1, 7};
        c.points = fivePoints();
        Driver d(c);
        CHECK(d.curves() == nullptr);
    }
    SECTION("without indices the points are taken in order") {
        Curves c;
        c.counts = {3, 2};
        c.points = fivePoints();
        Driver d(c);
        const nova::MeshDesc *desc = d.curves();
        REQUIRE(desc);
        REQUIRE(desc->points.size() == 5);
        for (int i = 0; i < 5; ++i)
            CHECK_THAT(desc->points[size_t(i)].x, WithinAbs(float(i), 1e-6));
    }
}
