// Material translation and IPR behaviour (docs/design/M3.md, M3.6).
//
// Translator unit tests feed HdNovaNetworkSource networks like the ones
// Hydra delivers (docs/research/m3/hydra-materials.md 2.3). The IPR tests
// drive a real render index (UsdImaging scene indices, Nova's delegate)
// through USD edits, as Solaris does, and check what reaches the scene: a
// value edit keeps the program and rebuilds no geometry, a geomprop rename
// rebinds the attribute, a GeomSubset rebind keeps the geometry, an edit on
// a node no terminal uses changes nothing, and a removed material renders
// the default.

#include "hydra/material/nodeDefDatabase.h"
#include "hydra/material/principled.h"
#include "hydra/material/registry.h"
#include "hydra/material/translator.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderBuffer.h"
#include "hydra/renderParam.h"

#include "core/bsdf/closure_graph.h"
#include "core/integrator/renderer.h"
#include "test_helpers.h"
#include "core/shading/compiler.h"

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace sf = nova::surface_field;

namespace {

HdMaterialNode2 node(char const *id) {
    HdMaterialNode2 n;
    n.nodeTypeId = TfToken(id);
    return n;
}

HdNovaNetworkSource network(HdMaterialNode2 const &surface) {
    HdNovaNetworkSource src;
    src.context = TfToken("mtlx");
    src.net.nodes[SdfPath("surface")] = surface;
    src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("surface"), TfToken()};
    return src;
}

// The material's resolved (uniform) inputs, as the scene would see them.
nova::MaterialRecord record(HdNovaTranslatedMaterial const &tr) {
    nova::MaterialDesc desc;
    desc.inputs = tr.defaults;
    desc.program = nova::shading::compile(tr.graph).program;
    desc.params = tr.params;
    REQUIRE(desc.program);
    return nova::makeMaterialRecord(desc);
}

} // namespace

// M6.4: Karma's material AOVs (kma_aov: the surface shader's aov:<name>
// inputs) become program outputs, and the shader AOV planes show them.
TEST_CASE("Material AOVs reach the shader AOV planes", "[hydra][materials][aov]") {
    HdMaterialNode2 surface = node("ND_standard_surface_surfaceshader");
    surface.inputConnections[TfToken("aov:myaov")] = {{SdfPath("tint"), TfToken("out")}};
    HdNovaNetworkSource src = network(surface);
    HdMaterialNode2 tint = node("ND_constant_color3");
    tint.parameters[TfToken("value")] = VtValue(GfVec3f(0.25f, 0.5f, 0.75f));
    src.net.nodes[SdfPath("tint")] = tint;
    HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
    REQUIRE(tr.ok);
    REQUIRE(tr.graph.aovs.size() == 1);
    CHECK(tr.graph.aovs[0].first == "myaov");
    CHECK(tr.graph.aovs[0].second >= nova::surface_field::kCount);

    nova::Scene scene;
    nova::MeshDesc quad;
    quad.points = {{-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, {-2, 2, 0}};
    quad.indices = {0, 1, 2, 0, 2, 3};
    const nova::MeshId mesh = scene.createMesh();
    scene.updateMesh(mesh, quad);
    nova::MaterialDesc desc;
    desc.inputs = tr.defaults;
    desc.program = nova::shading::compile(tr.graph).program;
    desc.params = tr.params;
    REQUIRE(desc.program);
    const nova::MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, desc);
    scene.setMeshMaterials(mesh, {m});
    scene.commit();
    nova::Renderer renderer;
    nova::RenderSettings settings;
    settings.maxSamples = 1;
    settings.utility = std::make_shared<const std::vector<nova::UtilityRequest>>(std::vector<nova::UtilityRequest>{
        {nova::UtilityKind::ShaderAov, "myaov"}, {nova::UtilityKind::ShaderAov, "other"}});
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(nova::test::makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 4, 4));
    renderer.resize(4, 4);
    REQUIRE(renderer.renderPass({}));
    const float *v = renderer.film().utility[0].data.data() + 5 * 3;
    CHECK(v[0] == 0.25f);
    CHECK(v[1] == 0.5f);
    CHECK(v[2] == 0.75f);
    CHECK(renderer.film().utility[1].data[5 * 3] == 0.0f); // no such AOV: 0, as Karma
}

TEST_CASE("Unauthored inputs take the node definition's defaults", "[hydra][materials]") {
    HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(network(node("ND_standard_surface_surfaceshader")));
    REQUIRE(tr.ok);
    nova::MaterialRecord r = record(tr);
    CHECK(r.resolved.model == nova::SurfaceModel::StandardSurface);
    CHECK(r.resolved.f(sf::baseWeight) == 1.0f); // 1.0.1: base 1, base_color 0.8
    CHECK(r.resolved.c(sf::baseColor).x == 0.8f);
    CHECK(r.resolved.f(sf::specularRoughness) == 0.2f);
    CHECK_FALSE(r.varying);

    tr = HdNovaTranslateMaterial(network(node("ND_standard_surface_surfaceshader_100")));
    REQUIRE(tr.ok);
    r = record(tr);
    CHECK(r.resolved.f(sf::baseWeight) == 0.8f); // 1.0.0: base 0.8, base_color 1
    CHECK(r.resolved.c(sf::baseColor).x == 1.0f);

    HdMaterialNode2 pbr = node("ND_open_pbr_surface_surfaceshader");
    pbr.parameters[TfToken("subsurface_radius")] = VtValue(2.0f);
    pbr.parameters[TfToken("geometry_opacity")] = VtValue(0.5f);
    tr = HdNovaTranslateMaterial(network(pbr));
    REQUIRE(tr.ok);
    r = record(tr);
    CHECK(r.resolved.model == nova::SurfaceModel::OpenPBR);
    CHECK(r.resolved.f(sf::specularRoughness) == 0.3f);
    CHECK(r.resolved.f(sf::subsurfaceScale) == 2.0f); // OpenPBR subsurface_radius is the scale field
    CHECK(r.resolved.c(sf::opacity).z == 0.5f);       // a float opacity fills the color field
}

TEST_CASE("Values, connections and primvars translate", "[hydra][materials]") {
    HdMaterialNode2 surface = node("ND_standard_surface_surfaceshader");
    surface.parameters[TfToken("base_color")] = VtValue(GfVec3f(0.1f, 0.2f, 0.3f));
    surface.parameters[TfToken("thin_walled")] = VtValue(true);
    surface.parameters[TfToken("karma_unknown_property")] = VtValue(3.0f); // extra inputs are ignored
    surface.inputConnections[TfToken("specular_roughness")] = {{SdfPath("NodeGraph1/mask"), TfToken("out")}};
    HdNovaNetworkSource src = network(surface);
    HdMaterialNode2 mask = node("ND_geompropvalue_float");
    mask.parameters[TfToken("geomprop")] = VtValue(std::string("mask"));
    mask.parameters[TfToken("default")] = VtValue(0.75f);
    src.net.nodes[SdfPath("NodeGraph1/mask")] = mask;
    HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
    REQUIRE(tr.ok);
    CHECK(tr.warnings.empty());
    const nova::shading::ProgramPtr program = nova::shading::compile(tr.graph).program;
    REQUIRE(program);
    REQUIRE(program->primvars.size() == 1);
    CHECK(program->primvars[0] == "mask");
    nova::MaterialRecord r = record(tr);
    CHECK(r.varying);
    CHECK(r.resolved.c(sf::baseColor).y == 0.2f);
    CHECK(r.resolved.f(sf::thinWalled) == 1.0f);
}

TEST_CASE("Texture coordinates default to the st primvar", "[hydra][materials]") {
    HdMaterialNode2 surface = node("ND_surface_unlit");
    surface.inputConnections[TfToken("emission_color")] = {{SdfPath("ramp"), TfToken()}};
    HdNovaNetworkSource src = network(surface);
    src.net.nodes[SdfPath("ramp")] = node("ND_ramplr_color3"); // texcoord: defaultgeomprop UV0
    HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
    REQUIRE(tr.ok);
    const nova::shading::ProgramPtr program = nova::shading::compile(tr.graph).program;
    REQUIRE(program);
    REQUIRE(program->primvars.size() == 1);
    // And on geometry without it to the parameters of its faces, as in Karma (attributes.h).
    CHECK(program->primvars[0] == std::string("st|") + nova::kFaceParametersName);
}

TEST_CASE("USD ids, legacy names and unknown nodes", "[hydra][materials]") {
    // A UsdPrimvarReader_float2 resolves to its MaterialX definition.
    HdMaterialNode2 surface = node("ND_surface_unlit");
    surface.inputConnections[TfToken("emission")] = {{SdfPath("reader"), TfToken("result")}};
    HdNovaNetworkSource src = network(surface);
    HdMaterialNode2 reader = node("UsdPrimvarReader_float");
    reader.parameters[TfToken("varname")] = VtValue(TfToken("weight"));
    src.net.nodes[SdfPath("reader")] = reader;
    HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
    REQUIRE(tr.ok);
    CHECK(tr.warnings.empty());
    const nova::shading::ProgramPtr program = nova::shading::compile(tr.graph).program;
    REQUIRE(program);
    REQUIRE(program->primvars.size() == 1);
    CHECK(program->primvars[0] == "weight");

    // An unknown kma_* node: a warning, and the input falls back to 0.
    surface.inputConnections[TfToken("emission")] = {{SdfPath("fake"), TfToken("out")}};
    src = network(surface);
    src.net.nodes[SdfPath("fake")] = node("kma_rayimport");
    tr = HdNovaTranslateMaterial(src);
    REQUIRE(tr.ok);
    REQUIRE(tr.warnings.size() == 1);
    CHECK(tr.warnings[0].find("kma_rayimport") != std::string::npos);
    CHECK(record(tr).resolved.f(sf::emissionWeight) == 0.0f);

    // Karma Hair is a native model (M9).
    tr = HdNovaTranslateMaterial(network(node("kma_hair")));
    REQUIRE(tr.ok);
    CHECK(record(tr).resolved.model == nova::SurfaceModel::Hair);

    // An unsupported surface terminal fails, so the next context is tried.
    tr = HdNovaTranslateMaterial(network(node("kma_pathswitch")));
    CHECK_FALSE(tr.ok);
    CHECK_FALSE(tr.error.empty());
}

TEST_CASE("The structure hash follows the structure, not the values", "[hydra][materials]") {
    auto hashOf = [](float roughness, char const *geomprop, char const *maskName) {
        HdMaterialNode2 surface = node("ND_standard_surface_surfaceshader");
        surface.parameters[TfToken("specular_roughness")] = VtValue(roughness);
        surface.inputConnections[TfToken("base")] = {{SdfPath(maskName), TfToken()}};
        HdNovaNetworkSource src = network(surface);
        HdMaterialNode2 mask = node("ND_geompropvalue_float");
        mask.parameters[TfToken("geomprop")] = VtValue(std::string(geomprop));
        src.net.nodes[SdfPath(maskName)] = mask;
        HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
        REQUIRE(tr.ok);
        return nova::shading::compile(tr.graph).program->structureHash;
    };
    CHECK(hashOf(0.2f, "mask", "a") == hashOf(0.7f, "mask", "a")); // a value edit
    CHECK(hashOf(0.2f, "mask", "a") == hashOf(0.2f, "mask", "b")); // node names do not matter
    CHECK(hashOf(0.2f, "mask", "a") != hashOf(0.2f, "other", "a")); // a different primvar
}

// ---------------------------------------------------------------------------
// IPR through a render index
// ---------------------------------------------------------------------------

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

const char *kIprScene = R"(#usda 1.0
def Mesh "quad" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 3, 4, 1, 1, 4, 5, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]
    float[] primvars:mask = [0.25] (interpolation = "constant")
    float[] primvars:mask2 = [0.75] (interpolation = "constant")
    rel material:binding = </mat/a>
    uniform token subsetFamily:materialBind:familyType = "nonOverlapping"
    def GeomSubset "right" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform token elementType = "face"
        uniform token familyName = "materialBind"
        int[] indices = [1]
        rel material:binding = </mat/a>
    }
}
def Scope "mat"
{
    def Material "a"
    {
        token outputs:mtlx:surface.connect = </mat/a/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.1, 0.2, 0.3)
            float inputs:base.connect = </mat/a/mask.outputs:out>
            token outputs:out
        }
        def Shader "mask"
        {
            uniform token info:id = "ND_geompropvalue_float"
            string inputs:geomprop = "mask"
            float outputs:out
        }
        def Shader "unused"
        {
            uniform token info:id = "ND_add_float"
            float inputs:in1 = 1
            float outputs:out
        }
    }
    def Material "b"
    {
        token outputs:mtlx:surface.connect = </mat/b/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_surface_unlit"
            token outputs:out
        }
    }
}
)";

struct IprDriver {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    explicit IprDriver(const char *usda = kIprScene) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(usda);
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
    ~IprDriver() {
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
    HdNovaRenderParam &param() { return *static_cast<HdNovaRenderParam *>(delegate->GetRenderParam()); }
    nova::Scene &scene() { return param().GetScene(); }
    nova::MaterialId id(char const *path) { return param().GetMaterials().Acquire(SdfPath(path), scene()); }
    nova::MaterialRecord const &material(char const *path) { return scene().material(id(path)); }
    void set(char const *attr, VtValue const &v) { stage->GetAttributeAtPath(SdfPath(attr)).Set(v); }
};

} // namespace

TEST_CASE("Material edits in IPR", "[hydra][materials][ipr]") {
    IprDriver d;
    const nova::MeshId mesh = 0; // the only mesh
    REQUIRE(d.scene().meshUpdateCount() == 1);
    const nova::MaterialRecord &a = d.material("/mat/a");
    REQUIRE(a.desc.program);
    const nova::shading::ProgramPtr program = a.desc.program;
    CHECK(a.resolved.c(sf::baseColor).z == 0.3f);
    CHECK(d.scene().materialIdOf(mesh, 0) == d.id("/mat/a"));

    SECTION("a value edit keeps the program and the geometry") {
        const auto start = std::chrono::steady_clock::now();
        d.set("/mat/a/s.inputs:base_color", VtValue(GfVec3f(0.7f, 0.6f, 0.5f)));
        d.sync();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        nova::MaterialRecord const &edited = d.material("/mat/a");
        CHECK(edited.desc.program == program);
        CHECK(edited.resolved.c(sf::baseColor).x == 0.7f);
        CHECK(d.scene().meshUpdateCount() == 1);
        CHECK(ms < 100.0);
    }
    SECTION("a geomprop rename rebinds the attribute without touching the mesh") {
        const nova::AttributeRef *before = d.scene().attributeRefs(mesh, d.id("/mat/a"));
        REQUIRE(before);
        CHECK(before[0].data[0] == 0.25f);
        d.set("/mat/a/mask.inputs:geomprop", VtValue(std::string("mask2")));
        d.sync();
        CHECK(d.material("/mat/a").desc.program != program);
        const nova::AttributeRef *after = d.scene().attributeRefs(mesh, d.id("/mat/a"));
        REQUIRE(after);
        CHECK(after[0].source == nova::AttributeSource::Constant);
        CHECK(after[0].data[0] == 0.75f);
        CHECK(d.scene().meshUpdateCount() == 1);
    }
    SECTION("a GeomSubset rebind keeps the geometry") {
        d.stage->GetPrimAtPath(SdfPath("/quad/right")).GetRelationship(TfToken("material:binding"))
            .SetTargets({SdfPath("/mat/b")});
        d.sync();
        CHECK(d.scene().materialIdOf(mesh, 0) == d.id("/mat/a"));
        CHECK(d.scene().materialIdOf(mesh, 2) == d.id("/mat/b")); // face 1 = triangles 2 and 3
        CHECK(d.scene().meshUpdateCount() == 1);
    }
    SECTION("an edit on a node no terminal uses changes nothing") {
        const uint64_t version = d.param().GetEditVersion();
        d.set("/mat/a/unused.inputs:in1", VtValue(5.0f));
        d.sync();
        CHECK(d.param().GetEditVersion() == version);
    }
    SECTION("a removed material renders the default") {
        d.stage->RemovePrim(SdfPath("/mat/a"));
        d.sync();
        CHECK(d.material("/mat/a").desc.program == nova::defaultMaterial().program);
        CHECK(d.scene().meshUpdateCount() == 1);
    }
}

// M6.2: LPE tags from Karma's attributes: lights (inputs:karma:light:lpetag),
// objects (the constant primvar karma:object:lpetag) and materials
// (karma:lpetag on the surface shader, for objects without their own).
TEST_CASE("LPE tags reach the scene", "[hydra][materials][lpe]") {
    IprDriver d(R"(#usda 1.0
def Mesh "tagged" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    string primvars:karma:object:lpetag = "hero" (interpolation = "constant")
    rel material:binding = </mat/wall>
}
def Mesh "untagged" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 1), (1, 0, 1), (0, 1, 1)]
    rel material:binding = </mat/wall>
}
def RectLight "key"
{
    string inputs:karma:light:lpetag = "key"
}
def Scope "mat"
{
    def Material "wall"
    {
        token outputs:mtlx:surface.connect = </mat/wall/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            string inputs:karma:lpetag = "wall"
            token outputs:out
        }
    }
}
)");
    nova::Scene &scene = d.scene();
    std::vector<std::string> meshTags;
    for (nova::MeshId id = 0; id < scene.meshCount(); ++id)
        if (scene.meshRecord(id).alive)
            meshTags.push_back(scene.meshRecord(id).lpeTag);
    std::sort(meshTags.begin(), meshTags.end());
    CHECK(meshTags == std::vector<std::string>{"", "hero"});
    CHECK(d.material("/mat/wall").desc.lpeTag == "wall");
    REQUIRE(scene.lights().size() == 1);
    CHECK(scene.lightLpeTag(0) == "key");

    d.set("/tagged.primvars:karma:object:lpetag", VtValue(std::string("villain")));
    d.set("/key.inputs:karma:light:lpetag", VtValue(std::string("fill")));
    d.sync();
    meshTags.clear();
    for (nova::MeshId id = 0; id < scene.meshCount(); ++id)
        if (scene.meshRecord(id).alive)
            meshTags.push_back(scene.meshRecord(id).lpeTag);
    std::sort(meshTags.begin(), meshTags.end());
    CHECK(meshTags == std::vector<std::string>{"", "villain"});
    CHECK(scene.lightLpeTag(0) == "fill");
}

// M3.7 node coverage: every node definition with a numeric output, wired
// into an unlit surface, lowers (natively or through its nodegraph, all the
// way down) with no unknown or unsupported node and compiles within the row
// cap. Definitions that wait for a later step are listed with the step.
namespace {

struct ViewCamera final : HdCamera {
    ViewCamera() : HdCamera(SdfPath("/viewcam")) {
        _projection = Perspective;
        _horizontalAperture = 36.0f;
        _verticalAperture = 24.0f;
        _focalLength = 35.0f;
        _clippingRange = GfRange1f(0.1f, 1000.0f);
        _transform = GfMatrix4d(1.0).SetTranslate(GfVec3d(0.0, 0.0, 10.0));
    }
};

// A binding as Solaris makes it from a render var (the settings husk passes too).
HdRenderPassAovBinding renderVar(char const *name, char const *type, char const *source, HdRenderBuffer *buffer) {
    HdRenderPassAovBinding b;
    b.aovName = TfToken(name);
    b.aovSettings[TfToken("sourceType")] = VtValue(TfToken(type));
    b.aovSettings[TfToken("sourceName")] = VtValue(std::string(source));
    b.renderBuffer = buffer;
    b.clearValue = VtValue(0.0f);
    return b;
}

} // namespace

// M6.6: the viewport's "Viewing output" menu (not scriptable in Houdini 22,
// docs/design/M6.md) rebinds the pass to the chosen render var while the
// render runs. Here the same switch: beauty, a component (C<RD>L), a utility
// AOV (N), the beauty again, each converging to its own values.
TEST_CASE("The viewport switches between the beauty and AOVs", "[hydra][aov][ipr]") {
    IprDriver d(R"(#usda 1.0
def Mesh "wall" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-20, -20, 0), (20, -20, 0), (20, 20, 0), (-20, 20, 0)]
    rel material:binding = </mat/grey>
}
def DomeLight "sky"
{
    float inputs:intensity = 1
}
def Scope "mat"
{
    def Material "grey"
    {
        token outputs:mtlx:surface.connect = </mat/grey/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:specular = 0
            token outputs:out
        }
    }
}
)");
    d.delegate->SetRenderSetting(HdNovaSettingsTokens->samples, VtValue(16));
    ViewCamera camera;
    auto state = std::make_shared<HdRenderPassState>();
    state->SetCamera(&camera);
    state->SetViewport(GfVec4d(0, 0, 32, 32));
    std::vector<HdNovaRenderBuffer *> buffers;
    const auto buffer = [&](HdFormat format) {
        auto *b = static_cast<HdNovaRenderBuffer *>(
            d.delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, SdfPath("/rb" + std::to_string(buffers.size()))));
        b->Allocate(GfVec3i(32, 32, 1), format, false);
        buffers.push_back(b);
        return b;
    };
    HdNovaRenderBuffer *depth = buffer(HdFormatFloat32);
    HdRenderPassAovBinding depthBinding;
    depthBinding.aovName = HdAovTokens->depth;
    depthBinding.renderBuffer = depth;
    depthBinding.clearValue = VtValue(1.0f);
    // Renders until converged; the centre pixel of the first binding.
    const auto render = [&](HdRenderPassAovBinding const &shown, int channels) {
        state->SetAovBindings({shown, depthBinding});
        const auto start = std::chrono::steady_clock::now();
        do
            d.pass->Execute(state, TfTokenVector());
        while (!d.pass->IsConverged() &&
               std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 30.0);
        REQUIRE(d.pass->IsConverged());
        const float *v = static_cast<const float *>(shown.renderBuffer->Map()) + (16 * 32 + 16) * channels;
        const GfVec3f centre(v[0], v[1], v[2]);
        shown.renderBuffer->Unmap();
        return centre;
    };
    HdRenderPassAovBinding color;
    color.aovName = HdAovTokens->color;
    color.renderBuffer = buffer(HdFormatFloat32Vec4);
    color.clearValue = VtValue(GfVec4f(0.0f));

    const GfVec3f beauty = render(color, 4);
    CHECK_THAT(beauty[0], WithinAbs(0.8, 0.02)); // base 1 * base_color 0.8 under a white dome
    // One diffuse bounce to the dome is the whole beauty.
    const GfVec3f diffuse = render(renderVar("directdiffuse", "lpe", "C<RD>L", buffer(HdFormatFloat32Vec3)), 3);
    CHECK_THAT(diffuse[0], WithinAbs(beauty[0], 1e-4));
    CHECK_THAT(diffuse[2], WithinAbs(beauty[2], 1e-4));
    const GfVec3f normal = render(renderVar("N", "raw", "ray:hitN", buffer(HdFormatFloat32Vec3)), 3);
    CHECK(normal == GfVec3f(0.0f, 0.0f, 1.0f));
    const GfVec3f again = render(color, 4);
    CHECK_THAT(again[0], WithinAbs(beauty[0], 1e-5));

    d.pass.reset();
    d.tasks.clear();
    for (HdNovaRenderBuffer *b : buffers) {
        b->Finalize(d.delegate->GetRenderParam());
        d.delegate->DestroyBprim(b);
    }
}

TEST_CASE("Every library node lowers", "[hydra][materials][coverage]") {
    static const std::map<std::string, char const *> deferred = {
        // M4.3: hex tiling
        {"hextiledimage", "M4"},
        {"hextilednormalmap", "M4"},
        // rolled back to proposals in MaterialX 1.39.2 (M3.md, deviation 12)
        {"curveadjust", "proposal"},
    };
    MaterialX::ConstDocumentPtr lib = HdNovaNodeDefDatabase::Get().GetLibrary();
    REQUIRE(lib->getNodeDefs().size() > 500);
    auto numeric = [](std::string const &t) {
        return t == "float" || t == "integer" || t == "boolean" || t == "vector2" || t == "vector3" ||
               t == "color3" || t == "vector4" || t == "color4";
    };
    int tested = 0, waiting = 0;
    uint32_t maxRows = 0, maxUniforms = 0;
    std::string worstRows, worstUniforms;
    std::vector<std::string> failures;
    for (MaterialX::NodeDefPtr const &nd : lib->getNodeDefs()) {
        if (nd->getName().rfind("kma_", 0) == 0)
            continue; // Karma's registry nodes (M4): test_hydra_karma.cpp
        MaterialX::OutputPtr out;
        for (MaterialX::OutputPtr const &o : nd->getActiveOutputs())
            if (numeric(o->getType())) {
                out = o;
                break;
            }
        if (!out)
            continue; // shaders, closures, strings, matrices (matrices through their consumers)
        HdMaterialNode2 surface = node("ND_surface_unlit");
        surface.inputConnections[TfToken("emission_color")] = {{SdfPath("probe"), TfToken(out->getName())}};
        HdNovaNetworkSource src = network(surface);
        src.net.nodes[SdfPath("probe")] = node(nd->getName().c_str());
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
        ++tested;
        std::string problem;
        if (!tr.ok) {
            problem = tr.error;
        } else if (!tr.warnings.empty()) {
            problem = tr.warnings.front();
        } else {
            const nova::shading::CompileResult r = nova::shading::compile(tr.graph);
            if (!r.program) {
                problem = "compile: " + r.error;
            } else {
                if (r.program->rowCount > maxRows) {
                    maxRows = r.program->rowCount;
                    worstRows = nd->getName();
                }
                if (r.program->uniformCount > maxUniforms) {
                    maxUniforms = r.program->uniformCount;
                    worstUniforms = nd->getName();
                }
            }
        }
        if (problem.empty())
            continue;
        if (deferred.count(nd->getNodeString())) {
            ++waiting;
            continue;
        }
        failures.push_back(nd->getName() + " [" + nd->getNodeString() + "]: " + problem);
    }
    std::string report;
    for (std::string const &f : failures)
        report += f + "\n";
    INFO(report);
    WARN("lowered " << tested - waiting << " of " << tested << " definitions (" << waiting
                    << " wait for later steps); most rows " << maxRows << " (" << worstRows << "), most uniforms "
                    << maxUniforms << " (" << worstUniforms << ")");
    CHECK(tested > 600);
    CHECK(failures.empty());
}

namespace {

// The emission color of an unlit surface fed by `probe`'s output.
nova::Spectrum probeColor(HdMaterialNode2 const &probe, char const *output = "out",
                          std::map<std::string, HdMaterialNode2> const &upstream = {}) {
    HdMaterialNode2 surface = node("ND_surface_unlit");
    surface.inputConnections[TfToken("emission_color")] = {{SdfPath("probe"), TfToken(output)}};
    HdNovaNetworkSource src = network(surface);
    src.net.nodes[SdfPath("probe")] = probe;
    for (auto const &[name, n] : upstream)
        src.net.nodes[SdfPath(name)] = n;
    const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
    REQUIRE(tr.ok);
    CHECK(tr.warnings.empty());
    return record(tr).resolved.c(sf::emissionColor);
}

void connect(HdMaterialNode2 &n, char const *input, char const *upstream, char const *output = "out") {
    n.inputConnections[TfToken(input)] = {{SdfPath(upstream), TfToken(output)}};
}

} // namespace

TEST_CASE("Matrix nodes follow MaterialX's row vectors", "[hydra][materials][coverage]") {
    // ACEScg to linear Rec.709 (a cmlib nodegraph: constant matrix33 and
    // transformmatrix).
    HdMaterialNode2 cs = node("ND_acescg_to_lin_rec709_color3");
    cs.parameters[TfToken("in")] = VtValue(GfVec3f(0.2f, 0.5f, 0.7f));
    const nova::Spectrum rec709 = probeColor(cs);
    CHECK_THAT(rec709.x, WithinAbs(1.705051f * 0.2f - 0.621792f * 0.5f - 0.083259f * 0.7f, 1e-5));
    CHECK_THAT(rec709.y, WithinAbs(-0.130256f * 0.2f + 1.140805f * 0.5f - 0.010548f * 0.7f, 1e-5));
    CHECK_THAT(rec709.z, WithinAbs(-0.024003f * 0.2f - 0.128969f * 0.5f + 1.152972f * 0.7f, 1e-5));

    // A user matrix44 (Gf uses row vectors as well): p M, p M M^-1 = p and
    // det M.
    GfMatrix4d M(1.0);
    M.SetRotate(GfRotation(GfVec3d(1, 2, 3), 37.0));
    M.SetTranslateOnly(GfVec3d(0.5, -1.0, 2.0));
    M = GfMatrix4d(1.0).SetScale(GfVec3d(2.0, 0.5, 1.5)) * M;
    const GfVec3d p(0.3, -0.7, 1.1);

    HdMaterialNode2 constant = node("ND_constant_matrix44");
    constant.parameters[TfToken("value")] = VtValue(M);
    HdMaterialNode2 xf = node("ND_transformmatrix_vector3M4");
    xf.parameters[TfToken("in")] = VtValue(GfVec3f(p));
    connect(xf, "mat", "m");
    const nova::Spectrum moved = probeColor(xf, "out", {{"m", constant}});
    const GfVec3d expect = M.Transform(p);
    CHECK_THAT(moved.x, WithinAbs(expect[0], 1e-5));
    CHECK_THAT(moved.y, WithinAbs(expect[1], 1e-5));
    CHECK_THAT(moved.z, WithinAbs(expect[2], 1e-5));

    HdMaterialNode2 inverse = node("ND_invertmatrix_matrix44");
    connect(inverse, "in", "m");
    HdMaterialNode2 product = node("ND_multiply_matrix44");
    connect(product, "in1", "m");
    connect(product, "in2", "inv");
    HdMaterialNode2 back = node("ND_transformmatrix_vector3M4");
    back.parameters[TfToken("in")] = VtValue(GfVec3f(p));
    connect(back, "mat", "prod");
    const nova::Spectrum same = probeColor(back, "out", {{"m", constant}, {"inv", inverse}, {"prod", product}});
    CHECK_THAT(same.x, WithinAbs(p[0], 1e-5));
    CHECK_THAT(same.y, WithinAbs(p[1], 1e-5));
    CHECK_THAT(same.z, WithinAbs(p[2], 1e-5));

    HdMaterialNode2 det = node("ND_determinant_matrix44");
    connect(det, "in", "m");
    CHECK_THAT(probeColor(det, "out", {{"m", constant}}).x, WithinAbs(M.GetDeterminant(), 1e-4));

    // matrix33: transpose, inverse and determinant against Gf.
    GfMatrix3d A(2.0, 1.0, 0.5, -1.0, 3.0, 0.25, 0.0, 0.75, 1.5);
    HdMaterialNode2 a = node("ND_constant_matrix33");
    a.parameters[TfToken("value")] = VtValue(A);
    HdMaterialNode2 t = node("ND_transpose_matrix33");
    connect(t, "in", "a");
    HdMaterialNode2 ti = node("ND_invertmatrix_matrix33");
    connect(ti, "in", "t");
    HdMaterialNode2 v = node("ND_transformmatrix_vector3");
    v.parameters[TfToken("in")] = VtValue(GfVec3f(1.0f, 2.0f, 3.0f));
    connect(v, "mat", "ti");
    const nova::Spectrum r = probeColor(v, "out", {{"a", a}, {"t", t}, {"ti", ti}});
    const GfVec3d e = GfVec3d(1.0, 2.0, 3.0) * A.GetTranspose().GetInverse();
    CHECK_THAT(r.x, WithinAbs(e[0], 1e-5));
    CHECK_THAT(r.y, WithinAbs(e[1], 1e-5));
    CHECK_THAT(r.z, WithinAbs(e[2], 1e-5));
    HdMaterialNode2 d3 = node("ND_determinant_matrix33");
    connect(d3, "in", "a");
    CHECK_THAT(probeColor(d3, "out", {{"a", a}}).x, WithinAbs(A.GetDeterminant(), 1e-5));
}

TEST_CASE("Legacy and pbrlib utility nodes", "[hydra][materials][coverage]") {
    HdMaterialNode2 sw = node("ND_swizzle_color3_color3");
    sw.parameters[TfToken("in")] = VtValue(GfVec3f(0.1f, 0.2f, 0.3f));
    sw.parameters[TfToken("channels")] = VtValue(std::string("bgr"));
    nova::Spectrum c = probeColor(sw);
    CHECK(c.x == 0.3f);
    CHECK(c.y == 0.2f);
    CHECK(c.z == 0.1f);
    HdMaterialNode2 splat = node("ND_swizzle_float_color3");
    splat.parameters[TfToken("in")] = VtValue(0.4f);
    splat.parameters[TfToken("channels")] = VtValue(std::string("r1r"));
    c = probeColor(splat);
    CHECK(c.x == 0.4f);
    CHECK(c.y == 1.0f);
    CHECK(c.z == 0.4f);

    HdMaterialNode2 at = node("ND_atan2_float_1_38"); // in1 is y
    at.parameters[TfToken("in1")] = VtValue(1.0f);
    at.parameters[TfToken("in2")] = VtValue(0.0f);
    CHECK_THAT(probeColor(at).x, WithinAbs(1.5707963, 1e-6));

    HdMaterialNode2 ra = node("ND_roughness_anisotropy");
    ra.parameters[TfToken("roughness")] = VtValue(0.5f);
    ra.parameters[TfToken("anisotropy")] = VtValue(0.5f);
    c = probeColor(ra);
    CHECK_THAT(c.x, WithinAbs(0.25 / std::sqrt(0.5), 1e-6));
    CHECK_THAT(c.y, WithinAbs(0.25 * std::sqrt(0.5), 1e-6));

    // artistic_ior at its defaults (gold), against the closed form.
    c = probeColor(node("ND_artistic_ior"), "ior");
    const double r = 0.944, g = 0.998;
    const double nmin = (1 - r) / (1 + r), nmax = (1 + std::sqrt(r)) / (1 - std::sqrt(r));
    const double n = nmax + (nmin - nmax) * g;
    CHECK_THAT(c.x, WithinAbs(n, 1e-4));
    c = probeColor(node("ND_artistic_ior"), "extinction");
    CHECK_THAT(c.x, WithinAbs(std::sqrt(((n + 1) * (n + 1) * r - (n - 1) * (n - 1)) / (1 - r)), 1e-3));

    // blackbody: Karma's, power-scaled (docs/design/M8.md D14): warm and
    // dim at 2000 K, about white and 23 bright at 6000 K (research H 4.4).
    HdMaterialNode2 bb = node("ND_blackbody");
    bb.parameters[TfToken("temperature")] = VtValue(2000.0f);
    c = probeColor(bb);
    CHECK(c.x > c.y);
    CHECK(c.y > c.z);
    CHECK_THAT(0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z, WithinRel(0.2758 / 0.96, 0.05));
    bb.parameters[TfToken("temperature")] = VtValue(6000.0f);
    c = probeColor(bb);
    CHECK_THAT(0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z, WithinRel(22.81 / 0.97, 0.05));
    CHECK_THAT(c.x / c.z, WithinAbs(1.15, 0.05));

    c = probeColor(node("ND_chiang_hair_roughness"), "roughness_TT");
    const double lr = 0.1, ar = 0.2;
    const double v = std::pow(0.726 * lr + 0.812 * lr * lr + 3.7 * std::pow(lr, 20.0), 2.0);
    CHECK_THAT(c.x, WithinAbs(v * 0.25, 1e-6));
    CHECK_THAT(c.y, WithinAbs(0.265 * ar + 1.194 * ar * ar + 5.372 * std::pow(ar, 22.0), 1e-6));
}

TEST_CASE("Enum inputs authored by name take their enum values", "[hydra][materials]") {
    // USD declares UsdPreviewSurface's opacityMode as a token, MaterialX as an
    // integer with enum="transparent,presence" enumvalues="0,1".
    auto translate = [](VtValue const &mode) {
        HdMaterialNode2 surface = node("UsdPreviewSurface");
        surface.parameters[TfToken("opacity")] = VtValue(0.5f);
        surface.parameters[TfToken("opacityMode")] = mode;
        HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(network(surface));
        REQUIRE(tr.ok);
        CHECK(tr.warnings.empty());
        return record(tr);
    };
    for (VtValue const &presence : {VtValue(TfToken("presence")), VtValue(std::string("presence")), VtValue(1)}) {
        const nova::MaterialRecord r = translate(presence);
        CHECK(r.resolved.c(sf::opacity).x == 0.5f); // fractional presence
        CHECK(r.resolved.f(sf::transmissionWeight) == 0.0f);
    }
    for (VtValue const &transparent : {VtValue(TfToken("transparent")), VtValue(TfToken("unknown")), VtValue(0)}) {
        const nova::MaterialRecord r = translate(transparent);
        CHECK(r.resolved.c(sf::opacity).x == 1.0f); // present, refracting half
        CHECK(r.resolved.f(sf::transmissionWeight) == 0.5f);
    }
}

TEST_CASE("Material AOVs stay behind the native segments of a surface mix", "[hydra][materials][aov]") {
    // Two native Standard Surfaces in a mix: each reserves a whole layout,
    // of which only the mapped inputs are outputs. An AOV in the unmapped
    // tail (houdiniPrincipled) would change the beauty.
    HdMaterialNode2 mix = node("ND_mix_surfaceshader");
    mix.inputConnections[TfToken("fg")] = {{SdfPath("fg"), TfToken("out")}};
    mix.inputConnections[TfToken("bg")] = {{SdfPath("bg"), TfToken("out")}};
    mix.inputConnections[TfToken("aov:foo")] = {{SdfPath("tint"), TfToken("out")}};
    HdNovaNetworkSource src = network(mix);
    HdMaterialNode2 metal = node("ND_standard_surface_surfaceshader");
    metal.parameters[TfToken("metalness")] = VtValue(1.0f);
    src.net.nodes[SdfPath("fg")] = node("ND_standard_surface_surfaceshader");
    src.net.nodes[SdfPath("bg")] = metal;
    HdMaterialNode2 tint = node("ND_constant_color3");
    tint.parameters[TfToken("value")] = VtValue(GfVec3f(1.0f, 1.0f, 1.0f));
    src.net.nodes[SdfPath("tint")] = tint;
    const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
    REQUIRE(tr.ok);
    REQUIRE(tr.graph.closure);
    REQUIRE(tr.graph.aovs.size() == 1);
    const nova::MaterialRecord r = record(tr);
    int native = 0;
    for (int s = 0; s < tr.graph.closure->segmentCount; ++s) {
        nova::GraphSegment const &seg = tr.graph.closure->segment[s];
        if (seg.model == nova::SurfaceModel::Graph)
            continue;
        ++native;
        CHECK(int(tr.graph.aovs[0].second) >= int(seg.base) + int(sf::kCount));
        CHECK(r.resolved.f(uint16_t(seg.base + sf::houdiniPrincipled)) == 0.0f);
        CHECK(r.resolved.f(uint16_t(seg.base + sf::useSpecularWorkflow)) == 0.0f);
    }
    CHECK(native == 2);
}

TEST_CASE("String and filename dot and constant nodes pass their value through", "[hydra][materials]") {
    for (char const *id : {"ND_constant_filename", "ND_dot_filename"}) {
        INFO(id);
        const bool dot = std::string(id) == "ND_dot_filename";
        HdMaterialNode2 surface = node("ND_surface_unlit");
        surface.inputConnections[TfToken("emission_color")] = {{SdfPath("image"), TfToken("out")}};
        HdNovaNetworkSource src = network(surface);
        HdMaterialNode2 image = node("ND_image_color3");
        image.inputConnections[TfToken("file")] = {{SdfPath("path"), TfToken("out")}};
        src.net.nodes[SdfPath("image")] = image;
        HdMaterialNode2 path = node(id);
        path.parameters[TfToken(dot ? "in" : "value")] = VtValue(SdfAssetPath("tex/a.png", "C:/tex/a.png"));
        src.net.nodes[SdfPath("path")] = path;
        src.meta[SdfPath("path")][TfToken(dot ? "in" : "value")].colorSpace = TfToken("srgb_texture");
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
        REQUIRE(tr.ok);
        CHECK(tr.warnings.empty());
        REQUIRE(tr.textures.size() == 1);
        CHECK(tr.textures[0].path == "C:/tex/a.png");
        CHECK(tr.textures[0].colorSpace == "srgb_texture");
    }
}

namespace {

// The Principled Shader's surface (and displacement) on one node each, as
// Hydra delivers them.
HdNovaNetworkSource principled(std::map<TfToken, VtValue> params, bool displacement = false) {
    HdNovaNetworkSource src;
    src.context = TfToken("vex");
    for (int k = 0; k < (displacement ? 2 : 1); ++k) {
        const SdfPath shader(k ? "principled_displacement" : "principled_surface");
        src.net.nodes[shader].parameters = params;
        src.net.terminals[k ? HdMaterialTerminalTokens->displacement : HdMaterialTerminalTokens->surface] = {
            shader, TfToken(k ? "displacement" : "surface")};
        VtDictionary info;
        info["implementationSource"] = VtValue(TfToken("sourceAsset"));
        info["sourceAsset"] = VtValue(SdfAssetPath(k ? "opdef:/Vop/principledshader::2.0?DisplacementVexCode"
                                                     : "opdef:/Vop/principledshader::2.0?SurfaceVexCode"));
        src.nodeTypeInfo[shader] = info;
    }
    return src;
}

HdMaterialNode2 const *upstream(HdNovaNetworkSource const &net, HdMaterialNode2 const &n, char const *input) {
    const auto c = n.inputConnections.find(TfToken(input));
    return c != n.inputConnections.end() && !c->second.empty() ? &net.net.nodes.at(c->second.front().upstreamNode)
                                                               : nullptr;
}

} // namespace

TEST_CASE("Principled Shader decal wraps, vector displacement and normal space", "[hydra][materials][vex]") {
    std::vector<std::string> warnings;
    HdNovaNetworkSource out;
    SECTION("decal is a black border") {
        REQUIRE(HdNovaRewritePrincipled(principled({{TfToken("rough_useTexture"), VtValue(1)},
                                                    {TfToken("rough_texture"), VtValue(SdfAssetPath("r.png", "C:/r.png"))},
                                                    {TfToken("rough_textureWrap"), VtValue(std::string("decal"))}}),
                                        &out, &warnings));
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(out);
        REQUIRE(tr.ok);
        REQUIRE(tr.graph.textures.size() == 1);
        CHECK(tr.graph.textures[0].wrapS == nova::TexWrap::Black);
        CHECK(tr.graph.textures[0].wrapT == nova::TexWrap::Black);
    }
    SECTION("vector displacement: texel * scale, no offset, channel order") {
        for (char const *order : {"xyz", "xzy"}) {
            INFO(order);
            out = HdNovaNetworkSource{};
            warnings.clear();
            REQUIRE(HdNovaRewritePrincipled(
                principled({{TfToken("dispTex_enable"), VtValue(1)},
                            {TfToken("dispTex_type"), VtValue(std::string("vectordisp"))},
                            {TfToken("dispTex_texture"), VtValue(SdfAssetPath("v.exr", "C:/v.exr"))},
                            {TfToken("dispTex_channelOrder"), VtValue(std::string(order))},
                            {TfToken("dispTex_scale"), VtValue(0.25f)}},
                           true),
                &out, &warnings));
            CHECK(warnings.empty());
            const auto t = out.net.terminals.find(HdMaterialTerminalTokens->displacement);
            REQUIRE(t != out.net.terminals.end());
            HdMaterialNode2 const &d = out.net.nodes.at(t->second.upstreamNode);
            CHECK(d.nodeTypeId == TfToken("ND_displacement_vector3"));
            HdMaterialNode2 const *scaled = upstream(out, d, "displacement");
            REQUIRE(scaled);
            CHECK(scaled->nodeTypeId == TfToken("ND_multiply_vector3FA"));
            CHECK(scaled->parameters.at(TfToken("in2")) == VtValue(0.25f));
            HdMaterialNode2 const *texel = upstream(out, *scaled, "in1");
            REQUIRE(texel);
            if (std::string(order) == "xzy") {
                REQUIRE(texel->nodeTypeId == TfToken("ND_combine3_vector3"));
                int k = 0;
                for (char const *in : {"in1", "in2", "in3"}) {
                    HdMaterialNode2 const *e = upstream(out, *texel, in);
                    REQUIRE(e);
                    CHECK(e->nodeTypeId == TfToken("ND_extract_vector3"));
                    CHECK(e->parameters.at(TfToken("index")) == VtValue(k == 0 ? 0 : k == 1 ? 2 : 1));
                    ++k;
                }
            } else {
                CHECK(texel->nodeTypeId == TfToken("ND_convert_color3_vector3"));
            }
            for (auto const &[path, n] : out.net.nodes)
                CHECK(n.nodeTypeId != TfToken("ND_add_color3")); // no offset
        }
    }
    SECTION("normal space -1 to 1 undoes the normal map's 2x - 1") {
        auto normalIn = [&](int space) {
            out = HdNovaNetworkSource{};
            REQUIRE(HdNovaRewritePrincipled(
                principled({{TfToken("baseBumpAndNormal_enable"), VtValue(1)},
                            {TfToken("baseNormal_texture"), VtValue(SdfAssetPath("n.exr", "C:/n.exr"))},
                            {TfToken("baseNormal_space"), VtValue(space)}}),
                &out, &warnings));
            HdMaterialNode2 const &ss = out.net.nodes.at(out.net.terminals.at(HdMaterialTerminalTokens->surface).upstreamNode);
            HdMaterialNode2 const *map = upstream(out, ss, "normal");
            REQUIRE(map);
            REQUIRE(map->nodeTypeId == TfToken("ND_normalmap_float"));
            HdMaterialNode2 const *convert = upstream(out, *map, "in");
            REQUIRE(convert);
            HdMaterialNode2 const *rgb = upstream(out, *convert, "in");
            REQUIRE(rgb);
            return rgb;
        };
        CHECK(normalIn(0)->nodeTypeId == TfToken("ND_convert_color4_color3")); // the texel as it is
        HdMaterialNode2 const *add = normalIn(1);
        REQUIRE(add->nodeTypeId == TfToken("ND_add_color3"));
        CHECK(add->parameters.at(TfToken("in2")) == VtValue(GfVec3f(0.5f)));
        HdMaterialNode2 const *half = upstream(out, *add, "in1");
        REQUIRE(half);
        CHECK(half->nodeTypeId == TfToken("ND_multiply_color3FA"));
        CHECK(half->parameters.at(TfToken("in2")) == VtValue(0.5f));
        CHECK(warnings.empty());
    }
}
