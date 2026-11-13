// Stage features that Nova did not apply, or not as Karma does
// (docs/bug-audit-2026-11.md, section 16).
//
// Render passes (husk --pass, the pass menu of the Solaris viewport): the
// host names the active UsdRenderPass in the scene globals, and its
// collections are the renderer's to apply. Nova applied none: every pass
// rendered the whole scene. HdNovaRenderPassSceneIndex applies prune,
// renderVisibility, cameraVisibility and matte.
//
// DomeLight_1's pole axis: Karma turns the dome to the axis after the light's
// own transform; Nova turned it before, so a rotated dome on a z-up stage
// pointed elsewhere.
//
// Cylinders, cones and capsules: they were the ten-sided cage UsdImaging makes
// of them. HdNovaImplicitSceneIndex makes them meshes of 96 sides at their
// size, with the generators' normals.

#include "hydra/implicitSceneIndex.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"
#include "hydra/renderPassSceneIndex.h"

#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/sceneGlobalsSceneIndex.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

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

// A stage behind scene globals and Nova's render pass index, synced into
// the delegate's scene. `first` is the pass that is active from the start.
struct Driver {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdsiSceneGlobalsSceneIndexRefPtr globals;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    explicit Driver(std::string const &body, char const *first = nullptr) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        REQUIRE(layer->ImportFromString("#usda 1.0\n" + body));
        stage = UsdStage::Open(layer);
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        globals = HdsiSceneGlobalsSceneIndex::New(sceneIndices.finalSceneIndex);
        if (first)
            globals->SetActiveRenderPassPrimPath(SdfPath(first));
        index->InsertSceneIndex(HdNovaImplicitSceneIndex::New(HdNovaRenderPassSceneIndex::New(globals)),
                                SdfPath::AbsoluteRootPath(), false);
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
        scene().commit();
    }
    void activate(char const *path) {
        globals->SetActiveRenderPassPrimPath(path ? SdfPath(path) : SdfPath());
        sync();
    }
    nova::Scene &scene() { return static_cast<HdNovaRenderParam *>(delegate->GetRenderParam())->GetScene(); }
    nova::LightData const *dome() {
        for (nova::LightData const &l : scene().lights())
            if (l.type == nova::LightType::Dome)
                return &l;
        return nullptr;
    }
    bool has(char const *path) { return index->GetRprim(SdfPath(path)) != nullptr; }
    bool shown(char const *path) { return has(path) && index->GetRprim(SdfPath(path))->IsVisible(); }
    // The live mesh record whose user id belongs to `path`'s rprim.
    nova::MeshRecord const &record(char const *path) {
        const int id = has(path) ? index->GetRprim(SdfPath(path))->GetPrimId() : -2;
        for (uint32_t m = 0; m < scene().meshCount(); ++m)
            if (scene().meshRecord(m).alive && scene().meshRecord(m).desc.userId == id)
                return scene().meshRecord(m);
        FAIL("no mesh for " << path);
        static nova::MeshRecord none;
        return none;
    }
};

std::string mesh(char const *name, char const *extra = "") {
    return std::string("def Mesh \"") + name + R"("
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uniform token subdivisionScheme = "none"
)" + extra + "\n}\n";
}

// Meshes a to d (d invisible to shadow rays by its own setting, e inside a
// group) and the passes.
const std::string kStage =
    mesh("a") + mesh("b") + mesh("c") +
    mesh("d", "    string primvars:karma:object:rendervisibility = \"-shadow\" (interpolation = \"constant\")") +
    "def Xform \"group\"\n{\n" + mesh("e") + "}\n" + R"(
def Scope "Render"
{
    def RenderPass "prune"
    {
        rel collection:prune:includes = [</b>, </group>]
    }
    def RenderPass "camera"
    {
        uniform bool collection:cameraVisibility:includeRoot = 1
        rel collection:cameraVisibility:excludes = [</c>, </d>]
    }
    def RenderPass "visible"
    {
        uniform bool collection:renderVisibility:includeRoot = 0
        rel collection:renderVisibility:includes = [</b>, </group>]
    }
    def RenderPass "matte"
    {
        rel collection:matte:includes = </a>
    }
    def RenderPass "all"
    {
        rel collection:prune:includes = </b>
        rel collection:matte:includes = [</a>, </group>]
        uniform bool collection:cameraVisibility:includeRoot = 1
        rel collection:cameraVisibility:excludes = [</c>, </d>]
        uniform bool collection:renderVisibility:includeRoot = 1
        rel collection:renderVisibility:excludes = </c>
    }
    def RenderPass "empty"
    {
    }
}
)";

const uint8_t kNoCamera = nova::kVisibleAll & ~nova::kVisibleCamera;

void checkWhole(Driver &d) {
    for (char const *path : {"/a", "/b", "/c", "/d", "/group/e"}) {
        INFO(path);
        CHECK(d.shown(path));
        CHECK(d.record(path).holdout == nova::HoldoutMode::None);
    }
    CHECK(d.record("/a").visibility == nova::kVisibleAll);
    CHECK(d.record("/c").visibility == nova::kVisibleAll);
    CHECK(d.record("/d").visibility == (nova::kVisibleAll & ~nova::kVisibleShadow));
}

} // namespace

TEST_CASE("Without an active render pass the scene is whole", "[hydra][pass]") {
    Driver d(kStage);
    checkWhole(d);
    // A pass that says nothing, and a path that names no pass.
    d.activate("/Render/empty");
    checkWhole(d);
    d.activate("/Render/none");
    checkWhole(d);
}

TEST_CASE("The active render pass's collections are applied", "[hydra][pass]") {
    SECTION("prune: what is in it is not in the scene") {
        Driver d(kStage, "/Render/prune");
        CHECK(!d.has("/b"));
        CHECK(!d.has("/group/e"));
        CHECK(d.shown("/a"));
        CHECK(d.record("/a").visibility == nova::kVisibleAll);
    }
    SECTION("cameraVisibility: what is outside it is hidden from camera rays alone") {
        Driver d(kStage, "/Render/camera");
        CHECK(d.shown("/c"));
        CHECK(d.record("/c").visibility == kNoCamera);
        // Its own expression stays: neither shadow nor camera rays.
        CHECK(d.record("/d").visibility == (kNoCamera & ~nova::kVisibleShadow));
        CHECK(d.record("/a").visibility == nova::kVisibleAll);
        CHECK(d.record("/c").holdout == nova::HoldoutMode::None);
    }
    SECTION("renderVisibility: what is outside it is invisible") {
        Driver d(kStage, "/Render/visible");
        CHECK(!d.shown("/a"));
        CHECK(!d.shown("/c"));
        CHECK(d.shown("/b"));
        CHECK(d.shown("/group/e"));
        CHECK(d.record("/b").visibility == nova::kVisibleAll);
    }
    SECTION("matte: what is in it is a matte") {
        Driver d(kStage, "/Render/matte");
        CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);
        CHECK(d.record("/a").visibility == nova::kVisibleAll);
        CHECK(d.record("/b").holdout == nova::HoldoutMode::None);
    }
    SECTION("all four at once") {
        Driver d(kStage, "/Render/all");
        CHECK(!d.has("/b"));
        CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);
        CHECK(d.record("/group/e").holdout == nova::HoldoutMode::Matte);
        CHECK(!d.shown("/c"));
        CHECK(d.record("/d").visibility == (kNoCamera & ~nova::kVisibleShadow));
        CHECK(d.record("/d").holdout == nova::HoldoutMode::None);
    }
}

// The viewport changes the pass while the scene is up, and the pass's
// collections are edited like anything else on the stage.
TEST_CASE("Another pass, no pass and an edited pass change the scene that is up", "[hydra][pass]") {
    Driver d(kStage);
    checkWhole(d);

    d.activate("/Render/all");
    CHECK(!d.has("/b"));
    CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);
    CHECK(!d.shown("/c"));
    CHECK(d.record("/d").visibility == (kNoCamera & ~nova::kVisibleShadow));

    d.activate("/Render/camera");
    CHECK(d.has("/b"));
    CHECK(d.record("/a").holdout == nova::HoldoutMode::None);
    CHECK(d.shown("/c"));
    CHECK(d.record("/c").visibility == kNoCamera);

    d.activate("/Render/matte");
    CHECK(d.record("/c").visibility == nova::kVisibleAll);
    CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);
    CHECK(d.record("/c").holdout == nova::HoldoutMode::None);
    // The pass gets another member.
    d.stage->GetPrimAtPath(SdfPath("/Render/matte"))
        .GetRelationship(TfToken("collection:matte:includes"))
        .AddTarget(SdfPath("/c"));
    d.sync();
    CHECK(d.record("/c").holdout == nova::HoldoutMode::Matte);
    CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);

    d.activate(nullptr);
    checkWhole(d);
}

// A dome light on a stage with the up axis `up`: `type` with `pole` as its
// pole axis ("" for none), rotated by `rotate` degrees about x, y and z.
static std::string domeStage(char const *up, char const *type, char const *pole, char const *rotate) {
    std::string s = std::string("(\n    upAxis = \"") + up + "\"\n)\ndef " + type + " \"sky\"\n{\n";
    s += "    float inputs:intensity = 1\n";
    if (*pole)
        s += std::string("    uniform token poleAxis = \"") + pole + "\"\n";
    if (*rotate)
        s += std::string("    float3 xformOp:rotateXYZ = ") + rotate +
             "\n    uniform token[] xformOpOrder = [\"xformOp:rotateXYZ\"]\n";
    return s + "}\n";
}

static bool along(nova::Vec3f v, float x, float y, float z) {
    return std::fabs(v.x - x) < 1e-5f && std::fabs(v.y - y) < 1e-5f && std::fabs(v.z - z) < 1e-5f;
}

TEST_CASE("A DomeLight_1's pole axis turns the dome after the light's transform", "[hydra][dome]") {
    // The dome's pole is its +y. Without a rotation it is the axis named.
    {
        Driver d(domeStage("Y", "DomeLight", "", ""));
        REQUIRE(d.dome());
        CHECK(along(d.dome()->domeY, 0, 1, 0));
    }
    for (char const *up : {"Y", "Z"}) {
        INFO("up axis " << up);
        Driver y(domeStage(up, "DomeLight_1", "Y", "")), z(domeStage(up, "DomeLight_1", "Z", "")),
            scene(domeStage(up, "DomeLight_1", "scene", "")), classic(domeStage(up, "DomeLight", "", ""));
        REQUIRE((y.dome() && z.dome() && scene.dome() && classic.dome()));
        CHECK(along(y.dome()->domeY, 0, 1, 0));
        CHECK(along(z.dome()->domeY, 0, 0, 1));
        CHECK(along(scene.dome()->domeY, 0, *up == 'Y' ? 1.0f : 0.0f, *up == 'Z' ? 1.0f : 0.0f));
        CHECK(along(classic.dome()->domeY, 0, 1, 0)); // UsdLuxDomeLight has no pole axis
    }
    // A quarter turn about z takes +y to -x and +x to +y. With the pole at z the dome is then turned as the
    // y-up dome is, and stood up after it: its pole along -x, its +x along +z. (Turned to z first, the pole
    // would stay on z, the axis of the turn.)
    for (char const *up : {"Y", "Z"}) {
        INFO("up axis " << up);
        Driver z(domeStage(up, "DomeLight_1", "Z", "(0, 0, 90)"));
        REQUIRE(z.dome());
        CHECK(along(z.dome()->domeY, -1, 0, 0));
        CHECK(along(z.dome()->domeX, 0, 0, 1));
        Driver y(domeStage(up, "DomeLight_1", "Y", "(0, 0, 90)"));
        REQUIRE(y.dome());
        CHECK(along(y.dome()->domeY, -1, 0, 0));
        CHECK(along(y.dome()->domeX, 0, 1, 0));
    }
    {
        Driver scene(domeStage("Z", "DomeLight_1", "scene", "(0, 0, 90)"));
        REQUIRE(scene.dome());
        CHECK(along(scene.dome()->domeY, -1, 0, 0));
        CHECK(along(scene.dome()->domeX, 0, 0, 1));
    }
}

// What a mesh of the scene encloses and how far it reaches: its volume (the
// triangles face outward), the largest distance of a point from the axis
// `axis` and the largest coordinate along it.
struct Solid {
    double volume = 0.0;
    float radius = 0.0f, reach = 0.0f;
    size_t triangles = 0;
    bool unitNormals = true;
};

static Solid solidOf(nova::MeshRecord const &record, int axis) {
    nova::MeshDesc const &d = record.desc;
    Solid s;
    s.triangles = d.indices.size() / 3;
    for (size_t t = 0; t + 2 < d.indices.size(); t += 3) {
        const nova::Vec3f a = d.points[d.indices[t]], b = d.points[d.indices[t + 1]], c = d.points[d.indices[t + 2]];
        s.volume += double(nova::dot(a, nova::cross(b, c))) / 6.0;
    }
    for (size_t i = 0; i < d.pointCount(); ++i) {
        const nova::Vec3f p = d.points[i];
        const float along = axis == 0 ? p.x : axis == 1 ? p.y : p.z;
        s.reach = std::max(s.reach, std::fabs(along));
        s.radius = std::max(s.radius, std::sqrt(std::max(nova::dot(p, p) - along * along, 0.0f)));
    }
    s.unitNormals = d.normals.size() == d.pointCount();
    for (nova::Vec3f const &n : d.normals)
        s.unitNormals = s.unitNormals && std::fabs(nova::length(n) - 1.0f) < 1e-4f;
    return s;
}

TEST_CASE("Cylinders, cones and capsules are meshes of their size", "[hydra][implicit]") {
    const double pi = 3.14159265358979323846;
    Driver d(R"(
def Cylinder "cylinder"
{
    double radius = 1.5
    double height = 3
    uniform token axis = "X"
}
def Cone "cone"
{
    double radius = 1
    double height = 2
    uniform token axis = "Y"
}
def Capsule "capsule"
{
    double radius = 0.5
    double height = 1
    uniform token axis = "Z"
}
)");
    // The ten-sided cage encloses 93.5 % of a cylinder; 96 sides 99.93 %.
    const Solid cylinder = solidOf(d.record("/cylinder"), 0);
    CHECK(std::fabs(cylinder.volume) == Catch::Approx(pi * 1.5 * 1.5 * 3.0).epsilon(0.002));
    CHECK(cylinder.radius == Catch::Approx(1.5f).epsilon(1e-5));
    CHECK(cylinder.reach == Catch::Approx(1.5f).epsilon(1e-5));
    CHECK(cylinder.triangles >= 2 * HdNovaImplicitSceneIndex::kSides);
    CHECK(cylinder.unitNormals);

    const Solid cone = solidOf(d.record("/cone"), 1);
    CHECK(std::fabs(cone.volume) == Catch::Approx(pi * 2.0 / 3.0).epsilon(0.002));
    CHECK(cone.radius == Catch::Approx(1.0f).epsilon(1e-5));
    CHECK(cone.reach == Catch::Approx(1.0f).epsilon(1e-5));
    CHECK(cone.unitNormals);

    // A capsule's height is that of its cylinder: it reaches a radius beyond.
    const Solid capsule = solidOf(d.record("/capsule"), 2);
    CHECK(std::fabs(capsule.volume) == Catch::Approx(pi * 0.25 * 1.0 + 4.0 / 3.0 * pi * 0.125).epsilon(0.003));
    CHECK(capsule.radius == Catch::Approx(0.5f).epsilon(1e-5));
    CHECK(capsule.reach == Catch::Approx(1.0f).epsilon(1e-5));
    CHECK(capsule.unitNormals);

    // An edited parameter moves the mesh.
    d.stage->GetAttributeAtPath(SdfPath("/cylinder.radius")).Set(2.0);
    d.stage->GetAttributeAtPath(SdfPath("/capsule.height")).Set(3.0);
    d.sync();
    CHECK(solidOf(d.record("/cylinder"), 0).radius == Catch::Approx(2.0f).epsilon(1e-5));
    CHECK(solidOf(d.record("/capsule"), 2).reach == Catch::Approx(2.0f).epsilon(1e-5));
}

// UsdLux ShadowAPI (docs/bug-audit-2026-11.md, section 20): the attributes
// reach the lights; a light without them casts full shadows by every
// strategy.
TEST_CASE("A light's ShadowAPI settings reach the scene", "[hydra][lights][shadow]") {
    Driver d(R"(
def DistantLight "plain"
{
}

def DistantLight "off" (
    prepend apiSchemas = ["ShadowAPI"]
)
{
    bool inputs:shadow:enable = 0
}

def SphereLight "tinted" (
    prepend apiSchemas = ["ShadowAPI"]
)
{
    color3f inputs:shadow:color = (1, 0.25, -2)
    float inputs:shadow:distance = 3
    float inputs:shadow:falloff = 1.5
    float inputs:shadow:falloffGamma = 2
}

def DomeLight "white" (
    prepend apiSchemas = ["ShadowAPI"]
)
{
    color3f inputs:shadow:color = (1, 1, 4)
}
)");
    REQUIRE(d.scene().lights().size() == 4);
    int plain = 0, off = 0, tinted = 0, white = 0;
    for (nova::LightData const &l : d.scene().lights()) {
        if (l.type == nova::LightType::Sphere) {
            ++tinted;
            CHECK(l.castsShadows);
            CHECK(l.shadowTinted);
            CHECK(l.shadowShaped);
            CHECK(l.ownShadows());
            CHECK(l.neeOnly()); // a continuation ray would shadow it as any light
            CHECK(l.shadowColor.x == 1.0f);
            CHECK(l.shadowColor.y == 0.25f);
            CHECK(l.shadowColor.z == 0.0f); // a share of the light: 0 to 1
            CHECK(l.shadowDistance == 3.0f);
            CHECK(l.shadowFalloff == 1.5f);
            CHECK(l.shadowFalloffGamma == 2.0f);
        } else if (l.type == nova::LightType::Dome) {
            ++white; // a white shadow colour: no shadows
            CHECK_FALSE(l.castsShadows);
            CHECK_FALSE(l.shadowTinted);
            CHECK(l.ownShadows());
            CHECK_FALSE(l.neeOnly()); // a dome's BSDF samples are shadow rays of their own
        } else if (!l.castsShadows) {
            ++off;
            CHECK(l.shadowShaped);
            CHECK_FALSE(l.shadowTinted);
        } else {
            ++plain;
            CHECK_FALSE(l.shadowShaped);
            CHECK_FALSE(l.ownShadows());
            CHECK(l.shadowDistance <= 0.0f);
        }
    }
    CHECK(plain == 1);
    CHECK(off == 1);
    CHECK(tinted == 1);
    CHECK(white == 1);
    // The dome with shadow settings is among those whose BSDF samples are shadow rays.
    CHECK(d.scene().shadowLinkedDomes().size() == 1);

    // Shadows on again.
    d.stage->GetAttributeAtPath(SdfPath("/off.inputs:shadow:enable")).Set(true);
    d.sync();
    int shaped = 0;
    for (nova::LightData const &l : d.scene().lights())
        shaped += l.type == nova::LightType::Distant && l.shadowShaped;
    CHECK(shaped == 0);
}

// The ShapingAPI's IES profile (docs/bug-audit-2026-11.md, section 22): the
// file, its angle scale and its normalization reach the light's record; a
// file that is none leaves the light as it is, and a distant light has no
// profile.
TEST_CASE("A light's IES profile reaches the scene", "[hydra][lights][ies]") {
    const std::string file = std::string(NOVA_TEST_DIR) + "/data/ies/lobe.ies";
    Driver d(R"(
def SphereLight "profile" (
    prepend apiSchemas = ["ShapingAPI"]
)
{
    asset inputs:shaping:ies:file = @)" + file + R"(@
    float inputs:shaping:ies:angleScale = 0.5
    bool inputs:shaping:ies:normalize = 1
    float inputs:radius = 0.5
}

def RectLight "missing" (
    prepend apiSchemas = ["ShapingAPI"]
)
{
    asset inputs:shaping:ies:file = @)" + std::string(NOVA_TEST_DIR) + R"(/data/ies/none.ies@
}

def DistantLight "sun" (
    prepend apiSchemas = ["ShapingAPI"]
)
{
    asset inputs:shaping:ies:file = @)" + file + R"(@
}
)");
    REQUIRE(d.scene().lights().size() == 3);
    int profiles = 0;
    for (nova::LightData const &l : d.scene().lights()) {
        if (l.type != nova::LightType::Sphere) {
            CHECK(l.ies == nullptr);
            continue;
        }
        ++profiles;
        REQUIRE(l.ies != nullptr);
        CHECK(l.shaped);
        CHECK(l.iesHeight > 100);
        CHECK(l.iesWidth > 100); // the lobe's file has horizontal angles
        CHECK(l.iesAngleScale == 0.5f);
        CHECK(l.iesScale == Catch::Approx(1.0 / 144.87).epsilon(2e-3)); // Karma's mean of the file
        // The frame of the horizontal angle: the light's own X and Y.
        CHECK(l.iesX.x == Catch::Approx(1.0f));
        CHECK(l.iesY.y == Catch::Approx(1.0f));
        // Straight down its axis the file has its floor of 100.
        CHECK(nova::iesFactor(l, l.axis) == Catch::Approx(100.0 / 144.87).epsilon(3e-3));
    }
    CHECK(profiles == 1);

    // Without the normalization: the candela themselves.
    d.stage->GetAttributeAtPath(SdfPath("/profile.inputs:shaping:ies:normalize")).Set(false);
    d.sync();
    for (nova::LightData const &l : d.scene().lights())
        if (l.type == nova::LightType::Sphere) {
            REQUIRE(l.ies != nullptr);
            CHECK(l.iesScale == 1.0f);
            CHECK(nova::iesFactor(l, l.axis) == Catch::Approx(100.0).epsilon(1e-3));
        }
}

// Geometry without a material: Karma renders it see-through by its
// displayOpacity primvar (section 20). It has a default material of its own
// for that, so geometry without the primvar stays opaque to every ray.
TEST_CASE("Geometry without a material is see-through by its displayOpacity", "[hydra][materials][opacity]") {
    Driver d(mesh("plain") + mesh("veiled", "    float[] primvars:displayOpacity = [0.5]") +
             mesh("faces", "    float[] primvars:displayOpacity = [0.25] (interpolation = \"uniform\")") + R"(
def Material "matte"
{
}

def Mesh "bound" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uniform token subdivisionScheme = "none"
    float[] primvars:displayOpacity = [0.5]
    rel material:binding = </matte>
}
)");
    const nova::MeshRecord &plain = d.record("/plain");
    CHECK(plain.materialSlots.empty());
    CHECK_FALSE(plain.filtered);
    CHECK_FALSE(d.scene().material(nova::kDefaultMaterialId).hasOpacity);

    const nova::MeshRecord &veiled = d.record("/veiled");
    REQUIRE(veiled.materialSlots.size() == 1);
    const nova::MaterialId withOpacity = veiled.materialSlots[0];
    CHECK(withOpacity != nova::kDefaultMaterialId);
    CHECK(veiled.filtered);
    const nova::MaterialRecord &material = d.scene().material(withOpacity);
    CHECK(material.hasOpacity);
    REQUIRE(material.desc.program);
    CHECK(material.desc.program->opacity);
    // Karma's plastic, as the default material is.
    CHECK(material.resolved.model == d.scene().material(nova::kDefaultMaterialId).resolved.model);
    // One such material for all of them; the primvar goes to the mesh with its interpolation.
    const nova::MeshRecord &faces = d.record("/faces");
    REQUIRE(faces.materialSlots.size() == 1);
    CHECK(faces.materialSlots[0] == withOpacity);
    const nova::MeshAttribute *opacity = faces.desc.findAttribute("displayOpacity");
    REQUIRE(opacity);
    CHECK(opacity->rate == nova::AttributeRate::Uniform);
    // A prim with a material keeps it (here one that is not translated: the default look, opaque).
    const nova::MeshRecord &bound = d.record("/bound");
    REQUIRE(bound.materialSlots.size() == 1);
    CHECK(bound.materialSlots[0] != withOpacity);

    // The primvar gone: the plain default material again.
    d.stage->GetAttributeAtPath(SdfPath("/veiled.primvars:displayOpacity")).Block();
    d.sync();
    CHECK(d.record("/veiled").materialSlots.empty());
    CHECK_FALSE(d.record("/veiled").filtered);
    // And authored where there was none.
    d.stage->GetPrimAtPath(SdfPath("/plain"))
        .CreateAttribute(TfToken("primvars:displayOpacity"), SdfValueTypeNames->FloatArray)
        .Set(VtFloatArray{0.75f});
    d.sync();
    REQUIRE(d.record("/plain").materialSlots.size() == 1);
    CHECK(d.record("/plain").materialSlots[0] == withOpacity);
}
