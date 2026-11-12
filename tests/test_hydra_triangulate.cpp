// Concave polygons. HdMeshUtil triangulates every face as a fan from its
// first vertex; for a concave face that is not star-shaped from that vertex
// the fan covers what the polygon does not (an L started at its tip: 10.4
// units of area for 7; Karma renders 7). HdNovaConcaveFaces replaces the
// triangles of those faces, and of no others.

#include "hydra/meshTriangulate.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/pxOsd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

using Outline = std::vector<GfVec2d>;

Outline star() {
    Outline p;
    for (int k = 0; k < 10; ++k) {
        const double r = k % 2 == 0 ? 2.2 : 0.8, a = (90.0 + 36.0 * k) * 3.14159265358979323846 / 180.0;
        p.push_back({2.5 + r * std::cos(a), 2.0 + r * std::sin(a)});
    }
    return p;
}

// A comb: a bar of height 1 with `teeth` teeth of height 2 on it, counter-clockwise: along the bar's underside,
// then over the teeth from the right.
Outline comb(int teeth) {
    Outline p{{0.0, 0.0}, {2.0 * teeth - 1.0, 0.0}};
    for (int t = teeth - 1; t >= 0; --t) {
        p.push_back({2.0 * t + 1.0, 3.0});
        p.push_back({2.0 * t, 3.0});
        if (t > 0) {
            p.push_back({2.0 * t, 1.0});
            p.push_back({2.0 * t - 1.0, 1.0});
        }
    }
    return p;
}

double areaOf(Outline const &p) {
    double a = 0.0;
    for (size_t i = 0; i < p.size(); ++i)
        a += p[i][0] * p[(i + 1) % p.size()][1] - p[(i + 1) % p.size()][0] * p[i][1];
    return 0.5 * a;
}

// The polygon started at vertex `first`.
Outline rotated(Outline const &p, size_t first) {
    Outline r;
    for (size_t i = 0; i < p.size(); ++i)
        r.push_back(p[(first + i) % p.size()]);
    return r;
}

// One face in a plane through the origin, tilted by `rotation`.
struct Face {
    HdMeshTopology topology;
    VtVec3fArray points;
    GfVec3d normal; // of the polygon's winding
};

Face faceOf(Outline const &p, GfRotation const &rotation, TfToken const &orientation = HdTokens->rightHanded) {
    Face f;
    VtIntArray counts{int(p.size())}, indices;
    for (size_t i = 0; i < p.size(); ++i) {
        indices.push_back(int(i));
        f.points.push_back(GfVec3f(rotation.TransformDir(GfVec3d(p[i][0], p[i][1], 0.0))));
    }
    f.topology = HdMeshTopology(PxOsdOpenSubdivTokens->none, orientation, counts, indices);
    f.normal = rotation.TransformDir(GfVec3d(0.0, 0.0, 1.0));
    return f;
}

GfVec3d doubleArea(VtVec3fArray const &points, GfVec3i const &t) {
    const GfVec3d a(points[t[0]]), b(points[t[1]]), c(points[t[2]]);
    return GfCross(b - a, c - a);
}

struct Triangles {
    VtVec3iArray fan, fixed;
    size_t concave = 0;
};

Triangles triangulate(Face const &f) {
    Triangles t;
    HdMeshUtil util(&f.topology, SdfPath("/face"));
    VtIntArray params;
    util.ComputeTriangleIndices(&t.fan, &params);
    t.fixed = t.fan;
    const HdNovaConcaveFaces concave(f.topology, f.points);
    concave.fixTriangles(&t.fixed);
    t.concave = concave.size();
    return t;
}

const GfRotation kTilts[] = {GfRotation(GfVec3d(1, 0, 0), 0.0), GfRotation(GfVec3d(1, 0, 0), 90.0),
                             GfRotation(GfVec3d(0, 1, 0), -90.0), GfRotation(GfVec3d(1, 2, 3), 37.0),
                             GfRotation(GfVec3d(1, 0, 0), 180.0)};

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

// A stage from USDA text, synced into the delegate's scene.
struct Driver {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    explicit Driver(std::string const &body) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        REQUIRE(layer->ImportFromString("#usda 1.0\n" + body));
        stage = UsdStage::Open(layer);
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        index->InsertSceneIndex(sceneIndices.finalSceneIndex, SdfPath::AbsoluteRootPath(), false);
        pass = delegate->CreateRenderPass(index.get(),
                                          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        tasks.push_back(std::make_shared<SyncTask>(pass));
        sceneIndices.stageSceneIndex->ApplyPendingUpdates();
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
    // The area of the scene's triangles (meshes with an instance), and how many of them face along -y.
    double area(size_t *turned = nullptr) {
        double a = 0.0;
        for (nova::MeshId i = 0; i < scene().meshCount(); ++i) {
            nova::MeshRecord const &record = scene().meshRecord(i);
            if (!record.alive || record.instanceCount() == 0)
                continue;
            nova::MeshDesc const &d = record.desc;
            for (size_t t = 0; t + 2 < d.indices.size(); t += 3) {
                const nova::Vec3f p0 = d.points[d.indices[t]], p1 = d.points[d.indices[t + 1]],
                                  p2 = d.points[d.indices[t + 2]];
                const nova::Vec3f n = nova::cross(p1 - p0, p2 - p0);
                a += 0.5 * double(nova::length(n));
                if (turned && n.y < 0.0f)
                    ++*turned;
            }
        }
        return a;
    }
};

// The U of the Karma comparison in the xz plane, facing +y, started at an arm.
const char *kUPoints = "[(4, 0, -4), (4, 0, -1), (1, 0, -1), (1, 0, -4), (0, 0, -4), (0, 0, 0), (5, 0, 0), (5, 0, -4)]";

} // namespace

TEST_CASE("Concave faces are triangulated inside their polygon", "[hydra][triangulate]") {
    const Outline l{{0, 0}, {4, 0}, {4, 1}, {1, 1}, {1, 4}, {0, 4}};
    const Outline u{{0, 0}, {5, 0}, {5, 4}, {4, 4}, {4, 1}, {1, 1}, {1, 4}, {0, 4}};
    const Outline dart{{0, 0}, {2.5, 1.5}, {5, 0}, {2.5, 4}};
    // Three vertices in a line on an edge, and a vertex twice.
    const Outline lined{{0, 0}, {2, 0}, {4, 0}, {4, 1}, {1, 1}, {1, 2.5}, {1, 4}, {0, 4}, {0, 4}};
    size_t fixedFaces = 0;
    for (Outline const &shape : {l, u, dart, star(), comb(2), comb(7), lined}) {
        const double area = areaOf(shape);
        REQUIRE(area > 0.0);
        for (size_t first = 0; first < shape.size(); ++first) {
            for (GfRotation const &tilt : kTilts) {
                const Face f = faceOf(rotated(shape, first), tilt);
                const Triangles t = triangulate(f);
                REQUIRE(t.fixed.size() == shape.size() - 2);
                fixedFaces += t.concave;
                double sum = 0.0, least = 0.0;
                for (GfVec3i const &tri : t.fixed) {
                    const double along = 0.5 * GfDot(doubleArea(f.points, tri), f.normal);
                    sum += along;
                    least = std::min(least, along);
                }
                // The triangles tile the polygon: none is turned over and their areas add up to its area.
                CHECK(least > -1e-5 * area);
                CHECK(sum == Catch::Approx(area).epsilon(1e-5));
            }
        }
    }
    CHECK(fixedFaces > 100); // the fan is wrong for most starts of these shapes
}

TEST_CASE("Faces whose fan is right keep it", "[hydra][triangulate]") {
    // A triangle, a square, a hexagon, a square with a vertex on an edge, and the L started at its inner
    // corner's opposite (star-shaped from there).
    const Outline tri{{0, 0}, {1, 0}, {0, 1}}, square{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    Outline hexagon;
    for (int k = 0; k < 6; ++k)
        hexagon.push_back({std::cos(k * 1.0471975512), std::sin(k * 1.0471975512)});
    const Outline edge{{0, 0}, {0.5, 0}, {1, 0}, {1, 1}, {0, 1}};
    const Outline l{{0, 0}, {4, 0}, {4, 1}, {1, 1}, {1, 4}, {0, 4}};
    for (Outline const &shape : {tri, square, hexagon, edge, l})
        for (GfRotation const &tilt : kTilts) {
            const Triangles t = triangulate(faceOf(shape, tilt));
            CHECK(t.concave == 0);
            CHECK(t.fixed == t.fan);
        }
    // Every start of a convex polygon.
    for (size_t first = 0; first < hexagon.size(); ++first)
        CHECK(triangulate(faceOf(rotated(hexagon, first), kTilts[3])).concave == 0);
}

TEST_CASE("Concave faces among others: holes, short faces, left-handed meshes", "[hydra][triangulate]") {
    // Faces: a triangle, two vertices (no face), the U started at an arm, a hole, a square.
    VtVec3fArray points;
    VtIntArray counts, indices;
    const auto add = [&](Outline const &p, double x) {
        counts.push_back(int(p.size()));
        for (GfVec2d const &v : p) {
            indices.push_back(int(points.size()));
            points.push_back(GfVec3f(float(v[0] + x), float(v[1]), 0.0f));
        }
    };
    const Outline u = rotated({{0, 0}, {5, 0}, {5, 4}, {4, 4}, {4, 1}, {1, 1}, {1, 4}, {0, 4}}, 3);
    add({{0, 0}, {1, 0}, {0, 1}}, 0.0);
    add({{0, 0}, {1, 0}}, 10.0);
    add(u, 20.0);
    add({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, 30.0);
    add({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, 40.0);
    for (TfToken const &orientation : {HdTokens->rightHanded, HdTokens->leftHanded}) {
        HdMeshTopology topology(PxOsdOpenSubdivTokens->none, orientation, counts, indices, VtIntArray{3});
        HdMeshUtil util(&topology, SdfPath("/mesh"));
        VtVec3iArray fan;
        VtIntArray params;
        util.ComputeTriangleIndices(&fan, &params);
        REQUIRE(fan.size() == 1 + 6 + 2);
        VtVec3iArray fixed = fan;
        const HdNovaConcaveFaces concave(topology, points);
        concave.fixTriangles(&fixed);
        CHECK(concave.size() == 1);
        // The triangle and the square are as they were; the U's six triangles tile it, in the mesh's winding.
        CHECK(fixed[0] == fan[0]);
        CHECK(fixed[7] == fan[7]);
        CHECK(fixed[8] == fan[8]);
        const double sign = orientation == HdTokens->rightHanded ? 1.0 : -1.0;
        CHECK(sign * doubleArea(points, fixed[0])[2] > 0.0);
        double sum = 0.0;
        for (size_t t = 1; t <= 6; ++t) {
            CHECK(HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(params[t]) == 2);
            const double along = 0.5 * sign * doubleArea(points, fixed[t])[2];
            CHECK(along > -1e-9);
            sum += along;
            for (int k = 0; k < 3; ++k) // corners of the U
                CHECK((fixed[t][k] >= 5 && fixed[t][k] < 13));
        }
        CHECK(sum == Catch::Approx(11.0).epsilon(1e-6));

        // A face-varying primvar follows the triangles: each corner gets the value of its face-vertex
        // (here the face-vertex's own number, which is its point's).
        std::vector<float> authored(indices.size() * 2);
        for (size_t i = 0; i < indices.size(); ++i) {
            authored[i * 2] = float(i);
            authored[i * 2 + 1] = 100.0f + float(i);
        }
        VtValue triangulated;
        REQUIRE(util.ComputeTriangulatedFaceVaryingPrimvar(authored.data(), int(indices.size()), HdTypeFloatVec2,
                                                           &triangulated) == HdMeshComputationResult::Success);
        VtVec2fArray corners = triangulated.Get<VtVec2fArray>();
        REQUIRE(corners.size() == fixed.size() * 3);
        concave.fixFaceVarying(authored.data(), indices.size(), 2, corners.data()->data(), corners.size());
        for (size_t t = 0; t < fixed.size(); ++t)
            for (int k = 0; k < 3; ++k) {
                CHECK(corners[t * 3 + size_t(k)][0] == float(fixed[t][k]));
                CHECK(corners[t * 3 + size_t(k)][1] == 100.0f + float(fixed[t][k]));
            }
    }
}

// Displacement dices a polygon mesh as bilinear patches around each face's
// centre, which leaves a concave face as the fan does (a displaced U covered
// 16.7 units for 11): such faces are diced as their triangles.
TEST_CASE("A mesh split at its concave faces keeps its faces and face-vertices", "[hydra][triangulate]") {
    // Faces: a square, the U started at an arm, a hole, two vertices, a triangle.
    VtVec3fArray points;
    VtIntArray counts, indices;
    const auto add = [&](Outline const &p, double x) {
        counts.push_back(int(p.size()));
        for (GfVec2d const &v : p) {
            indices.push_back(int(points.size()));
            points.push_back(GfVec3f(float(v[0] + x), float(v[1]), 0.0f));
        }
    };
    const Outline square{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    add(square, 0.0);
    add(rotated({{0, 0}, {5, 0}, {5, 4}, {4, 4}, {4, 1}, {1, 1}, {1, 4}, {0, 4}}, 3), 10.0);
    add(square, 20.0);
    add({{0, 0}, {1, 0}}, 30.0);
    add({{0, 0}, {1, 0}, {0, 1}}, 40.0);
    const HdMeshTopology topology(PxOsdOpenSubdivTokens->none, HdTokens->rightHanded, counts, indices, VtIntArray{2});
    const HdNovaConcaveFaces concave(topology, points, HdNovaConcaveFaces::Faces::Concave);
    REQUIRE(concave.size() == 1);
    const HdNovaConcaveFaces::Split split = concave.split(topology);
    // The square, six triangles, the hole, the two vertices, the triangle.
    REQUIRE(split.counts == VtIntArray{4, 3, 3, 3, 3, 3, 3, 4, 2, 3});
    CHECK(split.faces == std::vector<int32_t>{0, 1, 1, 1, 1, 1, 1, 2, 3, 4});
    CHECK(split.holes == VtIntArray{7});
    REQUIRE(split.indices.size() == 4 + 18 + 4 + 2 + 3);
    REQUIRE(split.corners.size() == split.indices.size());
    for (size_t c = 0; c < split.corners.size(); ++c) // a face-vertex still names its point
        CHECK(split.indices[c] == indices[size_t(split.corners[c])]);
    double sum = 0.0;
    for (size_t t = 0; t < 6; ++t) {
        const size_t c = 4 + t * 3;
        const GfVec3i triangle(split.indices[c], split.indices[c + 1], split.indices[c + 2]);
        const double area = 0.5 * doubleArea(points, triangle)[2];
        CHECK(area > -1e-9); // in the U's own winding
        sum += area;
    }
    CHECK(sum == Catch::Approx(11.0).epsilon(1e-6));

    // An L started at its outer corner: the fan from there is right, the patches around its centre
    // (which lies outside it) are not.
    const Face l = faceOf({{0, 0}, {4, 0}, {4, 1}, {1, 1}, {1, 4}, {0, 4}}, kTilts[3]);
    CHECK(HdNovaConcaveFaces(l.topology, l.points).empty());
    const HdNovaConcaveFaces whole(l.topology, l.points, HdNovaConcaveFaces::Faces::Concave);
    CHECK(whole.size() == 1);
    CHECK(whole.split(l.topology).counts == VtIntArray{3, 3, 3, 3});
    // Convex faces stay whole either way.
    const Face square4 = faceOf(square, kTilts[3]);
    CHECK(HdNovaConcaveFaces(square4.topology, square4.points, HdNovaConcaveFaces::Faces::Concave).empty());
}

TEST_CASE("Polygons that cannot be triangulated keep a fan", "[hydra][triangulate]") {
    // A bow tie (it crosses itself), a polygon without area, points that are not finite.
    const Outline bow{{0, 0}, {2, 2}, {2, 0}, {0, 2}}, line{{0, 0}, {1, 0}, {2, 0}, {3, 0}, {1.5, 0}};
    for (Outline const &shape : {bow, line}) {
        const Triangles t = triangulate(faceOf(shape, kTilts[3]));
        CHECK(t.fixed.size() == shape.size() - 2);
        for (GfVec3i const &tri : t.fixed)
            for (int k = 0; k < 3; ++k)
                CHECK((tri[k] >= 0 && tri[k] < int(shape.size())));
    }
    Face f = faceOf({{4, 0}, {4, 1}, {1, 1}, {1, 4}, {0, 4}, {0, 0}}, kTilts[0]);
    f.points[2] = GfVec3f(std::nanf(""), 0.0f, 0.0f);
    f.points[4] = GfVec3f(HUGE_VALF, -HUGE_VALF, 0.0f);
    const Triangles t = triangulate(f);
    CHECK(t.fixed.size() == 4);
    for (GfVec3i const &tri : t.fixed)
        for (int k = 0; k < 3; ++k)
            CHECK((tri[k] >= 0 && tri[k] < 6));
}

TEST_CASE("A concave polygon reaches the scene with its own area", "[hydra][triangulate]") {
    const std::string usda = std::string(R"(
def Mesh "u"
{
    int[] faceVertexCounts = [8]
    int[] faceVertexIndices = [0, 1, 2, 3, 4, 5, 6, 7]
    point3f[] points = )") + kUPoints + R"(
    uniform token subdivisionScheme = "none"
)";
    SECTION("points alone") {
        Driver driver(usda + "}\n");
        size_t turned = 0;
        CHECK(driver.area(&turned) == Catch::Approx(11.0).epsilon(1e-5)); // the fan: 20
        CHECK(turned == 0);
    }
    SECTION("left-handed") {
        Driver driver(usda + "    uniform token orientation = \"leftHanded\"\n}\n");
        CHECK(driver.area() == Catch::Approx(11.0).epsilon(1e-5));
    }
    SECTION("with face-varying normals: a corner per triangle vertex") {
        Driver driver(usda + R"(    normal3f[] primvars:normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0),
                                   (0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (
        interpolation = "faceVarying"
    )
}
)");
        size_t turned = 0;
        CHECK(driver.area(&turned) == Catch::Approx(11.0).epsilon(1e-5));
        CHECK(turned == 0);
    }
}
