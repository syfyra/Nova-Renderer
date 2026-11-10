// Media bounded by surfaces through Hydra (docs/design/M11-P5.md F1): a mesh
// or sphere whose material is a volume becomes a scene volume with a shell
// instead of a scene mesh, by Karma's rules (volumeuniform on the prim,
// inherited and array-valued as the Fog Box authors it, or on the volume
// shader; volume-only materials; no material with volumeuniform), and turns
// back into a surface when the material changes.

#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <string>
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

// The Fog Box's layout (geo/render carries the binding and the primvars,
// one-element arrays, its gprim inherits them), a cube mesh and a sphere;
// a kma_volume reading the geomprop density, a surface material, a material
// with both terminals, and a kma_volume with karma:volumeuniform.
const char *kStage = R"(#usda 1.0
def Xform "fog"
{
    def Scope "render" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        rel material:binding = </mtl/fog>
        bool[] primvars:karma:object:volumeuniform = [1] (interpolation = "constant")
        float[] primvars:karma:object:volumeuniformdensity = [0.1] (interpolation = "constant")
        def Mesh "volume"
        {
            int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
            int[] faceVertexIndices = [0, 3, 2, 1, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5, 4, 5, 6, 7]
            point3f[] points = [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5), (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]
            uniform token subdivisionScheme = "none"
            float primvars:touch = 0 (interpolation = "constant")
        }
    }
}
def Sphere "ball" (prepend apiSchemas = ["MaterialBindingAPI"])
{
    double radius = 0.75
    rel material:binding = </mtl/fog>
    bool[] primvars:karma:object:volumeuniform = [1] (interpolation = "constant")
    float primvars:touch = 0 (interpolation = "constant")
    double3 xformOp:translate = (3, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def Scope "mtl"
{
    def Material "fog"
    {
        token outputs:kma:volume.connect = </mtl/fog/v.outputs:out>
        def Shader "v"
        {
            uniform token info:id = "kma_volume"
            color3f inputs:absorption.connect = </mtl/fog/d.outputs:out>
            token outputs:out
        }
        def Shader "d"
        {
            uniform token info:id = "ND_geompropvalue_color3"
            string inputs:geomprop = "density"
            color3f inputs:default = (0.5, 0.5, 0.5)
            color3f outputs:out
        }
    }
    def Material "grey"
    {
        token outputs:mtlx:surface.connect = </mtl/grey/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            token outputs:out
        }
    }
    def Material "both"
    {
        token outputs:mtlx:surface.connect = </mtl/both/s.outputs:out>
        token outputs:kma:volume.connect = </mtl/both/v.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            token outputs:out
        }
        def Shader "v"
        {
            uniform token info:id = "kma_volume"
            color3f inputs:absorption = (0.5, 0.5, 0.5)
            token outputs:out
        }
    }
    def Material "flagged"
    {
        token outputs:mtlx:surface.connect = </mtl/flagged/s.outputs:out>
        token outputs:kma:volume.connect = </mtl/flagged/v.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            token outputs:out
        }
        def Shader "v"
        {
            uniform token info:id = "kma_volume"
            color3f inputs:absorption = (0.5, 0.5, 0.5)
            int inputs:karma:volumeuniform = 1
            token outputs:out
        }
    }
}
)";

// Instanced media: a point instancer over a fog cube and a fog sphere, and
// two native instances of a fog cube.
const char *kInstancedStage = R"(#usda 1.0
def PointInstancer "inst"
{
    point3f[] positions = [(0, 0, 0), (3, 0, 0), (0, 4, 0)]
    int[] protoIndices = [0, 0, 1]
    rel prototypes = [</inst/protos/box>, </inst/protos/ball>]
    def Scope "protos"
    {
        def Mesh "box" (prepend apiSchemas = ["MaterialBindingAPI"])
        {
            int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
            int[] faceVertexIndices = [0, 3, 2, 1, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5, 4, 5, 6, 7]
            point3f[] points = [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5), (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]
            uniform token subdivisionScheme = "none"
            rel material:binding = </mtl/fog>
            bool[] primvars:karma:object:volumeuniform = [1] (interpolation = "constant")
            float primvars:touch = 0 (interpolation = "constant")
        }
        def Sphere "ball" (prepend apiSchemas = ["MaterialBindingAPI"])
        {
            double radius = 0.5
            rel material:binding = </mtl/fog>
            bool[] primvars:karma:object:volumeuniform = [1] (interpolation = "constant")
            float primvars:touch = 0 (interpolation = "constant")
        }
    }
}
def Scope "mtl"
{
    def Material "fog"
    {
        token outputs:kma:volume.connect = </mtl/fog/v.outputs:out>
        def Shader "v"
        {
            uniform token info:id = "kma_volume"
            color3f inputs:absorption = (0.5, 0.5, 0.5)
            token outputs:out
        }
    }
    def Material "grey"
    {
        token outputs:mtlx:surface.connect = </mtl/grey/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            token outputs:out
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
    std::vector<std::string> touched; // the media gprims (their primvars:touch)

    explicit Driver(char const *text = kStage,
                    std::vector<std::string> prims = {"/fog/render/volume", "/ball"})
        : touched(std::move(prims)) {
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
        // Prims that synced before their material: what the render pass does
        // for the prims the material queues.
        touch();
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
        scene().commit();
    }
    void touch() {
        static float t = 0.0f;
        t += 1.0f;
        for (std::string const &prim : touched)
            stage->GetAttributeAtPath(SdfPath(prim + ".primvars:touch")).Set(t);
        sync();
    }
    // The active volumes of the gprim at `path` (instanced: one per
    // instance; UsdImaging re-roots a point instancer's prototypes under
    // <path>/ForInstancer<hash>), ordered by their bounds' lower corner.
    std::vector<const nova::VolumeRecord *> volumes(std::string const &path) {
        std::vector<const nova::VolumeRecord *> out;
        for (nova::VolumeId id : scene().activeVolumes()) {
            std::string const &name = scene().volumeRecord(id).desc.name;
            if (name == path || name.rfind(path + "/", 0) == 0)
                out.push_back(&scene().volumeRecord(id));
        }
        std::sort(out.begin(), out.end(), [](const nova::VolumeRecord *a, const nova::VolumeRecord *b) {
            return a->lower.x != b->lower.x ? a->lower.x < b->lower.x : a->lower.y < b->lower.y;
        });
        return out;
    }
    HdNovaRenderParam &param() { return *static_cast<HdNovaRenderParam *>(delegate->GetRenderParam()); }
    nova::Scene &scene() { return param().GetScene(); }
    void bind(char const *prim, char const *material) {
        UsdRelationship rel = stage->GetPrimAtPath(SdfPath(prim)).CreateRelationship(TfToken("material:binding"));
        if (material)
            rel.SetTargets({SdfPath(material)});
        else
            stage->GetPrimAtPath(SdfPath(prim)).RemoveProperty(TfToken("material:binding"));
        sync();
        touch();
    }
    void setUniform(char const *prim, bool on) {
        stage->GetAttributeAtPath(SdfPath(std::string(prim) + ".primvars:karma:object:volumeuniform"))
            .Set(VtArray<bool>{on});
        sync();
    }
    int liveMeshes() {
        int n = 0;
        for (nova::MeshId i = 0; i < scene().meshCount(); ++i)
            n += scene().meshRecord(i).alive;
        return n;
    }
    const nova::VolumeRecord *volume(char const *name) {
        for (nova::VolumeId id : scene().activeVolumes())
            if (scene().volumeRecord(id).desc.name == name)
                return &scene().volumeRecord(id);
        return nullptr;
    }
};

} // namespace

TEST_CASE("Gprims with volume materials become media bounded by their surface", "[hydra][volume][shell]") {
    Driver d;
    // Both are media: the cube inherits the binding and volumeuniform.
    CHECK(d.liveMeshes() == 0);
    REQUIRE(d.scene().activeVolumes().size() == 2);
    const nova::VolumeRecord *cube = d.volume("/fog/render/volume");
    const nova::VolumeRecord *ball = d.volume("/ball");
    REQUIRE(cube);
    REQUIRE(ball);
    REQUIRE(cube->desc.shell);
    CHECK(cube->desc.shell->kind == nova::ShellKind::Triangles);
    CHECK(cube->desc.shell->openEdges == 0);
    CHECK(cube->desc.shell->triangles.size() == 36);
    REQUIRE(ball->desc.shell);
    CHECK(ball->desc.shell->kind == nova::ShellKind::Sphere);
    CHECK_THAT(ball->desc.shell->radius, WithinAbs(0.75, 1e-6));
    CHECK_THAT(ball->lower.x, WithinAbs(2.25, 1e-4)); // placed by its transform
    CHECK(cube->program != nullptr);                 // the material's volume shader
    CHECK(cube->desc.constants.empty());             // no density primvar: the fallback

    SECTION("a constant primvar the shader reads") {
        d.stage->GetPrimAtPath(SdfPath("/fog/render/volume"))
            .CreateAttribute(TfToken("primvars:density"), SdfValueTypeNames->Float)
            .Set(2.0f);
        d.sync();
        const nova::VolumeRecord *v = d.volume("/fog/render/volume");
        REQUIRE(v);
        REQUIRE(v->desc.constants.size() == 1);
        CHECK(v->desc.constants[0].name == "density");
        CHECK(v->constantValues.size() == 3); // the color3 read, a scalar for each component
    }
    SECTION("volumeuniform off: a volume-only material is still a medium") {
        d.setUniform("/fog/render", false);
        CHECK(d.volume("/fog/render/volume"));
        CHECK(d.liveMeshes() == 0);
    }
    SECTION("a surface material makes it a surface again, and back") {
        d.bind("/fog/render", "/mtl/grey");
        CHECK(d.volume("/fog/render/volume") == nullptr);
        CHECK(d.liveMeshes() == 1);
        d.bind("/fog/render", "/mtl/fog");
        CHECK(d.volume("/fog/render/volume"));
        CHECK(d.liveMeshes() == 0);
    }
    SECTION("both terminals: the volume with volumeuniform, else the surface") {
        d.bind("/fog/render", "/mtl/both");
        CHECK(d.volume("/fog/render/volume"));
        d.setUniform("/fog/render", false);
        d.touch();
        CHECK(d.volume("/fog/render/volume") == nullptr);
        CHECK(d.liveMeshes() == 1);
        d.bind("/fog/render", "/mtl/flagged"); // karma:volumeuniform on the shader
        CHECK(d.volume("/fog/render/volume"));
    }
    SECTION("no material: the white medium with volumeuniform, a surface without") {
        d.bind("/ball", nullptr);
        const nova::VolumeRecord *v = d.volume("/ball");
        REQUIRE(v);
        CHECK(v->program == nullptr); // the default medium (density 1)
        d.setUniform("/ball", false);
        d.touch();
        CHECK(d.volume("/ball") == nullptr);
        CHECK(d.liveMeshes() == 1);
    }
    SECTION("hidden media are inactive") {
        d.stage->GetPrimAtPath(SdfPath("/ball")).CreateAttribute(TfToken("visibility"), SdfValueTypeNames->Token)
            .Set(TfToken("invisible"));
        d.sync();
        CHECK(d.volume("/ball") == nullptr);
        CHECK(d.scene().activeVolumes().size() == 1);
    }
}

// Instanced media (docs/design/M11-P5.md F9): every instance is a volume of
// its own with the gprim's shell, at the instance's transform. They rendered
// nothing before (a warning).
TEST_CASE("Instanced gprims with volume materials are media at every instance", "[hydra][volume][shell][instancing]") {
    Driver d(kInstancedStage, {"/inst/protos/box", "/inst/protos/ball"});
    CHECK(d.liveMeshes() == 0);
    REQUIRE(d.scene().activeVolumes().size() == 3);
    std::vector<const nova::VolumeRecord *> boxes = d.volumes("/inst/protos/box");
    std::vector<const nova::VolumeRecord *> balls = d.volumes("/inst/protos/ball");
    REQUIRE(boxes.size() == 2);
    REQUIRE(balls.size() == 1);
    // One shell, shared; each instance at its position.
    CHECK(boxes[0]->desc.shell == boxes[1]->desc.shell);
    CHECK(boxes[0]->desc.shell->kind == nova::ShellKind::Triangles);
    CHECK_THAT(boxes[0]->lower.x, WithinAbs(-0.5, 1e-4));
    CHECK_THAT(boxes[0]->upper.x, WithinAbs(0.5, 1e-4));
    CHECK_THAT(boxes[1]->lower.x, WithinAbs(2.5, 1e-4));
    CHECK_THAT(boxes[1]->upper.x, WithinAbs(3.5, 1e-4));
    CHECK(balls[0]->desc.shell->kind == nova::ShellKind::Sphere);
    CHECK_THAT(balls[0]->lower.y, WithinAbs(3.5, 1e-4));
    CHECK_THAT(balls[0]->upper.y, WithinAbs(4.5, 1e-4));
    // Stable, distinct keys (the path and the instance), so images do not
    // depend on the sync order.
    CHECK(boxes[0]->key != boxes[1]->key);

    const UsdPrim inst = d.stage->GetPrimAtPath(SdfPath("/inst"));
    SECTION("moved instances move their media") {
        inst.GetAttribute(TfToken("positions")).Set(VtVec3fArray{{0, 0, 0}, {5, 0, 0}, {0, -4, 0}});
        d.sync();
        boxes = d.volumes("/inst/protos/box");
        balls = d.volumes("/inst/protos/ball");
        REQUIRE(boxes.size() == 2);
        REQUIRE(balls.size() == 1);
        CHECK_THAT(boxes[1]->lower.x, WithinAbs(4.5, 1e-4));
        CHECK_THAT(balls[0]->upper.y, WithinAbs(-3.5, 1e-4));
    }
    SECTION("the instance count follows the instancer") {
        inst.GetAttribute(TfToken("positions")).Set(VtVec3fArray{{0, 0, 0}, {3, 0, 0}, {0, 4, 0}, {6, 0, 0}, {9, 0, 0}});
        inst.GetAttribute(TfToken("protoIndices")).Set(VtIntArray{0, 0, 1, 0, 0});
        d.sync();
        boxes = d.volumes("/inst/protos/box");
        REQUIRE(boxes.size() == 4);
        CHECK_THAT(boxes[3]->lower.x, WithinAbs(8.5, 1e-4));
        CHECK(d.volumes("/inst/protos/ball").size() == 1);
        inst.GetAttribute(TfToken("positions")).Set(VtVec3fArray{{0, 4, 0}, {3, 0, 0}});
        inst.GetAttribute(TfToken("protoIndices")).Set(VtIntArray{1, 0});
        d.sync();
        boxes = d.volumes("/inst/protos/box");
        REQUIRE(boxes.size() == 1);
        CHECK_THAT(boxes[0]->lower.x, WithinAbs(2.5, 1e-4));
        CHECK(d.volumes("/inst/protos/ball").size() == 1);
        CHECK(d.scene().activeVolumes().size() == 2);
        // No instance of the box left: nothing of it renders.
        inst.GetAttribute(TfToken("positions")).Set(VtVec3fArray{{0, 4, 0}});
        inst.GetAttribute(TfToken("protoIndices")).Set(VtIntArray{1});
        d.sync();
        CHECK(d.volumes("/inst/protos/box").empty());
        CHECK(d.scene().activeVolumes().size() == 1);
    }
    SECTION("a hidden instancer hides its media, and shows them again") {
        const UsdAttribute visibility = inst.CreateAttribute(TfToken("visibility"), SdfValueTypeNames->Token);
        visibility.Set(TfToken("invisible"));
        d.sync();
        CHECK(d.scene().activeVolumes().empty());
        visibility.Set(TfToken("inherited"));
        d.sync();
        CHECK(d.scene().activeVolumes().size() == 3);
    }
    SECTION("a surface material makes the instances surfaces again, and back") {
        d.stage->GetPrimAtPath(SdfPath("/inst/protos/box"))
            .GetRelationship(TfToken("material:binding"))
            .SetTargets({SdfPath("/mtl/grey")});
        d.sync();
        d.touch();
        CHECK(d.volumes("/inst/protos/box").empty());
        CHECK(d.liveMeshes() == 1);
        CHECK(d.scene().activeVolumes().size() == 1); // the ball
        d.stage->GetPrimAtPath(SdfPath("/inst/protos/box"))
            .GetRelationship(TfToken("material:binding"))
            .SetTargets({SdfPath("/mtl/fog")});
        d.sync();
        d.touch();
        CHECK(d.volumes("/inst/protos/box").size() == 2);
        CHECK(d.liveMeshes() == 0);
    }
}
