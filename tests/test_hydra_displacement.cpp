// M4.6 displacement through Hydra (docs/design/M4.md 3.6): a displacing
// material dices and displaces its meshes (fixed rate by nova:dicingDepth,
// or the dicing view), surface-only edits leave them alone, a displacement
// edit queues them for a re-sync and rebuilds them, rebinding to a
// non-displacing material undisplaces, and without a view the mesh waits.

#include "hydra/material/registry.h"
#include "hydra/material/translator.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/geometry/displace.h"
#include "core/shading/compiler.h"

#include <algorithm>
#include <cmath>
#include <functional>

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

// A unit quad (two faces) in z = 0 facing +z, bound to a displacing
// material; dicingDepth 3 (8 segments per edge) unless removed.
const char *kScene = R"(#usda 1.0
def Mesh "quad" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]
    int primvars:nova:dicingDepth = 3 (interpolation = "constant")
    float primvars:touch = 0 (interpolation = "constant")
    rel material:binding = </mat/disp>
}
def Scope "mat"
{
    def Material "disp"
    {
        token outputs:mtlx:surface.connect = </mat/disp/s.outputs:out>
        token outputs:mtlx:displacement.connect = </mat/disp/d.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.1, 0.2, 0.3)
            token outputs:out
        }
        def Shader "d"
        {
            uniform token info:id = "ND_displacement_float"
            float inputs:displacement = 0.1
            token outputs:out
        }
    }
    def Material "flat"
    {
        token outputs:mtlx:surface.connect = </mat/flat/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            token outputs:out
        }
    }
    def Material "vexdisp"
    {
        token outputs:vex:surface.connect = </mat/vexdisp/s.outputs:surface>
        token outputs:vex:displacement.connect = </mat/vexdisp/d.outputs:displacement>
        def Shader "s"
        {
            uniform token info:implementationSource = "sourceCode"
            string info:sourceCode = "surface\nplain()\n{\n    F = diffuse(N);\n}\n"
            token outputs:surface
        }
        def Shader "d"
        {
            uniform token info:implementationSource = "sourceCode"
            string info:sourceCode = "displace\nlift(float amp = 0.1)\n{\n    P += normalize(N) * amp;\n    N = computenormal(P);\n}\n"
            float inputs:amp = 0.2
            token outputs:displacement
        }
    }
}
)";

struct Driver {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    explicit Driver(std::string const &text = kScene) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(text);
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
        sync();
    }
    ~Driver() {
        tasks.clear();
        pass.reset();
        index.reset();
        delegate.reset();
    }
    void sync() {
        sceneIndices.stageSceneIndex->ApplyPendingUpdates();
        engine.Execute(index.get(), &tasks);
        param().GetScene().commit();
    }
    // What the render pass does for pending meshes: make them sync again
    // (the test rig has no resync scene index; any primvar edit does).
    void touch(float v) { set("/quad.primvars:touch", VtValue(v)); }
    HdNovaRenderParam &param() { return *static_cast<HdNovaRenderParam *>(delegate->GetRenderParam()); }
    nova::MeshDesc const &mesh() { return param().GetScene().meshRecord(0).desc; }
    void set(char const *attr, VtValue const &v) { stage->GetAttributeAtPath(SdfPath(attr)).Set(v); }
    float maxZ() {
        float z = -1e9f;
        for (size_t i = 0; i < mesh().pointCount(); ++i)
            z = std::max(z, mesh().points[i].z);
        return z;
    }
};

} // namespace

TEST_CASE("Displacing materials dice and displace their meshes", "[hydra][displacement]") {
    Driver d;
    // 2 faces x 8 x 8 quads x 2 triangles, every point 0.1 up.
    CHECK(d.mesh().triangleCount() == 2 * 64 * 2);
    for (size_t i = 0; i < d.mesh().pointCount(); ++i)
        CHECK_THAT(d.mesh().points[i].z, WithinAbs(0.1, 1e-6));
    const uint64_t updates = d.param().GetScene().meshUpdateCount();
    CHECK(d.param().GetMaterials().TakePendingMeshes().empty());

    SECTION("a surface edit leaves the geometry") {
        d.set("/mat/disp/s.inputs:base_color", VtValue(GfVec3f(0.7f, 0.6f, 0.5f)));
        d.sync();
        CHECK(d.param().GetMaterials().TakePendingMeshes().empty());
        CHECK(d.param().GetScene().meshUpdateCount() == updates);
    }
    SECTION("a displacement edit queues the mesh and rebuilds it") {
        d.set("/mat/disp/d.inputs:displacement", VtValue(0.25f));
        d.sync();
        const std::vector<SdfPath> pending = d.param().GetMaterials().TakePendingMeshes();
        REQUIRE(pending.size() == 1);
        CHECK(pending[0] == SdfPath("/quad"));
        d.touch(1.0f);
        d.sync();
        CHECK_THAT(d.maxZ(), WithinAbs(0.25, 1e-6));
    }
    SECTION("binding a flat material undisplaces") {
        d.stage->GetPrimAtPath(SdfPath("/quad")).GetRelationship(TfToken("material:binding"))
            .SetTargets({SdfPath("/mat/flat")});
        d.sync();
        CHECK_THAT(d.maxZ(), WithinAbs(0.0, 1e-7));
        CHECK(d.mesh().triangleCount() == 4); // the polygon mesh again
    }
    SECTION("nova:displacement false keeps the mesh flat") {
        d.stage->GetPrimAtPath(SdfPath("/quad"))
            .CreateAttribute(TfToken("primvars:nova:displacement"), SdfValueTypeNames->Bool)
            .Set(false);
        d.sync();
        CHECK_THAT(d.maxZ(), WithinAbs(0.0, 1e-7));
    }
}

TEST_CASE("VEX displacement runs through the CVEX bridge", "[hydra][displacement][vex]") {
    // docs/design/M5.md D8: the displace function moves P along N by amp.
    Driver d;
    d.stage->GetPrimAtPath(SdfPath("/quad")).GetRelationship(TfToken("material:binding"))
        .SetTargets({SdfPath("/mat/vexdisp")});
    d.sync();
    CHECK(d.mesh().triangleCount() == 2 * 64 * 2);
    CHECK_THAT(d.maxZ(), WithinAbs(0.2, 1e-6));
    HdNovaDisplacementPtr before = d.param().GetMaterials().GetDisplacement(SdfPath("/mat/vexdisp"));
    REQUIRE(before);
    REQUIRE(before->shader.external);
    d.param().GetMaterials().TakePendingMeshes();

    SECTION("a parameter edit re-displaces without compiling again") {
        d.set("/mat/vexdisp/d.inputs:amp", VtValue(0.3f));
        d.sync();
        CHECK(d.param().GetMaterials().TakePendingMeshes().size() == 1);
        d.touch(1.0f);
        d.sync();
        CHECK_THAT(d.maxZ(), WithinAbs(0.3, 1e-6));
        HdNovaDisplacementPtr after = d.param().GetMaterials().GetDisplacement(SdfPath("/mat/vexdisp"));
        REQUIRE(after);
        CHECK(after->shader.external == before->shader.external);
    }
    SECTION("a code edit compiles the new code") {
        d.set("/mat/vexdisp/d.info:sourceCode",
              VtValue(std::string("displace\nlift(float amp = 0.1)\n{\n    P += normalize(N) * amp * 2;\n}\n")));
        d.sync();
        d.touch(1.0f);
        d.sync();
        CHECK_THAT(d.maxZ(), WithinAbs(0.4, 1e-6));
        HdNovaDisplacementPtr after = d.param().GetMaterials().GetDisplacement(SdfPath("/mat/vexdisp"));
        REQUIRE(after);
        CHECK(after->shader.external != before->shader.external);
    }
    SECTION("code that does not compile renders undisplaced") {
        d.set("/mat/vexdisp/d.info:sourceCode", VtValue(std::string("displace\nlift()\n{\n    P += nothing;\n}\n")));
        d.sync();
        d.touch(1.0f);
        d.sync();
        CHECK_THAT(d.maxZ(), WithinAbs(0.0, 1e-7));
        CHECK(d.param().GetMaterials().Warned(SdfPath("/mat/vexdisp"), "the displacement does not run"));
    }
}

TEST_CASE("Without a dicing view a displaced mesh waits", "[hydra][displacement]") {
    std::string scene = kScene;
    const std::string depth = "    int primvars:nova:dicingDepth = 3 (interpolation = \"constant\")\n";
    scene.erase(scene.find(depth), depth.size());
    Driver d(scene);
    CHECK_THAT(d.maxZ(), WithinAbs(0.0, 1e-7)); // undisplaced until the first frame
    const std::vector<SdfPath> waiting = d.param().TakeWaitingMeshes();
    REQUIRE(waiting.size() == 1);
    // The first frame's view: orthographic, 200 pixels per unit, quality 0.5:
    // 100 segments per unit edge.
    nova::DicingView view;
    view.valid = true;
    view.perspective = false;
    view.pixelsPerUnit = 200.0f;
    view.worldToClip = nova::Mat4::identity();
    view.worldToClip.m[0][0] = view.worldToClip.m[1][1] = 0.5; // the quad inside the frustum
    view.worldToClip.m[3][0] = view.worldToClip.m[3][1] = -0.5;
    d.param().SetDicingView(view);
    d.touch(1.0f);
    d.sync();
    CHECK_THAT(d.maxZ(), WithinAbs(0.1, 1e-6));
    CHECK(d.mesh().triangleCount() == 2 * 100 * 100 * 2);
}

// A GeomSubset membership edit keeps the slots and their materials but moves
// the displacement between faces: the mesh is diced again.
TEST_CASE("Moving faces between subsets moves the displacement", "[hydra][displacement]") {
    std::string scene = kScene;
    const std::string binding = "    rel material:binding = </mat/disp>\n}\n";
    scene.replace(scene.find(binding), binding.size(), R"(    rel material:binding = </mat/flat>
    uniform token subsetFamily:materialBind:familyType = "nonOverlapping"
    def GeomSubset "lifted" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform token elementType = "face"
        uniform token familyName = "materialBind"
        int[] indices = [0]
        rel material:binding = </mat/disp>
    }
}
)");
    Driver d(scene);
    // The highest point of face 0 (x in [0, 1]) and of face 1 (x in [1, 2]),
    // without the shared edge.
    const auto heights = [&d](float &left, float &right) {
        left = right = -1.0f;
        for (size_t i = 0; i < d.mesh().pointCount(); ++i) {
            const nova::Vec3f &p = d.mesh().points[i];
            if (p.x < 0.99f)
                left = std::max(left, p.z);
            else if (p.x > 1.01f)
                right = std::max(right, p.z);
        }
    };
    float left = 0.0f, right = 0.0f;
    heights(left, right);
    CHECK_THAT(left, WithinAbs(0.1, 1e-6));
    CHECK_THAT(right, WithinAbs(0.0, 1e-7));
    d.set("/quad/lifted.indices", VtValue(VtIntArray{1}));
    d.sync();
    heights(left, right);
    CHECK_THAT(left, WithinAbs(0.0, 1e-7));
    CHECK_THAT(right, WithinAbs(0.1, 1e-6));
}

// A displacement that reads the time (a MaterialX time node) follows the
// frame: houdini:frame reaches the tessellation before the next sync, and a
// new frame queues the mesh for a re-sync, which dices it at that frame.
TEST_CASE("Time-dependent displacement follows the frame", "[hydra][displacement]") {
    std::string scene = kScene;
    scene.insert(scene.find("    def Material \"flat\"\n"), R"(    def Material "timed"
    {
        token outputs:mtlx:surface.connect = </mat/timed/s.outputs:out>
        token outputs:mtlx:displacement.connect = </mat/timed/d.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            token outputs:out
        }
        def Shader "t"
        {
            uniform token info:id = "ND_time_float"
            float outputs:out
        }
        def Shader "d"
        {
            uniform token info:id = "ND_displacement_float"
            float inputs:displacement.connect = </mat/timed/t.outputs:out>
            token outputs:out
        }
    }
)");
    Driver d(scene);
    CHECK(d.param().TimeDisplacedMeshes().empty()); // /mat/disp does not read the time
    d.stage->GetPrimAtPath(SdfPath("/quad")).GetRelationship(TfToken("material:binding"))
        .SetTargets({SdfPath("/mat/timed")});
    d.sync();
    REQUIRE(d.param().TimeDisplacedMeshes() == std::vector<SdfPath>{SdfPath("/quad")});
    CHECK_THAT(d.maxZ(), WithinAbs(0.0, 1e-6)); // frame 1: time 0
    // Frame 13 at 24 fps: time 0.5.
    d.delegate->SetRenderSetting(TfToken("houdini:frame"), VtValue(13.0));
    CHECK(d.param().GetFrame() == 13.0f);
    REQUIRE(d.param().TakeFrameChanged()); // the render pass re-syncs the time meshes
    d.touch(1.0f);
    d.sync();
    CHECK_THAT(d.maxZ(), WithinAbs(0.5, 1e-5));
}

// Over a blurred shutter the motion decides, not only the points at the
// frame: a mesh at rest up to frame 10 that moves from there keeps the same
// points at frames 9 and 10, but only frame 10 moves within the shutter.
TEST_CASE("Deformation blur follows the motion within the shutter", "[hydra][motion]") {
    Driver d(R"(#usda 1.0
def Mesh "quad"
{
    uniform token subdivisionScheme = "none"
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]
    point3f[] points.timeSamples = {
        9: [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)],
        10: [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)],
        11: [(0, 0, 1), (1, 0, 1), (2, 0, 1), (0, 1, 1), (1, 1, 1), (2, 1, 1)],
    }
}
)");
    d.param().SetShutter(HdNovaResolveShutter(0.0, 0.5, false, false));
    d.sceneIndices.stageSceneIndex->SetTime(UsdTimeCode(9.0), true); // resampled with the shutter
    d.sync();
    REQUIRE(d.mesh().pointSteps == 1); // at rest within [9, 9.5]
    d.sceneIndices.stageSceneIndex->SetTime(UsdTimeCode(10.0));
    d.sync();
    REQUIRE(d.mesh().pointSteps > 1); // moving within [10, 10.5]
    const nova::MeshDesc &mesh = d.mesh();
    CHECK_THAT(mesh.points[(mesh.pointSteps - 1) * mesh.pointCount()].z, WithinAbs(0.5, 1e-5));
}

TEST_CASE("The triangle budget limits fixed dicing rates", "[hydra][displacement]") {
    Driver d;
    REQUIRE(d.mesh().triangleCount() == 256);
    // 8 segments per edge need about 256 triangles: a budget of 100 dices
    // 0.625x coarser, 5 segments.
    d.param().SetSubdivSettings(-1, 100);
    d.touch(1.0f);
    d.sync();
    CHECK(d.mesh().triangleCount() == 2 * 25 * 2);
    CHECK_THAT(d.maxZ(), WithinAbs(0.1, 1e-6));
}

namespace {

// A material whose displacement is ND_displacement_vector3 `v`.
HdNovaTranslatedMaterial vectorDisplacement(GfVec3f v) {
    HdNovaNetworkSource src;
    src.context = TfToken("mtlx");
    HdMaterialNode2 surface;
    surface.nodeTypeId = TfToken("ND_standard_surface_surfaceshader");
    src.net.nodes[SdfPath("/s")] = surface;
    HdMaterialNode2 disp;
    disp.nodeTypeId = TfToken("ND_displacement_vector3");
    disp.parameters[TfToken("displacement")] = VtValue(v);
    src.net.nodes[SdfPath("/d")] = disp;
    src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("/s"), TfToken()};
    src.net.terminals[HdMaterialTerminalTokens->displacement] = {SdfPath("/d"), TfToken()};
    return HdNovaTranslateMaterial(src);
}

// The offset of the unit quad's first point with st = st(x, y).
nova::Vec3f displacedOffset(GfVec3f v, std::function<nova::Vec2f(float, float)> const &st) {
    const HdNovaTranslatedMaterial tr = vectorDisplacement(v);
    REQUIRE(tr.ok);
    const nova::shading::CompileResult r = nova::shading::compile(tr.graph);
    REQUIRE(r.program);
    REQUIRE(r.program->displacement);
    nova::DisplacementShader shader;
    shader.program = r.program->displacement;
    shader.program->initUniforms(tr.params.data(), shader.uniforms);
    nova::MeshDesc mesh;
    mesh.points = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    nova::MeshAttribute a;
    a.name = "st";
    a.rate = nova::AttributeRate::Vertex;
    a.width = 2;
    for (const nova::Vec3f &p : mesh.points) {
        const nova::Vec2f t = st(p.x, p.y);
        a.data.insert(a.data.end(), {t.x, t.y});
    }
    mesh.attributes.push_back(a);
    nova::DisplacementInput in;
    in.shaders = {&shader};
    in.triangleShader = {0, 0};
    nova::displaceMesh(mesh, in, nova::DisplacementContext{});
    return mesh.points[0];
}

} // namespace

TEST_CASE("Vector displacement uses Karma's tangent frame", "[hydra][displacement]") {
    // docs/research/m4/displacement.md 2.2: T = normalized dP/ds, B the part
    // of dP/dt orthogonal to it on dP/dt's side, object space.
    auto plain = [](float x, float y) { return nova::Vec2f{x, y}; };
    auto skew = [](float x, float y) { return nova::Vec2f{x + 0.5f, x + y + 1.0f}; };
    auto mirror = [](float x, float y) { return nova::Vec2f{x + 0.5f, 0.5f - y}; };
    auto check = [](nova::Vec3f got, nova::Vec3f want) {
        CHECK_THAT(got.x, WithinAbs(want.x, 1e-4));
        CHECK_THAT(got.y, WithinAbs(want.y, 1e-4));
        CHECK_THAT(got.z, WithinAbs(want.z, 1e-4));
    };
    check(displacedOffset({0.2f, 0, 0}, plain), {0.2f, 0, 0});
    check(displacedOffset({0, 0.2f, 0}, plain), {0, 0.2f, 0});
    check(displacedOffset({0, 0, 0.2f}, plain), {0, 0, 0.2f});
    check(displacedOffset({0.2f, 0, 0}, skew), {0.141421f, -0.141421f, 0});
    check(displacedOffset({0, 0.2f, 0}, skew), {0.141421f, 0.141421f, 0});
    check(displacedOffset({0, 0.2f, 0}, mirror), {0, -0.2f, 0});
}
