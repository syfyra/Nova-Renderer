// M8.0 volumes through Hydra (docs/design/M8.md D3): a UsdVolVolume with
// OpenVDBAsset fields becomes one scene volume with the density field (the
// default material reads nothing else), placed by the volume's transform,
// with bounds from the grids; field, transform, visibility edits and removal
// reach the scene.

#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"
#include "hydra/volumeDependencies.h"

#include <pxr/imaging/hd/dependencyForwardingSceneIndex.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <pxr/usd/usd/relationship.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/integrator/volume_eval.h"

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

std::string pyroPath() { return TfStringReplace(std::string(NOVA_TEST_DIR) + "/data/volumes/pyro.vdb", "\\", "/"); }

// Volume materials (M8.1): a MaterialX volume whose scattering is the
// density field and emission the temperature field, and Karma's kma_volume
// with a density-driven scattering.
const char *kMaterials = R"(
def Scope "mat"
{
    def Material "fire"
    {
        token outputs:mtlx:volume.connect = </mat/fire/vol.outputs:out>
        def Shader "vol"
        {
            uniform token info:id = "ND_volume"
            token inputs:vdf.connect = </mat/fire/vdf.outputs:out>
            token inputs:edf.connect = </mat/fire/edf.outputs:out>
            token outputs:out
        }
        def Shader "vdf"
        {
            uniform token info:id = "ND_anisotropic_vdf"
            color3f inputs:scattering.connect = </mat/fire/dens.outputs:out>
            color3f inputs:absorption = (0.1, 0.1, 0.1)
            float inputs:anisotropy = 0.4
            token outputs:out
        }
        def Shader "dens"
        {
            uniform token info:id = "ND_geompropvalue_color3"
            string inputs:geomprop = "density"
            color3f outputs:out
        }
        def Shader "edf"
        {
            uniform token info:id = "ND_uniform_edf"
            color3f inputs:color.connect = </mat/fire/temp.outputs:out>
            token outputs:out
        }
        def Shader "temp"
        {
            uniform token info:id = "ND_geompropvalue_color3"
            string inputs:geomprop = "temperature"
            color3f outputs:out
        }
    }
    def Material "fog"
    {
        token outputs:mtlx:volume.connect = </mat/fog/vol.outputs:out>
        def Shader "vol"
        {
            uniform token info:id = "ND_volume"
            token inputs:vdf.connect = </mat/fog/vdf.outputs:out>
            token outputs:out
        }
        def Shader "vdf"
        {
            uniform token info:id = "ND_anisotropic_vdf"
            color3f inputs:scattering = (2, 2, 2)
            color3f inputs:absorption = (0, 0, 0)
            token outputs:out
        }
    }
    def Material "karma"
    {
        token outputs:kma:volume.connect = </mat/karma/vol.outputs:out>
        def Shader "vol"
        {
            uniform token info:id = "kma_volume"
            vector3f inputs:scattering.connect = </mat/karma/dens.outputs:out>
            float inputs:anisotropy = -0.2
            token outputs:out
        }
        def Shader "dens"
        {
            uniform token info:id = "ND_geompropvalue_vector3"
            string inputs:geomprop = "density"
            vector3f outputs:out
        }
    }
}
)";

std::string sceneText() {
    return R"(#usda 1.0
def Volume "smoke" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    float primvars:touch = 0 (interpolation = "constant")
    rel field:density = </smoke/density>
    rel field:temperature = </smoke/temperature>
    double3 xformOp:translate = (1, 2, 3)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    def OpenVDBAsset "density"
    {
        asset filePath = @)" +
           pyroPath() + R"(@
        token fieldName = "density"
    }
    def OpenVDBAsset "temperature"
    {
        asset filePath = @)" +
           pyroPath() + R"(@
        token fieldName = "temperature"
    }
}
)" + std::string(kMaterials);
}

struct Driver {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    // `linking`: with pxr's light-linking scene index, as an application has
    // it (volumes among its geometry types): it gives the prims of the lights'
    // collections their categories. The stage then comes after the chain
    // exists, as in husk, so that the index sees the prims arrive.
    explicit Driver(std::string const &text = sceneText(), bool linking = false) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(text);
        stage = UsdStage::Open(layer);
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        if (!linking)
            info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        // What Nova's scene index plugin adds for volumes (the test rig runs
        // no renderer plugins).
        HdSceneIndexBaseRefPtr si = HdDependencyForwardingSceneIndex::New(
            HdNovaVolumeDependencySceneIndex::New(sceneIndices.finalSceneIndex));
        if (linking) {
            using Tokens = HdRetainedTypedSampledDataSource<VtArray<TfToken>>;
            si = HdsiLightLinkingSceneIndex::New(
                si, HdRetainedContainerDataSource::New(
                        HdsiLightLinkingSceneIndexTokens->lightPrimTypes,
                        Tokens::New(VtArray<TfToken>{HdPrimTypeTokens->distantLight, HdPrimTypeTokens->sphereLight}),
                        HdsiLightLinkingSceneIndexTokens->lightFilterPrimTypes, Tokens::New(VtArray<TfToken>{}),
                        HdsiLightLinkingSceneIndexTokens->geometryPrimTypes,
                        Tokens::New(VtArray<TfToken>{HdPrimTypeTokens->mesh, HdPrimTypeTokens->volume})));
        }
        index->InsertSceneIndex(si, SdfPath::AbsoluteRootPath(), false);
        pass = delegate->CreateRenderPass(index.get(),
                                          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        tasks.push_back(std::make_shared<SyncTask>(pass));
        if (linking)
            sceneIndices.stageSceneIndex->SetStage(stage);
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
    HdNovaRenderParam &param() { return *static_cast<HdNovaRenderParam *>(delegate->GetRenderParam()); }
    nova::Scene &scene() { return param().GetScene(); }
    void set(char const *attr, VtValue const &v) { stage->GetAttributeAtPath(SdfPath(attr)).Set(v); }
    void bind(char const *material) {
        stage->GetPrimAtPath(SdfPath("/smoke"))
            .CreateRelationship(TfToken("material:binding"))
            .SetTargets({SdfPath(material)});
    }
    // What the render pass does for queued prims: make them sync again.
    void touch(float v) { set("/smoke.primvars:touch", VtValue(v)); }
};

} // namespace

TEST_CASE("Lights opt into equiangular sampling", "[hydra][volume][equiangular]") {
    // docs/design/M9.md D17: Karma's inputs:karma:light:equiangular (and
    // Nova's own) on a sphere light; off by default.
    Driver d;
    UsdPrim light = d.stage->DefinePrim(SdfPath("/key"), TfToken("SphereLight"));
    d.sync();
    REQUIRE(d.scene().lights().size() == 1);
    CHECK_FALSE(d.scene().lights()[0].equiangular);
    CHECK(d.scene().equiangularLights().table.count == 0);
    light.CreateAttribute(TfToken("inputs:karma:light:equiangular"), SdfValueTypeNames->Bool).Set(true);
    d.sync();
    REQUIRE(d.scene().lights().size() == 1);
    CHECK(d.scene().lights()[0].equiangular);
    CHECK(d.scene().equiangularLights().table.count == 1);
    light.CreateAttribute(TfToken("inputs:nova:light:equiangular"), SdfValueTypeNames->Bool).Set(false);
    d.sync();
    CHECK_FALSE(d.scene().lights()[0].equiangular); // Nova's own wins
}

TEST_CASE("Volume prims reach the scene with their density field", "[hydra][volume]") {
    Driver d;
    REQUIRE(d.scene().activeVolumes().size() == 1);
    const nova::VolumeId id = d.scene().activeVolumes()[0];
    const nova::VolumeRecord &rec = d.scene().volumeRecord(id);
    REQUIRE(rec.desc.fields.size() == 1); // temperature is not read by the default material
    CHECK(rec.desc.fields[0].name == "density");
    CHECK(rec.desc.fields[0].fieldName == "density");
    REQUIRE(rec.desc.fields[0].grid);

    // Bounds: the grid's active box (world space of the file), moved by the
    // volume's translate, within the majorant cells' extent.
    const nanovdb::FloatGrid &g = *rec.desc.fields[0].grid->grid();
    const nanovdb::BBox<nanovdb::Vec3d> box = g.worldBBox();
    const nova::Vec3f boxLo{float(box.min()[0]) + 1.0f, float(box.min()[1]) + 2.0f, float(box.min()[2]) + 3.0f};
    const nova::Vec3f boxHi{float(box.max()[0]) + 1.0f, float(box.max()[1]) + 2.0f, float(box.max()[2]) + 3.0f};
    CHECK(rec.lower.x <= boxLo.x);
    CHECK(rec.lower.y <= boxLo.y);
    CHECK(rec.lower.z <= boxLo.z);
    CHECK(rec.upper.x >= boxHi.x);
    CHECK(rec.upper.y >= boxHi.y);
    CHECK(rec.upper.z >= boxHi.z);
    // At most a cell (rounding up) and the one-voxel support beyond the box.
    const float slack = 0.05f * float(rec.layout.k + 2);
    CHECK(rec.lower.x > boxLo.x - slack);
    CHECK(rec.lower.y > boxLo.y - slack);
    CHECK(rec.upper.x < boxHi.x + slack);
    CHECK(rec.upper.y < boxHi.y + slack);

    SECTION("a transform edit moves the volume") {
        const float x = rec.lower.x;
        d.set("/smoke.xformOp:translate", VtValue(GfVec3d(4.0, 2.0, 3.0)));
        d.sync();
        CHECK_THAT(d.scene().volumeRecord(id).lower.x, WithinAbs(x + 3.0f, 1e-4));
    }
    SECTION("hiding the volume deactivates it") {
        UsdGeomImageable(d.stage->GetPrimAtPath(SdfPath("/smoke"))).MakeInvisible();
        d.sync();
        CHECK(d.scene().activeVolumes().empty());
        UsdGeomImageable(d.stage->GetPrimAtPath(SdfPath("/smoke"))).MakeVisible();
        d.sync();
        CHECK(d.scene().activeVolumes().size() == 1);
    }
    SECTION("a field that cannot be read leaves nothing to render") {
        d.set("/smoke/density.filePath", VtValue(SdfAssetPath(TfStringReplace(pyroPath(), "pyro.vdb", "missing.vdb"))));
        d.sync();
        CHECK(d.scene().activeVolumes().empty());
        d.set("/smoke/density.filePath", VtValue(SdfAssetPath(pyroPath())));
        d.sync();
        CHECK(d.scene().activeVolumes().size() == 1);
    }
    SECTION("removing the prim removes the volume") {
        d.stage->RemovePrim(SdfPath("/smoke"));
        d.sync();
        CHECK(d.scene().activeVolumes().empty());
    }
}

TEST_CASE("Volume materials read their fields and shade the volume", "[hydra][volume][material]") {
    Driver d;
    d.bind("/mat/fire");
    d.sync();
    REQUIRE(d.scene().activeVolumes().size() == 1);
    const nova::VolumeId id = d.scene().activeVolumes()[0];
    const nova::VolumeRecord &rec = d.scene().volumeRecord(id);
    REQUIRE(rec.program);
    CHECK(rec.desc.fields.size() == 2); // density and temperature
    CHECK(rec.estimatedCells == 0);

    // A point inside the pyro: the fields' values there (object space is the
    // files' world space; the volume's translate places it).
    const nova::VolumeField *density = rec.desc.findField("density");
    const nova::VolumeField *temperature = rec.desc.findField("temperature");
    REQUIRE(density);
    REQUIRE(temperature);
    const nova::Vec3f p{0.05f, 1.2f, 0.0f};
    nova::VolumeSampler sd(*density->grid->grid()), st(*temperature->grid->grid());
    const float dv = sd(nova::inverseAffine(density->indexToObject()).point(p));
    const float tv = st(nova::inverseAffine(temperature->indexToObject()).point(p));
    REQUIRE(dv > 0.1f);
    REQUIRE(tv > 0.1f);
    const auto at = [&] {
        nova::VolumeEvaluator eval(d.scene(), d.scene().activeVolumes()[0]);
        return eval(p, p + nova::Vec3f{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 1.0f}, 0.5f, 0, 0);
    };
    nova::VolumePoint v = at();
    CHECK_THAT(v.scattering.x, WithinAbs(dv, 1e-5));
    CHECK_THAT(v.absorption.y, WithinAbs(0.1, 1e-6));
    CHECK_THAT(v.emission.z, WithinAbs(tv, 1e-5));
    CHECK_THAT(v.g, WithinAbs(0.4, 1e-6));
    CHECK(d.param().GetMaterials().TakePendingMeshes().empty());

    SECTION("a value edit changes the shading, not the fields") {
        d.set("/mat/fire/vdf.inputs:absorption", VtValue(GfVec3f(0.7f, 0.7f, 0.7f)));
        d.sync();
        CHECK(d.param().GetMaterials().TakePendingMeshes().empty());
        CHECK(d.scene().volumeRecord(id).desc.fields.size() == 2);
        CHECK_THAT(at().absorption.x, WithinAbs(0.7, 1e-6));
    }
    SECTION("rewiring changes the fields a volume loads") {
        UsdAttribute color = d.stage->GetAttributeAtPath(SdfPath("/mat/fire/edf.inputs:color"));
        color.ClearConnections();
        color.Set(GfVec3f(1.0f, 0.0f, 0.0f));
        d.sync();
        const std::vector<SdfPath> pending = d.param().GetMaterials().TakePendingMeshes();
        CHECK(pending == std::vector<SdfPath>{SdfPath("/smoke")});
        d.touch(1.0f);
        d.sync();
        CHECK(d.scene().volumeRecord(id).desc.fields.size() == 1);
        CHECK_THAT(at().emission.x, WithinAbs(1.0, 1e-6));
    }
    SECTION("Karma's kma_volume") {
        d.bind("/mat/karma");
        d.sync();
        const nova::VolumeRecord &k = d.scene().volumeRecord(d.scene().activeVolumes()[0]);
        REQUIRE(k.program);
        CHECK(k.desc.fields.size() == 1);
        v = at();
        CHECK_THAT(v.scattering.y, WithinAbs(dv, 1e-5));
        CHECK(v.absorption.x == 0.0f);
        CHECK(v.emission.x == 0.0f);
        CHECK_THAT(v.g, WithinAbs(-0.2, 1e-6));
    }
}

// Audit V-3 (0.2.0): a volume material that reads no field (a constant
// medium) rendered nothing on a VDB volume; it fills the volume's density
// field, in the cells the field's voxels reach.
TEST_CASE("A constant volume material fills the volume's field", "[hydra][volume][material]") {
    Driver d;
    d.bind("/mat/fog");
    d.sync();
    REQUIRE(d.scene().activeVolumes().size() == 1);
    const nova::VolumeRecord &rec = d.scene().volumeRecord(d.scene().activeVolumes()[0]);
    REQUIRE_FALSE(rec.layout.empty());
    REQUIRE(rec.desc.fields.size() == 1); // the density, which the shader does not read
    size_t filled = 0, empty = 0;
    float most = 0.0f;
    for (float m : rec.majorant) {
        filled += m > 0.0f;
        empty += m == 0.0f;
        most = std::max(most, m);
    }
    CHECK(filled > 0);
    CHECK_THAT(most, WithinAbs(2.0, 1e-4)); // the constant scattering
    // The sparse smoke leaves cells of its box without voxels: nothing there.
    CHECK(empty > 0);
    // The medium itself, where the smoke is.
    const nova::Vec3f p{0.05f, 1.2f, 0.0f};
    nova::VolumeEvaluator eval(d.scene(), d.scene().activeVolumes()[0]);
    const nova::VolumePoint v = eval(p, p + nova::Vec3f{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 1.0f}, 0.5f, 0, 0);
    CHECK_THAT(v.scattering.x, WithinAbs(2.0, 1e-5));
}

// Audit V-5 (0.2.0): Houdini authors constant primvars as arrays of one.
TEST_CASE("A volume's settings as one-element arrays", "[hydra][volume][settings]") {
    Driver d(TfStringReplace(sceneText(), "float primvars:touch = 0 (interpolation = \"constant\")",
                             "float primvars:touch = 0 (interpolation = \"constant\")\n"
                             "    float[] primvars:karma:object:volumelimit = [4] (interpolation = \"constant\")"));
    REQUIRE(d.scene().activeVolumes().size() == 1);
    CHECK(d.scene().volumeRecord(d.scene().activeVolumes()[0]).desc.volumeLimit == 4.0f);
}

// Audit V-0 (0.2.0): an instanced Volume rendered once, at the prototype's
// own transform. It is one scene volume per instance.
TEST_CASE("Instanced volumes are a scene volume per instance", "[hydra][volume][instancing]") {
    const std::string volume = R"(
        rel field:density = </FIELD>
        def OpenVDBAsset "density"
        {
            asset filePath = @)" + pyroPath() + R"(@
            token fieldName = "density"
        }
)";
    const auto lowers = [](Driver &d) {
        std::vector<nova::Vec3f> out;
        for (nova::VolumeId v : d.scene().activeVolumes())
            out.push_back(d.scene().volumeRecord(v).lower);
        std::sort(out.begin(), out.end(), [](nova::Vec3f a, nova::Vec3f b) { return a.x < b.x; });
        return out;
    };
    SECTION("the prototype of a point instancer") {
        Driver d("#usda 1.0\ndef PointInstancer \"inst\"\n{\n    rel prototypes = </inst/smoke>\n"
                 "    int[] protoIndices = [0, 0, 0]\n    point3f[] positions = [(10, 0, 0), (20, 0, 0), (30, 5, 0)]\n"
                 "    def Volume \"smoke\"\n    {" + TfStringReplace(volume, "/FIELD", "/inst/smoke/density") + "    }\n}\n");
        std::vector<nova::Vec3f> at = lowers(d);
        REQUIRE(at.size() == 3);
        CHECK_THAT(at[1].x - at[0].x, WithinAbs(10.0, 1e-3));
        CHECK_THAT(at[2].x - at[1].x, WithinAbs(10.0, 1e-3));
        CHECK_THAT(at[2].y - at[1].y, WithinAbs(5.0, 1e-3));
        CHECK_THAT(at[1].y - at[0].y, WithinAbs(0.0, 1e-3));
        // The instances share the grid.
        const std::vector<nova::VolumeId> ids = d.scene().activeVolumes();
        CHECK(d.scene().volumeRecord(ids[0]).desc.fields[0].grid == d.scene().volumeRecord(ids[2]).desc.fields[0].grid);
        // Moved instances move their volumes; the grids are not loaded again.
        const auto grid = d.scene().volumeRecord(ids[0]).desc.fields[0].grid;
        d.set("/inst.positions", VtValue(VtVec3fArray{GfVec3f(10, 0, 0), GfVec3f(25, 0, 0), GfVec3f(30, 5, 0)}));
        d.sync();
        at = lowers(d);
        REQUIRE(at.size() == 3);
        CHECK_THAT(at[1].x - at[0].x, WithinAbs(15.0, 1e-3));
        CHECK(d.scene().volumeRecord(d.scene().activeVolumes()[0]).desc.fields[0].grid == grid);
        // Fewer and more instances.
        d.set("/inst.protoIndices", VtValue(VtIntArray{0}));
        d.set("/inst.positions", VtValue(VtVec3fArray{GfVec3f(7, 0, 0)}));
        d.sync();
        CHECK(lowers(d).size() == 1);
        d.set("/inst.protoIndices", VtValue(VtIntArray{0, 0, 0, 0}));
        d.set("/inst.positions",
              VtValue(VtVec3fArray{GfVec3f(0, 0, 0), GfVec3f(10, 0, 0), GfVec3f(20, 0, 0), GfVec3f(30, 0, 0)}));
        d.sync();
        at = lowers(d);
        REQUIRE(at.size() == 4);
        CHECK_THAT(at[3].x - at[0].x, WithinAbs(30.0, 1e-3));
        // No instance: nothing renders, and the prim comes back with its instances.
        d.set("/inst.protoIndices", VtValue(VtIntArray{}));
        d.set("/inst.positions", VtValue(VtVec3fArray{}));
        d.sync();
        CHECK(lowers(d).empty());
        d.set("/inst.protoIndices", VtValue(VtIntArray{0, 0}));
        d.set("/inst.positions", VtValue(VtVec3fArray{GfVec3f(0, 0, 0), GfVec3f(10, 0, 0)}));
        d.sync();
        CHECK(lowers(d).size() == 2);
        d.stage->RemovePrim(SdfPath("/inst"));
        d.sync();
        CHECK(d.scene().activeVolumes().empty());
    }
    SECTION("native instances") {
        Driver d("#usda 1.0\nclass \"proto\"\n{\n    def Volume \"smoke\"\n    {" +
                 TfStringReplace(volume, "/FIELD", "/proto/smoke/density") +
                 "    }\n}\n"
                 "def Xform \"a\" (\n    instanceable = true\n    references = </proto>\n)\n{\n"
                 "    double3 xformOp:translate = (10, 0, 0)\n    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n"
                 "def Xform \"b\" (\n    instanceable = true\n    references = </proto>\n)\n{\n"
                 "    double3 xformOp:translate = (22, 0, 0)\n    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n");
        std::vector<nova::Vec3f> at = lowers(d);
        REQUIRE(at.size() == 2);
        CHECK_THAT(at[1].x - at[0].x, WithinAbs(12.0, 1e-3));
        d.set("/b.xformOp:translate", VtValue(GfVec3d(40.0, 0.0, 0.0)));
        d.sync();
        at = lowers(d);
        REQUIRE(at.size() == 2);
        CHECK_THAT(at[1].x - at[0].x, WithinAbs(30.0, 1e-3));
    }
}

// Audit V-1 and V-2 (0.2.0): volumes had no categories, so a light with a
// link of any kind lit no volume and no shadow link held for them.
TEST_CASE("Volumes take part in light and shadow linking", "[hydra][volume][linking]") {
    Driver d(sceneText() + R"(
def Mesh "other"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(50, 0, 0), (51, 0, 0), (51, 1, 0), (50, 1, 0)]
    uniform token subdivisionScheme = "none"
}
def DistantLight "sun"
{
    uniform bool collection:lightLink:includeRoot = 1
    rel collection:lightLink:excludes = </other>
    uniform bool collection:shadowLink:includeRoot = 0
    rel collection:shadowLink:includes = </smoke>
}
def SphereLight "rim"
{
    uniform bool collection:lightLink:includeRoot = 0
    rel collection:lightLink:includes = </other>
}
)",
             true);
    REQUIRE(d.scene().activeVolumes().size() == 1);
    const nova::VolumeId id = d.scene().activeVolumes()[0];
    const auto lightOf = [&](nova::LightType type) {
        for (uint32_t i = 0; i < d.scene().lights().size(); ++i)
            if (d.scene().lights()[i].type == type)
                return i;
        FAIL("no light of that type");
        return 0u;
    };
    const uint32_t sun = lightOf(nova::LightType::Distant), rim = lightOf(nova::LightType::Sphere);
    {
        const nova::VolumeRecord &rec = d.scene().volumeRecord(id);
        CHECK_FALSE(rec.categories.empty());
        const nova::LightLinks links = d.scene().lightLinks();
        // The sun's link holds the volume, the rim's does not.
        CHECK(links.links(sun, rec.receiverSet));
        CHECK_FALSE(links.links(rim, rec.receiverSet));
        // The volume is in the sun's shadow link.
        const uint16_t category = d.scene().lights()[sun].shadowCategory;
        REQUIRE(category < 64);
        CHECK(((rec.shadowMask >> category) & 1u) != 0);
    }
    // Taken out of both of the sun's collections.
    UsdPrim light = d.stage->GetPrimAtPath(SdfPath("/sun"));
    light.GetRelationship(TfToken("collection:lightLink:excludes")).SetTargets({SdfPath("/other"), SdfPath("/smoke")});
    light.GetRelationship(TfToken("collection:shadowLink:includes")).SetTargets({SdfPath("/other")});
    d.sync();
    {
        const nova::VolumeRecord &rec = d.scene().volumeRecord(id);
        const nova::LightLinks links = d.scene().lightLinks();
        CHECK_FALSE(links.links(sun, rec.receiverSet));
        const uint16_t category = d.scene().lights()[sun].shadowCategory;
        REQUIRE(category < 64);
        CHECK(((rec.shadowMask >> category) & 1u) == 0);
    }
}
