// Random USD edits through the delegate against a freshly loaded stage, over
// the features that tests/test_hydra_fuzz.cpp leaves out: the scene index
// chain of the renderer plugin (render passes, velocity blur, implicit prims,
// light linking), motion over a shutter, subdivision, faces of any count,
// orientation, primvars of every interpolation (display colour and opacity,
// st, normals), per-object properties, groups, native instances, instancers
// of several prototypes, cubic curves, MaterialX and UsdPreviewSurface
// networks with textures and displacement, and the lights' shaping, IES
// profiles, shadow settings, textures and links.
//
// After every step the edited stage's scene must render what the scene of a
// new render index over the same stage renders, bit for bit.
//
// The default runs a few seeds; "[.][fuzz]" runs NOVA_FUZZ_SEEDS seeds of
// NOVA_FUZZ_STEPS steps from NOVA_FUZZ_FIRST. NOVA_FUZZ_ONLY=<kinds> keeps
// the edits whose kind's name holds one of the comma-separated words.

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "hydra/implicitSceneIndex.h"
#include "hydra/noticeSceneIndex.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"
#include "hydra/renderPassSceneIndex.h"
#include "hydra/resyncSceneIndex.h"
#include "hydra/stablePath.h"
#include "hydra/velocitySceneIndex.h"
#include "hydra/volumeDependencies.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>

#include <pxr/base/gf/vec3d.h>
#include <pxr/imaging/hd/dependencyForwardingSceneIndex.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/imaging/hdsi/legacyDisplayStyleOverrideSceneIndex.h>
#include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <process.h>
#include <random>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
namespace fs = std::filesystem;

namespace {

// Houdini's OCIO config, as husk sets it (before any texture is read).
const bool kOcioSet = [] {
    if (!std::getenv("OCIO"))
        _putenv_s("OCIO", NOVA_HFS "/packages/ocio/houdini-config-v3.0.0_aces-v2.0_ocio-v2.5.ocio");
    return true;
}();

int environmentInt(const char *name, int fallback) {
    char *value = nullptr;
    size_t size = 0;
    const bool has = _dupenv_s(&value, &size, name) == 0 && value && *value;
    const int result = has ? std::atoi(value) : fallback;
    std::free(value);
    return result;
}

std::string environmentText(const char *name) {
    char *value = nullptr;
    size_t size = 0;
    const bool has = _dupenv_s(&value, &size, name) == 0 && value && *value;
    const std::string result = has ? value : "";
    std::free(value);
    return result;
}

// Small images for the textures of materials and lights, written once per process.
std::string imageFile(int which) {
    static const std::string dir = [] {
        const fs::path d = fs::temp_directory_path() / ("nova_test_fuzz_" + std::to_string(_getpid()));
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        const auto write = [&](const char *name, int w, int h, int channels, OIIO::TypeDesc type) {
            std::vector<float> px(size_t(w) * size_t(h) * size_t(channels));
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < channels; ++c) {
                        const bool check = ((x / 4) + (y / 4)) % 2 == 0;
                        const float ramp = (float(x) + 0.5f) / float(w);
                        px[(size_t(y) * size_t(w) + size_t(x)) * size_t(channels) + size_t(c)] =
                            c == 3 ? (check ? 1.0f : 0.35f) : c == 0 ? ramp : c == 1 ? (check ? 0.85f : 0.15f) : 0.4f;
                    }
            const std::string path = (d / name).generic_string();
            auto out = OIIO::ImageOutput::create(path);
            REQUIRE(out);
            REQUIRE(out->open(path, OIIO::ImageSpec(w, h, channels, type)));
            REQUIRE(out->write_image(OIIO::TypeDesc::FLOAT, px.data()));
            out->close();
        };
        write("rgb.exr", 32, 32, 3, OIIO::TypeDesc::HALF);
        write("rgba.png", 32, 16, 4, OIIO::TypeDesc::UINT8);
        write("grey.exr", 16, 16, 1, OIIO::TypeDesc::HALF);
        write("sky.exr", 64, 32, 3, OIIO::TypeDesc::HALF);
        return d.generic_string();
    }();
    static const char *const names[4] = {"rgb.exr", "rgba.png", "grey.exr", "sky.exr"};
    return dir + "/" + names[which];
}

class SyncTask final : public HdTask {
public:
    explicit SyncTask(HdRenderPassSharedPtr pass) : HdTask(SdfPath::EmptyPath()), _pass(std::move(pass)) {}
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

using TokenArray = HdRetainedTypedSampledDataSource<VtArray<TfToken>>;

// A stage through UsdImaging and the scene indices of Nova's renderer plugin
// (hydra/implicitSurfaceSceneIndexPlugin.cpp, in its order) into a render
// index of its own, synced into the delegate's scene. The shutter is open
// half a frame around the frame, so authored time samples are motion.
struct Rig {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    explicit Rig(std::string const &text) {
        static std::atomic<int> counter{0};
        const std::string instance = "novaFeatureFuzz" + std::to_string(++counter);
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(text);
        stage = UsdStage::Open(layer);
        param().SetShutter(HdNovaResolveShutter(-0.25, 0.25, false, false));
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector(), instance));
        UsdImagingCreateSceneIndicesInfo info;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        sceneIndices.stageSceneIndex->SetTime(UsdTimeCode(0.0));

        const HdDataSourceBaseHandle toMesh =
            HdRetainedTypedSampledDataSource<TfToken>::New(HdsiImplicitSurfaceSceneIndexTokens->toMesh);
        const HdContainerDataSourceHandle implicitArgs = HdRetainedContainerDataSource::New(
            HdPrimTypeTokens->cube, toMesh, HdPrimTypeTokens->cone, toMesh, HdPrimTypeTokens->cylinder, toMesh,
            HdPrimTypeTokens->capsule, toMesh, HdPrimTypeTokens->plane, toMesh);
        const HdContainerDataSourceHandle linkArgs = HdRetainedContainerDataSource::New(
            HdsiLightLinkingSceneIndexTokens->lightPrimTypes,
            TokenArray::New(VtArray<TfToken>{HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight,
                                             HdPrimTypeTokens->sphereLight, HdPrimTypeTokens->rectLight,
                                             HdPrimTypeTokens->diskLight, HdPrimTypeTokens->cylinderLight,
                                             HdPrimTypeTokens->meshLight, HdPrimTypeTokens->light}),
            HdsiLightLinkingSceneIndexTokens->geometryPrimTypes,
            TokenArray::New(VtArray<TfToken>{HdPrimTypeTokens->mesh, HdPrimTypeTokens->basisCurves,
                                             HdPrimTypeTokens->points, HdPrimTypeTokens->sphere,
                                             HdPrimTypeTokens->volume}));

        // The refine level of a final render (husk's default complexity): subdivision surfaces are tessellated.
        HdsiLegacyDisplayStyleOverrideSceneIndexRefPtr style =
            HdsiLegacyDisplayStyleOverrideSceneIndex::New(sceneIndices.finalSceneIndex);
        style->SetRefineLevelFallback(8);
        HdSceneIndexBaseRefPtr scene = HdNovaRenderPassSceneIndex::New(style);
        scene = HdNovaVelocityModeSceneIndex::Append(scene);
        scene = HdNovaImplicitSceneIndex::New(scene);
        scene = HdsiImplicitSurfaceSceneIndex::New(scene, implicitArgs);
        scene = HdDependencyForwardingSceneIndex::New(HdNovaVolumeDependencySceneIndex::New(scene));
        scene = HdNovaResyncSceneIndex::New(scene, instance);
        scene = HdNovaNoticeSceneIndex::New(HdsiLightLinkingSceneIndex::New(HdNovaRelinkSceneIndex::New(scene), linkArgs));
        index->InsertSceneIndex(scene, SdfPath::AbsoluteRootPath(), false);
        pass = delegate->CreateRenderPass(index.get(),
                                          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        tasks.push_back(std::make_shared<SyncTask>(pass));
        // The stage after the chain exists, as in husk: the light-linking
        // index sees the prims arrive.
        sceneIndices.stageSceneIndex->SetStage(stage);
        sync();
    }
    ~Rig() {
        delegate->GetRenderThread().StopRender();
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
    std::string text() const {
        std::string out;
        stage->GetRootLayer()->ExportToString(&out);
        return out;
    }
};

const char *kStart = R"(#usda 1.0
(
    timeCodesPerSecond = 24
    upAxis = "Y"
)
def Xform "World"
{
    def Mesh "ground"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2.5, -2.5, -0.6), (2.5, -2.5, -0.6), (2.5, 2.5, -0.6), (-2.5, 2.5, -0.6)]
        color3f[] primvars:displayColor = [(0.6, 0.6, 0.6)]
        uniform token subdivisionScheme = "none"
    }
    def RectLight "key"
    {
        float inputs:intensity = 4
        float inputs:width = 1.5
        float inputs:height = 1.5
        double3 xformOp:translate = (0, 0, 3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Scope "Looks"
    {
    }
}
)";

// The host: random edits of a stage through USD's own interface.
class Host {
public:
    explicit Host(uint32_t seed, std::string const &only = "") : m_rng(seed), m_rig(kStart) {
        std::stringstream words(only);
        for (std::string w; std::getline(words, w, ',');)
            if (!w.empty())
                m_only.push_back(w);
    }

    Rig &rig() { return m_rig; }
    std::map<std::string, int> const &done() const { return m_done; }

    // One edit; its story. "none" when the edit drawn had nothing to work on.
    std::string edit() {
        struct Kind {
            const char *name;
            int weight;
            std::string (Host::*run)();
        };
        static const Kind kinds[] = {
            {"geometry add", 10, &Host::addGeometry},
            {"geometry reshape", 4, &Host::reshape},
            {"geometry move", 4, &Host::move},
            {"geometry visibility", 3, &Host::visibility},
            {"geometry activation", 2, &Host::activation},
            {"geometry remove", 3, &Host::removeGeometry},
            {"mesh scheme", 4, &Host::scheme},
            {"mesh orientation", 2, &Host::orientation},
            {"primvar colour", 4, &Host::displayColour},
            {"primvar opacity", 4, &Host::displayOpacity},
            {"primvar st", 4, &Host::textureCoordinates},
            {"primvar normals", 3, &Host::normals},
            {"object property", 5, &Host::objectProperty},
            {"motion", 5, &Host::motion},
            {"group", 4, &Host::group},
            {"native instance", 4, &Host::nativeInstance},
            {"material make", 8, &Host::material},
            {"material input", 4, &Host::materialInput},
            {"material bind", 7, &Host::binding},
            {"material remove", 2, &Host::removeMaterial},
            {"subset", 3, &Host::subset},
            {"light add", 5, &Host::addLight},
            {"light change", 4, &Host::changeLight},
            {"light shadow", 4, &Host::lightShadow},
            {"light shaping", 4, &Host::lightShaping},
            {"light link", 4, &Host::lightLink},
            {"light texture", 3, &Host::lightTexture},
            {"light visibility", 2, &Host::lightVisibility},
            {"light remove", 2, &Host::removeLight},
            {"instancer", 5, &Host::instancer},
            {"instancer edit", 4, &Host::instancerEdit},
            {"instancer remove", 1, &Host::removeInstancer},
            {"strands", 4, &Host::strands},
            {"strands remove", 1, &Host::removeStrands},
        };
        std::vector<const Kind *> allowed;
        int total = 0;
        for (const Kind &k : kinds) {
            bool in = m_only.empty();
            for (std::string const &w : m_only)
                in = in || std::string(k.name).find(w) != std::string::npos;
            if (in) {
                allowed.push_back(&k);
                total += k.weight;
            }
        }
        if (allowed.empty())
            return "none";
        int draw = pick(total);
        for (const Kind *k : allowed) {
            if (draw < k->weight) {
                const std::string story = (this->*(k->run))();
                if (story != "none")
                    ++m_done[k->name];
                return story;
            }
            draw -= k->weight;
        }
        return "none";
    }

private:
    int pick(int n) { return int(std::uniform_int_distribution<int>(0, n - 1)(m_rng)); }
    float uniform(float a, float b) { return std::uniform_real_distribution<float>(a, b)(m_rng); }
    bool chance(float p) { return uniform(0.0f, 1.0f) < p; }
    SdfPath any(std::vector<SdfPath> const &list) { return list[size_t(pick(int(list.size())))]; }
    UsdStageRefPtr stage() { return m_rig.stage; }
    UsdPrim prim(SdfPath const &path) { return m_rig.stage->GetPrimAtPath(path); }
    GfVec3f colourValue() { return GfVec3f(uniform(0.1f, 0.9f), uniform(0.1f, 0.9f), uniform(0.1f, 0.9f)); }
    // A prim that exists and can take edits below it (nothing can be defined below a deactivated prim).
    bool usable(SdfPath const &path) {
        const UsdPrim p = prim(path);
        if (!p)
            return false;
        for (UsdPrim q = p; q && !q.IsPseudoRoot(); q = q.GetParent())
            if (!q.IsActive())
                return false;
        return true;
    }
    std::string name(SdfPath const &path) { return path.GetName(); }

    void drop(std::vector<SdfPath> &list, SdfPath const &root) {
        for (size_t i = list.size(); i-- > 0;)
            if (list[i].HasPrefix(root))
                list.erase(list.begin() + ptrdiff_t(i));
    }
    std::string removeFrom(std::vector<SdfPath> &list, const char *what) {
        if (list.empty())
            return "none";
        const SdfPath path = any(list);
        stage()->RemovePrim(path);
        drop(list, path);
        return std::string("remove ") + what + " " + name(path);
    }

    // ---------------------------------------------------------------- geometry
    static int faceCountOf(UsdGeomMesh const &mesh) {
        VtIntArray counts;
        mesh.GetFaceVertexCountsAttr().Get(&counts);
        return int(counts.size());
    }
    static int cornerCountOf(UsdGeomMesh const &mesh) {
        VtIntArray indices;
        mesh.GetFaceVertexIndicesAttr().Get(&indices);
        return int(indices.size());
    }
    static int pointCountOf(UsdGeomMesh const &mesh) {
        VtVec3fArray points;
        mesh.GetPointsAttr().Get(&points);
        return int(points.size());
    }

    // The shapes a mesh takes: a quad, a box, a triangle, a pentagon, a
    // concave face of six vertices, a face of seven, a strip of two quads
    // with a triangle, a grid of nine quads (one of them a hole at times).
    std::string shape(UsdGeomMesh &mesh) {
        const float x = uniform(0.15f, 0.5f), y = uniform(0.15f, 0.5f), z = uniform(0.05f, 0.4f);
        VtVec3fArray points;
        VtIntArray counts, indices, holes;
        std::string what;
        switch (pick(8)) {
        case 0:
            points = {{-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0}};
            counts = {4};
            indices = {0, 1, 2, 3};
            what = "quad";
            break;
        case 1:
            points = {{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z}, {-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z}};
            counts = {4, 4, 4, 4, 4, 4};
            indices = {0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5};
            what = "box";
            break;
        case 2:
            points = {{-x, -y, 0}, {x, -y, 0.1f}, {0, y, 0}};
            counts = {3};
            indices = {0, 1, 2};
            what = "triangle";
            break;
        case 3:
            points = {{-x, -y, 0}, {x, -y, 0}, {1.3f * x, 0.2f * y, z * 0.3f}, {0, y, 0}, {-1.3f * x, 0.2f * y, 0}};
            counts = {5};
            indices = {0, 1, 2, 3, 4};
            what = "pentagon";
            break;
        case 4: // an L
            points = {{-x, -y, 0}, {x, -y, 0}, {x, 0, 0}, {0, 0, 0}, {0, y, 0}, {-x, y, 0}};
            counts = {6};
            indices = {0, 1, 2, 3, 4, 5};
            what = "concave face";
            break;
        case 5:
            for (int i = 0; i < 7; ++i) {
                const float a = 6.2831853f * float(i) / 7.0f;
                points.push_back({x * std::cos(a), y * std::sin(a), (i & 1) ? z * 0.2f : 0.0f});
                indices.push_back(i);
            }
            counts = {7};
            what = "heptagon";
            break;
        case 6:
            points = {{-x, -y, 0}, {0, -y, 0}, {x, -y, 0}, {-x, y, 0}, {0, y, z * 0.5f}, {x, y, 0}, {1.6f * x, 0, 0}};
            counts = {4, 4, 3};
            indices = {0, 1, 4, 3, 1, 2, 5, 4, 2, 6, 5};
            what = "strip";
            break;
        default:
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i)
                    points.push_back({x * (float(i) / 1.5f - 1.0f), y * (float(j) / 1.5f - 1.0f),
                                      ((i + j) & 1) ? z * 0.3f : 0.0f});
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i) {
                    counts.push_back(4);
                    const int a = j * 4 + i;
                    indices.push_back(a);
                    indices.push_back(a + 1);
                    indices.push_back(a + 5);
                    indices.push_back(a + 4);
                }
            if (chance(0.4f))
                holes = {4};
            what = holes.empty() ? "grid" : "grid with a hole";
            break;
        }
        // New points alone when the topology stays (an attribute edit, as a deforming mesh).
        clearSamples(mesh.GetPointsAttr());
        mesh.CreatePointsAttr().Set(points);
        mesh.CreateFaceVertexCountsAttr().Set(counts);
        mesh.CreateFaceVertexIndicesAttr().Set(indices);
        if (holes.empty())
            mesh.GetPrim().RemoveProperty(TfToken("holeIndices"));
        else
            mesh.CreateHoleIndicesAttr().Set(holes);
        // Primvars sized for the old topology would be wrong for the new one.
        for (const char *primvar : {"primvars:st", "primvars:displayOpacity", "primvars:normals", "normals", "velocities"})
            mesh.GetPrim().RemoveProperty(TfToken(primvar));
        VtVec3fArray one;
        UsdAttribute colour = mesh.GetDisplayColorAttr();
        if (colour && colour.Get(&one) && one.size() != 1)
            mesh.GetPrim().RemoveProperty(TfToken("primvars:displayColor"));
        if (const UsdPrim part = mesh.GetPrim().GetChild(TfToken("part")))
            stage()->RemovePrim(part.GetPath());
        return what;
    }

    static void clearSamples(UsdAttribute const &attr) {
        if (attr && attr.GetNumTimeSamples() > 0)
            attr.Clear();
    }

    void place(SdfPath const &path) {
        UsdPrim p = prim(path);
        for (const char *op : {"xformOp:translate", "xformOp:rotateXYZ", "xformOp:scale"})
            clearSamples(p.GetAttribute(TfToken(op)));
        UsdGeomXformCommonAPI api(p);
        api.SetTranslate(GfVec3d(uniform(-1.1f, 1.1f), uniform(-1.1f, 1.1f), uniform(-0.3f, 0.9f)));
        api.SetRotate(GfVec3f(uniform(0.0f, 90.0f), uniform(0.0f, 90.0f), uniform(0.0f, 90.0f)));
        if (chance(0.3f)) // also mirrored
            api.SetScale(GfVec3f(chance(0.3f) ? -1.0f : 1.0f, uniform(0.5f, 1.5f), uniform(0.5f, 1.5f)));
    }

    SdfPath parentForNew() {
        std::vector<SdfPath> open;
        for (SdfPath const &g : m_groups)
            if (usable(g))
                open.push_back(g);
        if (!open.empty() && chance(0.4f))
            return any(open);
        return SdfPath("/World");
    }

    // What `path` is made of: a prim of one of the kinds, with its attributes.
    std::string define(SdfPath const &path) {
        static const char *const implicit[5] = {"Cube", "Cylinder", "Cone", "Capsule", "Plane"};
        const int kind = pick(10);
        if (kind < 6) {
            UsdGeomMesh mesh = UsdGeomMesh::Define(stage(), path);
            mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
            return "mesh (" + shape(mesh) + ")";
        }
        if (kind < 8) {
            UsdPrim p = stage()->DefinePrim(path, TfToken("Sphere"));
            p.CreateAttribute(TfToken("radius"), SdfValueTypeNames->Double).Set(double(uniform(0.15f, 0.4f)));
            return "sphere";
        }
        const char *type = implicit[pick(5)];
        UsdPrim p = stage()->DefinePrim(path, TfToken(type));
        size(p);
        return type;
    }
    void size(UsdPrim const &p) {
        const std::string type = p.GetTypeName().GetString();
        const auto number = [&](const char *attr, float v) {
            p.CreateAttribute(TfToken(attr), SdfValueTypeNames->Double).Set(double(v));
        };
        if (type == "Sphere") {
            number("radius", uniform(0.15f, 0.4f));
        } else if (type == "Cube") {
            number("size", uniform(0.2f, 0.6f));
        } else if (type == "Plane") {
            number("width", uniform(0.3f, 1.0f));
            number("length", uniform(0.3f, 1.0f));
            p.CreateAttribute(TfToken("axis"), SdfValueTypeNames->Token, SdfVariabilityUniform)
                .Set(TfToken(chance(0.7f) ? "Z" : "Y"));
        } else {
            number("radius", uniform(0.1f, 0.3f));
            number("height", uniform(0.2f, 0.7f));
            p.CreateAttribute(TfToken("axis"), SdfValueTypeNames->Token, SdfVariabilityUniform)
                .Set(TfToken(chance(0.5f) ? "Z" : chance(0.5f) ? "X" : "Y"));
        }
    }

    std::string addGeometry() {
        if (m_geometry.size() >= 9)
            return removeFrom(m_geometry, "geometry");
        const SdfPath parent = parentForNew();
        const SdfPath path = parent.AppendChild(TfToken("g" + std::to_string(++m_names)));
        const std::string what = define(path);
        place(path);
        if (chance(0.5f))
            UsdGeomGprim(prim(path)).CreateDisplayColorAttr().Set(VtVec3fArray{colourValue()});
        m_geometry.push_back(path);
        if (!m_materials.empty() && chance(0.6f))
            bind(path, any(m_materials));
        return "add " + what + " " + path.GetString();
    }
    // A prim of the list that edits can reach, or an empty path.
    SdfPath usableOf(std::vector<SdfPath> const &list) {
        std::vector<SdfPath> open;
        for (SdfPath const &p : list)
            if (usable(p))
                open.push_back(p);
        return open.empty() ? SdfPath() : any(open);
    }
    // Meshes: of the geometry, of instancers' and native instances' prototypes.
    SdfPath anyMesh() {
        std::vector<SdfPath> meshes;
        for (std::vector<SdfPath> const *list : {&m_geometry, &m_prototypes})
            for (SdfPath const &p : *list)
                if (usable(p) && prim(p).IsA<UsdGeomMesh>())
                    meshes.push_back(p);
        return meshes.empty() ? SdfPath() : any(meshes);
    }
    SdfPath anyGprim() {
        std::vector<SdfPath> all;
        for (std::vector<SdfPath> const *list : {&m_geometry, &m_prototypes, &m_strands})
            for (SdfPath const &p : *list)
                if (usable(p))
                    all.push_back(p);
        return all.empty() ? SdfPath() : any(all);
    }

    std::string reshape() {
        const SdfPath path = anyGprim();
        if (path.IsEmpty() || prim(path).GetTypeName() == TfToken("BasisCurves") ||
            prim(path).GetTypeName() == TfToken("Points"))
            return "none";
        UsdPrim p = prim(path);
        if (UsdGeomMesh mesh{p})
            return "reshape " + name(path) + " (" + shape(mesh) + ")";
        size(p);
        return "resize " + name(path);
    }
    std::string move() {
        const SdfPath path = usableOf(m_geometry);
        if (path.IsEmpty())
            return "none";
        place(path);
        return "move " + name(path);
    }
    std::string toggleVisibility(SdfPath const &path) {
        UsdGeomImageable imageable(prim(path));
        TfToken now;
        imageable.GetVisibilityAttr().Get(&now);
        // (Authored, not MakeVisible: that would also change the ancestors.)
        imageable.CreateVisibilityAttr().Set(now == UsdGeomTokens->invisible ? UsdGeomTokens->inherited
                                                                             : UsdGeomTokens->invisible);
        return (now == UsdGeomTokens->invisible ? "show " : "hide ") + name(path);
    }
    std::string visibility() {
        const SdfPath path = usableOf(m_geometry);
        return path.IsEmpty() ? "none" : toggleVisibility(path);
    }
    std::string activation() {
        if (m_geometry.empty())
            return "none";
        const SdfPath path = any(m_geometry);
        UsdPrim p = prim(path);
        if (!p || !usable(path.GetParentPath()))
            return "none";
        const bool active = p.IsActive();
        p.SetActive(!active);
        return (active ? "deactivate " : "activate ") + name(path);
    }
    std::string removeGeometry() { return removeFrom(m_geometry, "geometry"); }

    std::string scheme() {
        const SdfPath path = anyMesh();
        if (path.IsEmpty())
            return "none";
        static const char *const schemes[4] = {"none", "catmullClark", "bilinear", "loop"};
        const char *s = schemes[pick(chance(0.15f) ? 4 : 2)];
        UsdGeomMesh(prim(path)).CreateSubdivisionSchemeAttr().Set(TfToken(s));
        return std::string("scheme ") + s + " on " + name(path);
    }
    std::string orientation() {
        const SdfPath path = anyGprim();
        if (path.IsEmpty())
            return "none";
        UsdGeomGprim gprim(prim(path));
        if (chance(0.5f)) {
            const bool left = chance(0.5f);
            gprim.CreateOrientationAttr().Set(left ? UsdGeomTokens->leftHanded : UsdGeomTokens->rightHanded);
            return std::string(left ? "left-handed " : "right-handed ") + name(path);
        }
        const bool both = chance(0.5f);
        gprim.CreateDoubleSidedAttr().Set(both);
        return std::string(both ? "double-sided " : "single-sided ") + name(path);
    }

    // The element count of an interpolation on a mesh; 1 on other prims.
    TfToken interpolationFor(UsdPrim const &p, int &count, bool withFaceVarying = true) {
        count = 1;
        UsdGeomMesh mesh(p);
        if (!mesh)
            return UsdGeomTokens->constant;
        switch (pick(withFaceVarying ? 4 : 3)) {
        case 0:
            return UsdGeomTokens->constant;
        case 1:
            count = faceCountOf(mesh);
            return UsdGeomTokens->uniform;
        case 2:
            count = pointCountOf(mesh);
            return UsdGeomTokens->vertex;
        default:
            count = cornerCountOf(mesh);
            return UsdGeomTokens->faceVarying;
        }
    }
    std::string displayColour() {
        const SdfPath path = anyGprim();
        if (path.IsEmpty())
            return "none";
        UsdPrim p = prim(path);
        if (chance(0.15f)) {
            p.RemoveProperty(TfToken("primvars:displayColor"));
            return "no colour on " + name(path);
        }
        int count = 1;
        const TfToken interpolation = interpolationFor(p, count);
        VtVec3fArray values;
        for (int i = 0; i < count; ++i)
            values.push_back(colourValue());
        UsdGeomPrimvar primvar = UsdGeomPrimvarsAPI(p).CreatePrimvar(TfToken("displayColor"),
                                                                    SdfValueTypeNames->Color3fArray, interpolation);
        primvar.Set(values);
        return "colour (" + interpolation.GetString() + ") on " + name(path);
    }
    std::string displayOpacity() {
        const SdfPath path = anyGprim();
        if (path.IsEmpty())
            return "none";
        UsdPrim p = prim(path);
        if (chance(0.3f)) {
            p.RemoveProperty(TfToken("primvars:displayOpacity"));
            return "no opacity on " + name(path);
        }
        int count = 1;
        const TfToken interpolation = interpolationFor(p, count, false);
        VtFloatArray values;
        for (int i = 0; i < count; ++i)
            values.push_back(chance(0.3f) ? 1.0f : uniform(0.1f, 0.9f));
        UsdGeomPrimvarsAPI(p)
            .CreatePrimvar(TfToken("displayOpacity"), SdfValueTypeNames->FloatArray, interpolation)
            .Set(values);
        return "opacity (" + interpolation.GetString() + ") on " + name(path);
    }
    std::string textureCoordinates() {
        const SdfPath path = anyMesh();
        if (path.IsEmpty())
            return "none";
        UsdPrim p = prim(path);
        if (chance(0.35f)) {
            p.RemoveProperty(TfToken("primvars:st"));
            return "no st on " + name(path);
        }
        UsdGeomMesh mesh(p);
        const bool perCorner = chance(0.6f);
        const int count = perCorner ? cornerCountOf(mesh) : pointCountOf(mesh);
        VtVec2fArray values;
        for (int i = 0; i < count; ++i)
            values.push_back(GfVec2f(uniform(0.0f, 2.0f), uniform(0.0f, 2.0f)));
        UsdGeomPrimvarsAPI(p)
            .CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray,
                           perCorner ? UsdGeomTokens->faceVarying : UsdGeomTokens->vertex)
            .Set(values);
        return std::string("st (") + (perCorner ? "faceVarying" : "vertex") + ") on " + name(path);
    }
    std::string normals() {
        const SdfPath path = anyMesh();
        if (path.IsEmpty())
            return "none";
        UsdPrim p = prim(path);
        p.RemoveProperty(TfToken("primvars:normals"));
        p.RemoveProperty(TfToken("normals"));
        if (chance(0.3f))
            return "no normals on " + name(path);
        UsdGeomMesh mesh(p);
        const bool perCorner = chance(0.5f);
        const int count = perCorner ? cornerCountOf(mesh) : pointCountOf(mesh);
        VtVec3fArray values;
        for (int i = 0; i < count; ++i)
            values.push_back(GfVec3f(uniform(-0.4f, 0.4f), uniform(-0.4f, 0.4f), 1.0f).GetNormalized());
        if (chance(0.5f)) {
            mesh.CreateNormalsAttr().Set(values);
            mesh.SetNormalsInterpolation(perCorner ? UsdGeomTokens->faceVarying : UsdGeomTokens->vertex);
        } else {
            UsdGeomPrimvarsAPI(p)
                .CreatePrimvar(TfToken("normals"), SdfValueTypeNames->Normal3fArray,
                               perCorner ? UsdGeomTokens->faceVarying : UsdGeomTokens->vertex)
                .Set(values);
        }
        return std::string("normals (") + (perCorner ? "faceVarying" : "vertex") + ") on " + name(path);
    }

    // Karma's and Nova's per-object properties, as constant primvars.
    std::string objectProperty() {
        SdfPath path = anyGprim();
        if (!m_groups.empty() && chance(0.2f)) // inherited from a group
            path = usableOf(m_groups);
        if (path.IsEmpty())
            return "none";
        UsdPrim p = prim(path);
        UsdGeomPrimvarsAPI api(p);
        static const char *const visibilities[6] = {"*", "-primary", "-shadow", "-diffuse", "primary", "-reflect -refract"};
        const auto remove = [&](const char *primvar) {
            p.RemoveProperty(TfToken(std::string("primvars:") + primvar));
            return std::string("no ") + primvar + " on " + name(path);
        };
        const bool off = chance(0.25f);
        switch (pick(11)) {
        case 0:
            if (off)
                return remove("karma:object:rendervisibility");
            api.CreatePrimvar(TfToken("karma:object:rendervisibility"), SdfValueTypeNames->String)
                .Set(std::string(visibilities[pick(6)]));
            return "render visibility on " + name(path);
        case 1:
            if (off)
                return remove("karma:object:holdoutmode");
            api.CreatePrimvar(TfToken("karma:object:holdoutmode"), SdfValueTypeNames->Int).Set(pick(3));
            return "holdout mode on " + name(path);
        case 2:
            if (off)
                return remove("karma:object:treat_as_lightsource");
            api.CreatePrimvar(TfToken("karma:object:treat_as_lightsource"), SdfValueTypeNames->Bool).Set(chance(0.6f));
            return "light source on " + name(path);
        case 3:
            if (off)
                return remove("nova:object:fakeCaustics");
            api.CreatePrimvar(TfToken("nova:object:fakeCaustics"), SdfValueTypeNames->Bool).Set(chance(0.5f));
            return "fake caustics on " + name(path);
        case 4:
            if (off)
                return remove("karma:object:diffuselimit");
            api.CreatePrimvar(TfToken("karma:object:diffuselimit"), SdfValueTypeNames->Int).Set(pick(3));
            return "diffuse limit on " + name(path);
        case 5:
            if (off)
                return remove("karma:object:reflectlimit");
            api.CreatePrimvar(TfToken("karma:object:reflectlimit"), SdfValueTypeNames->Int).Set(pick(4));
            return "reflect limit on " + name(path);
        case 6:
            if (off)
                return remove("karma:object:mblur");
            api.CreatePrimvar(TfToken("karma:object:mblur"), SdfValueTypeNames->Bool).Set(chance(0.5f));
            return "motion blur switch on " + name(path);
        case 7:
            if (off)
                return remove("nova:object:curveStyle");
            api.CreatePrimvar(TfToken("nova:object:curveStyle"), SdfValueTypeNames->String)
                .Set(std::string(chance(0.5f) ? "ribbon" : "tube"));
            return "curve style on " + name(path);
        case 8:
            if (off)
                return remove("nova:object:widthScale");
            api.CreatePrimvar(TfToken("nova:object:widthScale"), SdfValueTypeNames->Float).Set(uniform(0.5f, 2.0f));
            return "width scale on " + name(path);
        case 9:
            if (off)
                return remove("karma:object:lightsource_diffusescale");
            api.CreatePrimvar(TfToken("karma:object:lightsource_diffusescale"), SdfValueTypeNames->Float)
                .Set(uniform(0.2f, 2.0f));
            return "light source diffuse scale on " + name(path);
        default:
            if (off)
                return remove("karma:object:dielectricpriority");
            api.CreatePrimvar(TfToken("karma:object:dielectricpriority"), SdfValueTypeNames->Int).Set(pick(4));
            return "dielectric priority on " + name(path);
        }
    }

    // Time samples around the frame: a transform, a mesh's points, velocities.
    std::string motion() {
        SdfPath path = anyGprim();
        if (!m_groups.empty() && chance(0.15f))
            path = usableOf(m_groups);
        if (!m_instancers.empty() && chance(0.15f))
            path = usableOf(m_instancers);
        if (path.IsEmpty())
            return "none";
        UsdPrim p = prim(path);
        const int kind = pick(4);
        if (kind == 0) { // a moving transform
            UsdGeomXformCommonAPI api(p);
            GfVec3d t;
            GfVec3f r, s, pivot;
            UsdGeomXformCommonAPI::RotationOrder order;
            if (!api.GetXformVectors(&t, &r, &s, &pivot, &order, UsdTimeCode::Default()))
                return "none";
            const GfVec3d d(uniform(-0.4f, 0.4f), uniform(-0.4f, 0.4f), uniform(-0.2f, 0.2f));
            api.SetTranslate(t - d, UsdTimeCode(-1.0));
            api.SetTranslate(t + d, UsdTimeCode(1.0));
            if (chance(0.4f)) {
                api.SetRotate(r, UsdGeomXformCommonAPI::RotationOrderXYZ, UsdTimeCode(-1.0));
                api.SetRotate(r + GfVec3f(0.0f, 0.0f, uniform(10.0f, 80.0f)), UsdGeomXformCommonAPI::RotationOrderXYZ,
                              UsdTimeCode(1.0));
            }
            return "moving transform on " + name(path);
        }
        if (kind == 1) { // at rest again
            for (const char *op : {"xformOp:translate", "xformOp:rotateXYZ"}) {
                UsdAttribute attr = p.GetAttribute(TfToken(op));
                if (attr && attr.GetNumTimeSamples() > 0) {
                    VtValue v;
                    attr.Get(&v, UsdTimeCode(0.0));
                    attr.Clear();
                    attr.Set(v);
                }
            }
            for (const char *a : {"points", "positions"}) {
                UsdAttribute attr = p.GetAttribute(TfToken(a));
                if (attr && attr.GetNumTimeSamples() > 0) {
                    VtValue v;
                    attr.Get(&v, UsdTimeCode(-1.0));
                    attr.Clear();
                    attr.Set(v);
                }
            }
            p.RemoveProperty(TfToken("velocities"));
            return "rest " + name(path);
        }
        UsdAttribute points = p.GetAttribute(TfToken(p.GetTypeName() == TfToken("PointInstancer") ? "positions" : "points"));
        VtVec3fArray now;
        if (!points || !points.Get(&now, UsdTimeCode(-1.0)) || now.empty())
            return "none";
        if (kind == 2) { // deforming: other points a frame later
            VtVec3fArray later = now;
            for (GfVec3f &v : later)
                v += GfVec3f(uniform(-0.2f, 0.2f), uniform(-0.2f, 0.2f), uniform(-0.1f, 0.1f));
            points.Clear();
            points.Set(now, UsdTimeCode(-1.0));
            points.Set(later, UsdTimeCode(1.0));
            return "deforming " + name(path);
        }
        VtVec3fArray velocities;
        for (size_t i = 0; i < now.size(); ++i)
            velocities.push_back(GfVec3f(uniform(-6.0f, 6.0f), uniform(-6.0f, 6.0f), uniform(-2.0f, 2.0f)));
        p.CreateAttribute(TfToken("velocities"), SdfValueTypeNames->Vector3fArray).Set(velocities);
        if (chance(0.5f))
            UsdGeomPrimvarsAPI(p)
                .CreatePrimvar(TfToken("karma:object:vblur"), SdfValueTypeNames->Token)
                .Set(TfToken(chance(0.7f) ? "velocity" : "none"));
        return "velocities on " + name(path);
    }

    // Groups: transforms above geometry that move, hide, leave and return.
    std::string group() {
        if (m_groups.size() < 3 && (m_groups.empty() || chance(0.3f))) {
            const SdfPath path("/World/grp" + std::to_string(++m_names));
            stage()->DefinePrim(path, TfToken("Xform"));
            place(path);
            m_groups.push_back(path);
            return "add group " + name(path);
        }
        const SdfPath path = any(m_groups);
        UsdPrim p = prim(path);
        switch (pick(6)) {
        case 0:
        case 1:
            if (!usable(path))
                return "none";
            place(path);
            return "move group " + name(path);
        case 2:
            if (!usable(path))
                return "none";
            return toggleVisibility(path);
        case 3: {
            const bool active = p.IsActive();
            p.SetActive(!active);
            return (active ? "deactivate group " : "activate group ") + name(path);
        }
        case 4: { // a material for everything below
            if (m_materials.empty() || !usable(path))
                return "none";
            const SdfPath material = any(m_materials);
            bind(path, material);
            return "bind " + name(material) + " to group " + name(path);
        }
        default:
            stage()->RemovePrim(path);
            drop(m_geometry, path);
            drop(m_strands, path);
            drop(m_groups, path);
            return "remove group " + name(path);
        }
    }

    // Native instances: a class with geometry, referenced by instanceable prims.
    std::string nativeInstance() {
        if (m_classes.size() < 2 && (m_classes.empty() || chance(0.25f))) {
            const SdfPath path("/_class" + std::to_string(++m_names));
            stage()->CreateClassPrim(path);
            const SdfPath child = path.AppendChild(TfToken("geo"));
            const std::string what = define(child);
            if (chance(0.5f))
                UsdGeomGprim(prim(child)).CreateDisplayColorAttr().Set(VtVec3fArray{colourValue()});
            m_classes.push_back(path);
            m_prototypes.push_back(child);
            return "add class " + name(path) + " of a " + what;
        }
        const SdfPath source = any(m_classes);
        switch (pick(5)) {
        case 0:
        case 1: {
            if (m_instances.size() >= 5)
                return removeFrom(m_instances, "instance");
            const SdfPath path("/World/ni" + std::to_string(++m_names));
            UsdPrim p = stage()->DefinePrim(path, TfToken("Xform"));
            p.GetReferences().AddInternalReference(source);
            p.SetInstanceable(true);
            place(path);
            m_instances.push_back(path);
            return "add instance " + name(path) + " of " + name(source);
        }
        case 2: {
            if (m_instances.empty())
                return "none";
            const SdfPath path = any(m_instances);
            place(path);
            return "move instance " + name(path);
        }
        case 3: {
            if (m_instances.empty())
                return "none";
            const SdfPath path = any(m_instances);
            UsdPrim p = prim(path);
            const bool now = p.IsInstanceable();
            p.SetInstanceable(!now);
            return std::string(now ? "not instanceable " : "instanceable ") + name(path);
        }
        default:
            if (m_instances.empty())
                return "none";
            return chance(0.5f) ? toggleVisibility(any(m_instances)) : removeFrom(m_instances, "instance");
        }
    }

    // ---------------------------------------------------------------- materials
    void bind(SdfPath const &target, SdfPath const &material) {
        UsdShadeMaterialBindingAPI::Apply(prim(target)).Bind(UsdShadeMaterial(prim(material)));
    }
    void input(UsdShadeShader &shader, const char *name, float v) {
        shader.CreateInput(TfToken(name), SdfValueTypeNames->Float).Set(v);
    }
    void input(UsdShadeShader &shader, const char *name, GfVec3f v) {
        shader.CreateInput(TfToken(name), SdfValueTypeNames->Color3f).Set(v);
    }

    // A material of a random kind at `path`, in place of what was there.
    std::string material() {
        const bool fresh = m_materials.size() < 5 && (m_materials.empty() || chance(0.5f));
        const SdfPath path = fresh ? SdfPath("/World/Looks/m" + std::to_string(++m_names)) : any(m_materials);
        if (!fresh)
            stage()->RemovePrim(path);
        UsdShadeMaterial mat = UsdShadeMaterial::Define(stage(), path);
        UsdShadeShader shader = UsdShadeShader::Define(stage(), path.AppendChild(TfToken("s")));
        std::string kind;
        const int k = pick(9);
        if (k < 3) { // UsdPreviewSurface, plain or with a texture network
            shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
            mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));
            input(shader, "diffuseColor", colourValue());
            input(shader, "roughness", uniform(0.05f, 0.8f));
            input(shader, "metallic", k == 1 ? 1.0f : 0.0f);
            input(shader, "opacity", chance(0.25f) ? uniform(0.2f, 0.8f) : 1.0f);
            kind = "preview";
            if (k == 2) {
                UsdShadeShader tex = UsdShadeShader::Define(stage(), path.AppendChild(TfToken("tex")));
                tex.CreateIdAttr(VtValue(TfToken("UsdUVTexture")));
                tex.CreateInput(TfToken("file"), SdfValueTypeNames->Asset).Set(SdfAssetPath(imageFile(pick(2))));
                if (chance(0.5f))
                    tex.CreateInput(TfToken("wrapS"), SdfValueTypeNames->Token).Set(TfToken("repeat"));
                UsdShadeShader reader = UsdShadeShader::Define(stage(), path.AppendChild(TfToken("reader")));
                reader.CreateIdAttr(VtValue(TfToken("UsdPrimvarReader_float2")));
                reader.CreateInput(TfToken("varname"), SdfValueTypeNames->String).Set(std::string("st"));
                tex.CreateInput(TfToken("st"), SdfValueTypeNames->Float2)
                    .ConnectToSource(reader.CreateOutput(TfToken("result"), SdfValueTypeNames->Float2));
                shader.GetInput(TfToken("diffuseColor"))
                    .ConnectToSource(tex.CreateOutput(TfToken("rgb"), SdfValueTypeNames->Float3));
                kind = "preview with a texture";
            }
        } else { // MaterialX Standard Surface
            shader.CreateIdAttr(VtValue(TfToken("ND_standard_surface_surfaceshader")));
            mat.CreateSurfaceOutput(TfToken("mtlx"))
                .ConnectToSource(shader.CreateOutput(TfToken("surface"), SdfValueTypeNames->Token));
            input(shader, "base_color", colourValue());
            input(shader, "specular_roughness", uniform(0.05f, 0.8f));
            kind = "standard surface";
            switch (k) {
            case 3:
                input(shader, "metalness", 1.0f);
                kind += ", metal";
                break;
            case 4:
                input(shader, "transmission", uniform(0.5f, 1.0f));
                input(shader, "coat", chance(0.5f) ? 0.8f : 0.0f);
                kind += ", glass";
                break;
            case 5:
                input(shader, "emission", uniform(0.5f, 3.0f));
                input(shader, "emission_color", colourValue());
                kind += ", emitter";
                break;
            case 6: { // an image as the base colour, by the default coordinates or by st
                UsdShadeShader image = UsdShadeShader::Define(stage(), path.AppendChild(TfToken("image")));
                image.CreateIdAttr(VtValue(TfToken("ND_image_color3")));
                image.CreateInput(TfToken("file"), SdfValueTypeNames->Asset).Set(SdfAssetPath(imageFile(pick(2))));
                shader.GetInput(TfToken("base_color"))
                    .ConnectToSource(image.CreateOutput(TfToken("out"), SdfValueTypeNames->Color3f));
                kind += ", image";
                break;
            }
            case 7: { // a primvar as the base colour, and an opacity
                UsdShadeShader value = UsdShadeShader::Define(stage(), path.AppendChild(TfToken("value")));
                value.CreateIdAttr(VtValue(TfToken("ND_geompropvalue_color3")));
                value.CreateInput(TfToken("geomprop"), SdfValueTypeNames->String).Set(std::string("displayColor"));
                value.CreateInput(TfToken("default"), SdfValueTypeNames->Color3f).Set(GfVec3f(0.8f, 0.2f, 0.2f));
                shader.GetInput(TfToken("base_color"))
                    .ConnectToSource(value.CreateOutput(TfToken("out"), SdfValueTypeNames->Color3f));
                if (chance(0.5f))
                    input(shader, "opacity", GfVec3f(uniform(0.2f, 0.9f)));
                kind += ", primvar";
                break;
            }
            default: { // displaced
                UsdShadeShader disp = UsdShadeShader::Define(stage(), path.AppendChild(TfToken("disp")));
                disp.CreateIdAttr(VtValue(TfToken("ND_displacement_float")));
                disp.CreateInput(TfToken("displacement"), SdfValueTypeNames->Float).Set(uniform(0.02f, 0.15f));
                disp.CreateInput(TfToken("scale"), SdfValueTypeNames->Float).Set(1.0f);
                mat.CreateDisplacementOutput(TfToken("mtlx"))
                    .ConnectToSource(disp.CreateOutput(TfToken("out"), SdfValueTypeNames->Token));
                kind += ", displaced";
                break;
            }
            }
        }
        if (fresh)
            m_materials.push_back(path);
        return (fresh ? "add " : "remake ") + name(path) + " (" + kind + ")";
    }
    // One input of a material's shaders: a new value on what is there.
    std::string materialInput() {
        if (m_materials.empty())
            return "none";
        const SdfPath path = any(m_materials);
        std::vector<UsdAttribute> inputs;
        for (UsdPrim const &child : prim(path).GetChildren())
            for (UsdAttribute const &attr : child.GetAttributes()) {
                if (!TfStringStartsWith(attr.GetName().GetString(), "inputs:") || attr.HasAuthoredConnections())
                    continue;
                const SdfValueTypeName type = attr.GetTypeName();
                if (type == SdfValueTypeNames->Float || type == SdfValueTypeNames->Color3f)
                    inputs.push_back(attr);
            }
        if (inputs.empty())
            return "none";
        UsdAttribute attr = inputs[size_t(pick(int(inputs.size())))];
        if (attr.GetTypeName() == SdfValueTypeNames->Float) {
            float now = 0.0f;
            attr.Get(&now);
            attr.Set(now > 1.0f ? uniform(0.5f, 3.0f) : uniform(0.0f, 1.0f));
        } else {
            attr.Set(colourValue());
        }
        return "input " + attr.GetName().GetString() + " of " + name(path);
    }
    std::string binding() {
        SdfPath path = anyGprim();
        if (path.IsEmpty())
            return "none";
        if (m_materials.empty() || chance(0.25f)) {
            UsdShadeMaterialBindingAPI(prim(path)).UnbindAllBindings();
            return "unbind " + name(path);
        }
        const SdfPath material = any(m_materials);
        bind(path, material);
        return "bind " + name(material) + " to " + name(path);
    }
    std::string removeMaterial() { return removeFrom(m_materials, "material"); }

    std::string subset() {
        const SdfPath mesh = anyMesh();
        if (mesh.IsEmpty() || m_materials.empty())
            return "none";
        const SdfPath path = mesh.AppendChild(TfToken("part"));
        if (prim(path) && chance(0.3f)) {
            stage()->RemovePrim(path);
            return "remove subset of " + name(mesh);
        }
        UsdPrim sub = stage()->DefinePrim(path, TfToken("GeomSubset"));
        sub.CreateAttribute(TfToken("elementType"), SdfValueTypeNames->Token, SdfVariabilityUniform).Set(TfToken("face"));
        sub.CreateAttribute(TfToken("familyName"), SdfValueTypeNames->Token, SdfVariabilityUniform)
            .Set(TfToken("materialBind"));
        const int faceCount = faceCountOf(UsdGeomMesh(prim(mesh)));
        VtIntArray faces;
        for (int f = 0; f < faceCount; ++f)
            if (chance(0.4f))
                faces.push_back(f);
        sub.CreateAttribute(TfToken("indices"), SdfValueTypeNames->IntArray).Set(faces);
        const SdfPath material = any(m_materials);
        bind(path, material);
        return "subset of " + name(mesh) + " (" + std::to_string(faces.size()) + " faces, " + name(material) + ")";
    }

    // ---------------------------------------------------------------- lights
    void lightBasics(UsdPrim const &p) {
        const bool dome = p.GetTypeName() == TfToken("DomeLight"), distant = p.GetTypeName() == TfToken("DistantLight");
        const auto number = [&](const char *attr, float v) {
            p.CreateAttribute(TfToken(attr), SdfValueTypeNames->Float).Set(v);
        };
        number("inputs:intensity", dome ? uniform(0.2f, 0.8f) : distant ? uniform(0.5f, 2.0f) : uniform(1.0f, 6.0f));
        p.CreateAttribute(TfToken("inputs:color"), SdfValueTypeNames->Color3f)
            .Set(GfVec3f(uniform(0.5f, 1.0f), uniform(0.5f, 1.0f), uniform(0.5f, 1.0f)));
        number("inputs:width", uniform(0.3f, 1.5f));
        number("inputs:height", uniform(0.3f, 1.5f));
        number("inputs:radius", uniform(0.1f, 0.5f));
        number("inputs:length", uniform(0.3f, 1.5f));
        if (chance(0.3f))
            number("inputs:exposure", uniform(-1.0f, 1.0f));
        if (chance(0.3f)) {
            p.CreateAttribute(TfToken("inputs:enableColorTemperature"), SdfValueTypeNames->Bool).Set(chance(0.7f));
            number("inputs:colorTemperature", uniform(2500.0f, 9000.0f));
        }
        if (chance(0.3f))
            p.CreateAttribute(TfToken("inputs:normalize"), SdfValueTypeNames->Bool).Set(chance(0.5f));
        if (chance(0.2f))
            number("inputs:diffuse", uniform(0.0f, 1.5f));
        if (chance(0.2f))
            number("inputs:specular", uniform(0.0f, 1.5f));
        if (distant)
            number("inputs:angle", uniform(0.3f, 20.0f));
        if (chance(0.2f) && p.GetTypeName() == TfToken("SphereLight"))
            p.CreateAttribute(TfToken("treatAsPoint"), SdfValueTypeNames->Bool).Set(chance(0.5f));
    }
    void placeLight(UsdPrim const &p) {
        UsdGeomXformCommonAPI api(p);
        api.SetTranslate(GfVec3d(uniform(-1.5f, 1.5f), uniform(-1.5f, 1.5f), uniform(1.5f, 3.0f)));
        if (chance(0.5f))
            api.SetRotate(GfVec3f(uniform(-40.0f, 40.0f), uniform(-40.0f, 40.0f), uniform(0.0f, 90.0f)));
    }
    std::string addLight() {
        if (m_lights.size() >= 4)
            return removeFrom(m_lights, "light");
        static const char *const kinds[6] = {"RectLight",    "SphereLight", "DiskLight",
                                            "DistantLight", "DomeLight",   "CylinderLight"};
        const char *kind = kinds[pick(6)];
        const SdfPath path("/World/l" + std::to_string(++m_names));
        UsdPrim p = stage()->DefinePrim(path, TfToken(kind));
        lightBasics(p);
        placeLight(p);
        m_lights.push_back(path);
        return std::string("add ") + kind + " " + name(path);
    }
    std::string changeLight() {
        if (m_lights.empty())
            return "none";
        const SdfPath path = any(m_lights);
        lightBasics(prim(path));
        if (chance(0.5f))
            placeLight(prim(path));
        return "change " + name(path);
    }
    std::string lightShadow() {
        if (m_lights.empty())
            return "none";
        const SdfPath path = any(m_lights);
        UsdPrim p = prim(path);
        const auto number = [&](const char *attr, float v) {
            p.CreateAttribute(TfToken(attr), SdfValueTypeNames->Float).Set(v);
        };
        p.AddAppliedSchema(TfToken("ShadowAPI"));
        switch (pick(5)) {
        case 0:
            p.CreateAttribute(TfToken("inputs:shadow:enable"), SdfValueTypeNames->Bool).Set(chance(0.5f));
            return "shadow switch of " + name(path);
        case 1:
            p.CreateAttribute(TfToken("inputs:shadow:color"), SdfValueTypeNames->Color3f)
                .Set(chance(0.3f) ? GfVec3f(0.0f) : colourValue());
            return "shadow colour of " + name(path);
        case 2:
            number("inputs:shadow:distance", chance(0.3f) ? -1.0f : uniform(0.3f, 3.0f));
            return "shadow distance of " + name(path);
        case 3:
            number("inputs:shadow:falloff", chance(0.3f) ? -1.0f : uniform(0.2f, 2.0f));
            number("inputs:shadow:falloffGamma", uniform(0.5f, 2.0f));
            return "shadow falloff of " + name(path);
        default:
            for (const char *attr : {"inputs:shadow:enable", "inputs:shadow:color", "inputs:shadow:distance",
                                     "inputs:shadow:falloff", "inputs:shadow:falloffGamma"})
                p.RemoveProperty(TfToken(attr));
            return "no shadow settings on " + name(path);
        }
    }
    std::string lightShaping() {
        if (m_lights.empty())
            return "none";
        const SdfPath path = any(m_lights);
        UsdPrim p = prim(path);
        const auto number = [&](const char *attr, float v) {
            p.CreateAttribute(TfToken(attr), SdfValueTypeNames->Float).Set(v);
        };
        p.AddAppliedSchema(TfToken("ShapingAPI"));
        switch (pick(5)) {
        case 0:
            number("inputs:shaping:cone:angle", uniform(15.0f, 90.0f));
            number("inputs:shaping:cone:softness", uniform(0.0f, 0.5f));
            return "cone of " + name(path);
        case 1:
            number("inputs:shaping:focus", uniform(0.0f, 8.0f));
            return "focus of " + name(path);
        case 2: {
            const bool lobe = chance(0.5f);
            p.CreateAttribute(TfToken("inputs:shaping:ies:file"), SdfValueTypeNames->Asset)
                .Set(SdfAssetPath(std::string(NOVA_TEST_DIR) + (lobe ? "/data/ies/lobe.ies" : "/data/ies/downlight.ies")));
            p.CreateAttribute(TfToken("inputs:shaping:ies:normalize"), SdfValueTypeNames->Bool).Set(true);
            return "IES profile of " + name(path);
        }
        case 3:
            number("inputs:shaping:ies:angleScale", chance(0.3f) ? 0.0f : uniform(-1.5f, 1.5f));
            p.CreateAttribute(TfToken("inputs:shaping:ies:normalize"), SdfValueTypeNames->Bool).Set(chance(0.8f));
            return "IES scale of " + name(path);
        default:
            for (const char *attr : {"inputs:shaping:cone:angle", "inputs:shaping:cone:softness", "inputs:shaping:focus",
                                     "inputs:shaping:ies:file", "inputs:shaping:ies:angleScale",
                                     "inputs:shaping:ies:normalize"})
                p.RemoveProperty(TfToken(attr));
            return "no shaping on " + name(path);
        }
    }
    // Light and shadow links: the collections of a light over geometry, groups and instancers.
    std::string lightLink() {
        if (m_lights.empty())
            return "none";
        const SdfPath path = any(m_lights);
        UsdPrim p = prim(path);
        const char *which = chance(0.5f) ? "lightLink" : "shadowLink";
        const std::string base = std::string("collection:") + which;
        std::vector<SdfPath> candidates;
        for (std::vector<SdfPath> const *list : {&m_geometry, &m_groups, &m_instancers, &m_strands, &m_instances})
            for (SdfPath const &g : *list)
                if (prim(g))
                    candidates.push_back(g);
        candidates.push_back(SdfPath("/World/ground"));
        if (chance(0.2f)) {
            p.RemoveProperty(TfToken(base + ":includeRoot"));
            p.RemoveProperty(TfToken(base + ":includes"));
            p.RemoveProperty(TfToken(base + ":excludes"));
            return std::string("no ") + which + " on " + name(path);
        }
        SdfPathVector targets;
        for (SdfPath const &c : candidates)
            if (chance(0.35f))
                targets.push_back(c);
        const bool excluding = chance(0.5f);
        p.CreateAttribute(TfToken(base + ":includeRoot"), SdfValueTypeNames->Bool, SdfVariabilityUniform).Set(excluding);
        p.CreateRelationship(TfToken(base + (excluding ? ":excludes" : ":includes"))).SetTargets(targets);
        p.RemoveProperty(TfToken(base + (excluding ? ":includes" : ":excludes")));
        return std::string(which) + (excluding ? " without " : " of ") + std::to_string(targets.size()) + " on " +
               name(path);
    }
    std::string lightTexture() {
        std::vector<SdfPath> textured;
        for (SdfPath const &l : m_lights) {
            const TfToken type = prim(l).GetTypeName();
            if (type == TfToken("RectLight") || type == TfToken("DomeLight"))
                textured.push_back(l);
        }
        textured.push_back(SdfPath("/World/key"));
        const SdfPath path = any(textured);
        UsdPrim p = prim(path);
        if (chance(0.3f)) {
            p.RemoveProperty(TfToken("inputs:texture:file"));
            return "no texture on " + name(path);
        }
        const bool dome = p.GetTypeName() == TfToken("DomeLight");
        p.CreateAttribute(TfToken("inputs:texture:file"), SdfValueTypeNames->Asset)
            .Set(SdfAssetPath(imageFile(dome ? 3 : pick(2))));
        return "texture on " + name(path);
    }
    std::string lightVisibility() {
        if (m_lights.empty())
            return "none";
        return toggleVisibility(any(m_lights));
    }
    std::string removeLight() { return removeFrom(m_lights, "light"); }

    // ---------------------------------------------------------------- instancers
    std::string instancer() {
        const bool fresh = m_instancers.size() < 2 && (m_instancers.empty() || chance(0.4f));
        const SdfPath path = fresh ? SdfPath("/World/pi" + std::to_string(++m_names)) : any(m_instancers);
        if (!fresh && !usable(path))
            return "none";
        UsdPrim p = stage()->DefinePrim(path, TfToken("PointInstancer"));
        int prototypes = 1;
        if (fresh) {
            SdfPathVector targets;
            prototypes = 1 + pick(2);
            for (int i = 0; i < prototypes; ++i) {
                const SdfPath proto = path.AppendChild(TfToken("proto" + std::to_string(i)));
                define(proto);
                targets.push_back(proto);
                m_prototypes.push_back(proto);
            }
            p.CreateRelationship(TfToken("prototypes")).SetTargets(targets);
            m_instancers.push_back(path);
        } else {
            SdfPathVector targets;
            p.GetRelationship(TfToken("prototypes")).GetTargets(&targets);
            prototypes = int(targets.size());
        }
        const int count = pick(5); // also none
        VtVec3fArray positions, scales, colours;
        VtQuathArray orientations;
        VtIntArray indices;
        VtInt64Array invisible;
        for (int i = 0; i < count; ++i) {
            positions.push_back({uniform(-1.2f, 1.2f), uniform(-1.2f, 1.2f), uniform(-0.3f, 0.8f)});
            scales.push_back(GfVec3f(uniform(0.3f, 1.0f)));
            const float a = uniform(0.0f, 3.14f);
            orientations.push_back(GfQuath(GfHalf(std::cos(a * 0.5f)), GfHalf(0.0f), GfHalf(0.0f), GfHalf(std::sin(a * 0.5f))));
            indices.push_back(pick(std::max(prototypes, 1)));
            colours.push_back(colourValue());
            if (chance(0.15f))
                invisible.push_back(i);
        }
        for (const char *attr : {"positions", "velocities"})
            if (UsdAttribute a = p.GetAttribute(TfToken(attr)))
                a.Clear();
        p.RemoveProperty(TfToken("velocities"));
        p.CreateAttribute(TfToken("positions"), SdfValueTypeNames->Point3fArray).Set(positions);
        p.CreateAttribute(TfToken("scales"), SdfValueTypeNames->Float3Array).Set(scales);
        p.CreateAttribute(TfToken("protoIndices"), SdfValueTypeNames->IntArray).Set(indices);
        if (chance(0.5f))
            p.CreateAttribute(TfToken("orientations"), SdfValueTypeNames->QuathArray).Set(orientations);
        else
            p.RemoveProperty(TfToken("orientations"));
        if (chance(0.4f))
            UsdGeomPrimvarsAPI(p)
                .CreatePrimvar(TfToken("displayColor"), SdfValueTypeNames->Color3fArray, UsdGeomTokens->vertex)
                .Set(colours);
        else
            p.RemoveProperty(TfToken("primvars:displayColor"));
        if (!invisible.empty())
            p.CreateAttribute(TfToken("invisibleIds"), SdfValueTypeNames->Int64Array).Set(invisible);
        else
            p.RemoveProperty(TfToken("invisibleIds"));
        return (fresh ? "add " : "re-instance ") + name(path) + " x" + std::to_string(count) + " of " +
               std::to_string(prototypes);
    }
    std::string instancerEdit() {
        const SdfPath path = usableOf(m_instancers);
        if (path.IsEmpty())
            return "none";
        switch (pick(4)) {
        case 0: {
            const SdfPath proto = path.AppendChild(TfToken("proto0"));
            UsdPrim p = prim(proto);
            if (UsdGeomMesh mesh{p})
                return "reshape prototype of " + name(path) + " (" + shape(mesh) + ")";
            size(p);
            return "resize prototype of " + name(path);
        }
        case 1:
            place(path);
            return "move " + name(path);
        case 2: {
            if (m_materials.empty())
                return "none";
            const SdfPath material = any(m_materials);
            bind(chance(0.5f) ? path : path.AppendChild(TfToken("proto0")), material);
            return "bind " + name(material) + " to " + name(path);
        }
        default:
            return toggleVisibility(path);
        }
    }
    std::string removeInstancer() {
        if (m_instancers.empty())
            return "none";
        const SdfPath path = any(m_instancers);
        stage()->RemovePrim(path);
        drop(m_instancers, path);
        drop(m_prototypes, path);
        return "remove instancer " + name(path);
    }

    // Points or curves (linear and cubic, of every basis and wrap), or new ones in place of some.
    std::string strands() {
        const bool fresh = m_strands.size() < 3 && (m_strands.empty() || chance(0.5f));
        const SdfPath path =
            fresh ? parentForNew().AppendChild(TfToken("s" + std::to_string(++m_names))) : any(m_strands);
        if (!fresh && !usable(path))
            return "none";
        const bool curve = fresh ? chance(0.6f) : prim(path).GetTypeName() == TfToken("BasisCurves");
        UsdPrim p = stage()->DefinePrim(path, TfToken(curve ? "BasisCurves" : "Points"));
        static const char *const bases[3] = {"bezier", "bspline", "catmullRom"};
        static const char *const wraps[3] = {"nonperiodic", "pinned", "periodic"};
        const bool cubic = curve && chance(0.6f);
        const char *basis = bases[pick(3)];
        const char *wrap = cubic ? wraps[pick(3)] : "nonperiodic";
        int count = 2 + pick(4);
        if (cubic) // a count every basis and wrap takes: 4, 7 (Bezier's segments), 6 for a closed Bezier
            count = std::string(basis) == "bezier" ? (std::string(wrap) == "periodic" ? 6 : (chance(0.5f) ? 4 : 7))
                                                   : 4 + pick(3);
        VtVec3fArray points;
        VtFloatArray widths;
        for (int i = 0; i < count; ++i) {
            points.push_back({uniform(-1.2f, 1.2f), uniform(-1.2f, 1.2f), uniform(-0.2f, 0.8f)});
            widths.push_back(uniform(0.05f, 0.25f));
        }
        const bool oneWidth = chance(0.4f);
        if (oneWidth)
            widths.resize(1);
        for (const char *attr : {"points", "velocities"})
            if (UsdAttribute a = p.GetAttribute(TfToken(attr)))
                a.Clear();
        p.RemoveProperty(TfToken("velocities"));
        p.CreateAttribute(TfToken("points"), SdfValueTypeNames->Point3fArray).Set(points);
        UsdAttribute w = p.CreateAttribute(TfToken("widths"), SdfValueTypeNames->FloatArray);
        w.Set(widths);
        w.SetMetadata(TfToken("interpolation"), oneWidth ? UsdGeomTokens->constant : UsdGeomTokens->vertex);
        if (curve) {
            p.CreateAttribute(TfToken("type"), SdfValueTypeNames->Token, SdfVariabilityUniform)
                .Set(TfToken(cubic ? "cubic" : "linear"));
            p.CreateAttribute(TfToken("basis"), SdfValueTypeNames->Token, SdfVariabilityUniform).Set(TfToken(basis));
            p.CreateAttribute(TfToken("wrap"), SdfValueTypeNames->Token, SdfVariabilityUniform).Set(TfToken(wrap));
            p.CreateAttribute(TfToken("curveVertexCounts"), SdfValueTypeNames->IntArray).Set(VtIntArray{count});
        }
        if (fresh)
            m_strands.push_back(path);
        if (chance(0.5f))
            UsdGeomPrimvarsAPI(p)
                .CreatePrimvar(TfToken("displayColor"), SdfValueTypeNames->Color3fArray, UsdGeomTokens->constant)
                .Set(VtVec3fArray{colourValue()});
        else
            p.RemoveProperty(TfToken("primvars:displayColor"));
        p.RemoveProperty(TfToken("primvars:displayOpacity"));
        return (fresh ? "add " : "change ") +
               std::string(curve ? (cubic ? std::string(basis) + " " + wrap + " curve " : std::string("linear curve "))
                                 : std::string("points ")) +
               name(path) + " x" + std::to_string(count);
    }
    std::string removeStrands() { return removeFrom(m_strands, "strands"); }

    std::mt19937 m_rng;
    Rig m_rig;
    int m_names = 0;
    std::vector<std::string> m_only;
    std::map<std::string, int> m_done;
    std::vector<SdfPath> m_geometry, m_materials, m_lights, m_instancers, m_strands, m_groups, m_classes, m_instances;
    std::vector<SdfPath> m_prototypes; // gprims of instancers and classes
};

void render(nova::Renderer &r, const nova::Scene &scene, bool lightTree) {
    nova::RenderSettings settings;
    settings.maxSamples = 4;
    settings.maxDepth = 4;
    settings.lightTree = lightTree;
    r.setScene(&scene);
    r.setSettings(settings);
    r.setCamera(nova::test::makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.6f, 32, 32));
    r.resize(32, 32);
    nova::test::renderToConvergence(r);
}

size_t differing(const nova::Film &a, const nova::Film &b, float &largest) {
    size_t n = 0;
    largest = 0.0f;
    for (size_t i = 0; i < a.colorSum.size(); ++i) {
        const nova::Spectrum x = a.colorSum[i], y = b.colorSum[i];
        if (std::memcmp(&x, &y, sizeof(x)) != 0 || a.alphaSum[i] != b.alphaSum[i]) {
            ++n;
            largest = std::max({largest, std::fabs(x.x - y.x), std::fabs(x.y - y.y), std::fabs(x.z - y.z)});
        }
    }
    return n;
}

// A scene as lines of text, one per mesh and light, without the ids that
// depend on the order of arrival: what two scenes that render differently
// differ in.
template <class T> uint32_t digest(std::vector<T> const &values) {
    uint32_t h = 2166136261u;
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(values.data());
    for (size_t i = 0; i < values.size() * sizeof(T); ++i)
        h = (h ^ bytes[i]) * 16777619u;
    return h;
}

std::vector<std::string> describe(nova::Scene const &scene) {
    std::vector<std::string> lines;
    for (uint32_t id = 0; id < scene.meshCount(); ++id) {
        const nova::MeshRecord &rec = scene.meshRecord(id);
        if (!rec.alive)
            continue;
        const nova::MeshDesc &d = rec.desc;
        if (!d.visible) { // what a hidden mesh holds shows nowhere (it is brought up to date when it is shown)
            lines.push_back("mesh hidden, key " + std::to_string(rec.key));
            continue;
        }
        std::ostringstream s;
        s << "mesh key " << rec.key << " shape " << int(d.shape) << " points " << d.pointCount() << "x" << d.pointSteps << " #"
          << digest(d.points) << " tris " << d.triangleCount() << " #" << digest(d.indices) << " normals "
          << d.normals.size() << " #" << digest(d.normals) << " faces #" << digest(d.triangleFaces) << " params "
          << d.faceParameters.size() << " #" << digest(d.faceParameters) << " left " << d.leftHanded << " visible "
          << d.visible << " radius #" << digest(d.sphereRadius) << "/" << digest(d.curveRadius) << "/"
          << digest(d.pointRadius) << " curves " << d.curveVertexCounts.size() << " basis " << int(d.curveBasis)
          << " style " << int(d.curveStyle) << "/" << int(d.pointStyle) << " | instances " << rec.instances.size()
          << " #" << digest(rec.instances) << " keys " << rec.motionSteps << " #" << digest(rec.motionKeys)
          << " instanced " << rec.instanced << " | slots " << rec.materialSlots.size() << " triangle slots #"
          << digest(rec.triangleSlots) << " filtered " << rec.filtered << " caustics " << rec.fakeCaustics
          << " visibility " << int(rec.visibility) << " holdout " << int(rec.holdout) << " tag '" << rec.lpeTag
          << "' |";
        std::vector<std::string> attributes;
        for (const nova::MeshAttribute &a : d.attributes)
            attributes.push_back(" " + a.name + ":" + std::to_string(int(a.rate)) + ":" + std::to_string(int(a.width)) +
                                 ":" + std::to_string(a.data.size()) + "#" + std::to_string(digest(a.data)));
        for (const nova::InstanceAttribute &a : rec.instanceAttributes)
            attributes.push_back(" instance " + a.name + ":" + std::to_string(int(a.width)) + ":" +
                                 std::to_string(a.data.size()) + "#" + std::to_string(digest(a.data)));
        std::sort(attributes.begin(), attributes.end());
        for (std::string const &a : attributes)
            s << a;
        // (The categories' names are pxr's and follow the order the links were made in.)
        s << " | links " << rec.categories.size() << " sets #" << digest(rec.receiverSets) << " shadow #" << digest(rec.shadowMasks) << " | materials";
        for (nova::MaterialId m : rec.materialSlots) {
            if (!scene.hasMaterial(m)) {
                s << " none";
                continue;
            }
            const nova::MaterialRecord &mr = scene.material(m);
            s << " [uniforms " << mr.uniforms.size() << "#" << digest(mr.uniforms) << " varying " << mr.varying
              << " samplers " << mr.samplers.size() << " opacity " << mr.hasOpacity << " " << mr.uniformOpacity << " "
              << mr.opacityUniforms.size() << "#" << digest(mr.opacityUniforms) << " emission " << int(mr.emission)
              << " " << mr.uniformEmission.x << " " << mr.uniformEmission.y << " " << mr.uniformEmission.z << "]";
        }
        lines.push_back(s.str());
    }
    for (const nova::LightData &l : scene.lights()) {
        std::ostringstream s;
        s << "light type " << int(l.type) << " emission " << l.emission.x << " " << l.emission.y << " " << l.emission.z
          << " diffuse " << l.diffuse << " specular " << l.specular << " delta " << l.delta << " camera "
          << l.cameraVisible << " two-sided " << l.twoSided << " at " << l.position.x << " " << l.position.y << " "
          << l.position.z << " u " << l.axisU.x << " " << l.axisU.y << " " << l.axisU.z << " v " << l.axisV.x << " "
          << l.axisV.y << " " << l.axisV.z << " radius " << l.radius << " area " << l.area << " dir " << l.direction.x
          << " " << l.direction.y << " " << l.direction.z << " cone " << l.oneMinusCosThetaMax << " shaped " << l.shaped
          << " cut " << l.cosConeCut << " " << l.cosConeStart << " focus " << l.focus << " ies " << (l.ies != nullptr)
          << " " << l.iesWidth << "x" << l.iesHeight << " scale " << l.iesScale << " " << l.iesAngleScale << " mean "
          << l.iesMean << " texture " << (l.texels != nullptr) << " " << l.texWidth << "x" << l.texHeight << " lum "
          << l.texMeanLuminance << " dome " << l.domeX.x << " " << l.domeY.y << " " << l.domeZ.z << " portals "
          << l.portalCount << " selection " << l.selection << " equiangular " << l.equiangular << " outside "
          << l.outsideTree << " normal " << l.normal.x << " " << l.normal.y << " " << l.normal.z << " flip "
          << int(l.rectFlip) << " texels #"
          << digest(std::vector<nova::Spectrum>(l.texels, l.texels + (l.texels ? size_t(l.texWidth) * l.texHeight : 0)))
          << " shadow category " << l.shadowCategory << " casts " << l.castsShadows << " shaped " << l.shadowShaped
          << " tinted " << l.shadowTinted << " colour " << l.shadowColor.x << " " << l.shadowColor.y << " "
          << l.shadowColor.z << " distance " << l.shadowDistance << " falloff " << l.shadowFalloff << " "
          << l.shadowFalloffGamma << " background " << l.illumBackground << " filters " << l.filterCount
          << " triangle " << l.prim << " power " << l.power;
        lines.push_back(s.str());
    }
    std::sort(lines.begin(), lines.end());
    // What depends on the order: the lights as the samplers number them, and their links.
    std::ostringstream order;
    order << "light order";
    const nova::LightLinks links = scene.lightLinks();
    for (size_t i = 0; i < scene.lights().size(); ++i) {
        const nova::LightData &l = scene.lights()[i];
        order << " " << int(l.type) << "@" << l.position.x << "," << l.position.y << "," << l.position.z;
        if (links.light)
            order << " link " << links.light[i];
    }
    order << " | receiver sets " << scene.receiverSetCount() << " shadow linking " << scene.shadowLinking();
    lines.push_back(order.str());
    // The samplers' own state: the selection probabilities and the tree.
    std::ostringstream sampler;
    const nova::LightSamplerView table = scene.lightSampler();
    sampler << "light sampler columns " << table.count << " pmf";
    for (size_t i = 0; table.pmf && i < scene.lights().size(); ++i)
        sampler << " " << table.pmf[i];
    const nova::LightTreeView tree = scene.lightTree();
    sampler << " | tree nodes " << tree.nodeCount << " #"
            << digest(std::vector<nova::LightTreeNode>(tree.nodes, tree.nodes + (tree.nodes ? tree.nodeCount : 0)))
            << " infinite " << tree.infiniteCount;
    for (uint32_t i = 0; i < tree.infiniteCount; ++i)
        sampler << " " << tree.infinite[i] << ":" << tree.infiniteE[i];
    lines.push_back(sampler.str());
    return lines;
}

std::string difference(nova::Scene const &live, nova::Scene const &fresh) {
    const std::vector<std::string> a = describe(live), b = describe(fresh);
    std::ostringstream out;
    for (std::string const &line : a)
        if (std::find(b.begin(), b.end(), line) == b.end())
            out << "  edited only: " << line << "\n";
    for (std::string const &line : b)
        if (std::find(a.begin(), a.end(), line) == a.end())
            out << "  loaded only: " << line << "\n";
    if (out.str().empty())
        out << "  (the scenes' meshes and lights read the same)\n";
    return out.str();
}

struct Reach {
    int steps = 0, changed = 0;
    int treeShapes = 0; // steps whose scenes differed in the light tree's shape alone
    uint32_t meshes = 0, lights = 0, materials = 0;
    std::map<std::string, int> kinds;
};

// One seed: after each step of one to three edits the edited stage's scene
// against that of a new render index over the same stage. False at the first
// difference, with the story and the stage in `log`.
bool runSeed(uint32_t seed, int steps, std::string const &only, std::string &log, Reach &reach) {
    Host host(seed, only);
    std::mt19937 counts(seed ^ 0x5bd1e995u);
    std::ostringstream story;
    std::vector<nova::Spectrum> previous;
    bool same = true;
    for (int step = 0; step < steps && same; ++step) {
        story << "  step " << step << ":";
        for (int n = 1 + int(counts() % 3u); n > 0; --n)
            story << " [" << host.edit() << "]";
        story << "\n";
        host.rig().sync();
        const std::string text = host.rig().text();
        Rig loaded(text);
        const bool tree = (step & 1) != 0;
        nova::Renderer live, fresh;
        render(live, host.rig().scene(), tree);
        render(fresh, loaded.scene(), tree);
        float largest = 0.0f;
        size_t n = differing(live.film(), fresh.film(), largest);
        if (n != 0 && tree) {
            // A light whose values alone changed (a texture, a profile, its
            // power) keeps the shape of the light tree, which a new scene
            // builds for the new values (docs/design/M9.md D11): the two
            // trees then choose lights otherwise, with the same lights and
            // probabilities table. Such scenes must agree without the tree.
            const std::string what = difference(host.rig().scene(), loaded.scene());
            bool treeOnly = what.find("tree nodes") != std::string::npos;
            std::istringstream whatLines(what);
            for (std::string line; std::getline(whatLines, line);)
                treeOnly = treeOnly && line.find("light sampler") != std::string::npos;
            if (treeOnly) {
                nova::Renderer liveTable, freshTable;
                render(liveTable, host.rig().scene(), false);
                render(freshTable, loaded.scene(), false);
                n = differing(liveTable.film(), freshTable.film(), largest);
                reach.treeShapes += n == 0;
            }
        }
        if (n != 0) {
            story << "  -> " << n << " of " << live.film().colorSum.size() << " pixels differ (largest " << largest
                  << "); meshes " << host.rig().scene().meshCount() << " / " << loaded.scene().meshCount()
                  << ", lights " << host.rig().scene().lights().size() << " / " << loaded.scene().lights().size()
                  << "\n"
                  << difference(host.rig().scene(), loaded.scene());
            log = "seed " + std::to_string(seed) + ":\n" + story.str() + text;
            same = false;
            break;
        }
        const nova::Scene &scene = host.rig().scene();
        uint32_t alive = 0;
        for (uint32_t id = 0; id < scene.meshCount(); ++id)
            alive += scene.meshRecord(id).alive ? 1u : 0u;
        uint32_t materials = 0;
        for (uint32_t id = 1; id < scene.materialCount(); ++id)
            materials += scene.hasMaterial(id) ? 1u : 0u;
        reach.meshes = std::max(reach.meshes, alive);
        reach.lights = std::max(reach.lights, uint32_t(scene.lights().size()));
        reach.materials = std::max(reach.materials, materials);
        ++reach.steps;
        const std::vector<nova::Spectrum> &now = live.film().colorSum;
        reach.changed += previous.size() != now.size() ||
                         std::memcmp(previous.data(), now.data(), now.size() * sizeof(nova::Spectrum)) != 0;
        previous = now;
    }
    for (auto const &[kind, n] : host.done())
        reach.kinds[kind] += n;
    return same;
}

std::string kindsText(Reach const &reach) {
    std::ostringstream out;
    for (auto const &[kind, n] : reach.kinds)
        out << kind << " " << n << ", ";
    return out.str();
}

} // namespace

TEST_CASE("Edited stages of many features render as loaded ones", "[hydra][ipr][fuzz]") {
    Reach reach;
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        std::string log;
        const bool same = runSeed(seed, 10, "", log, reach);
        INFO(log);
        CHECK(same);
    }
    INFO(reach.changed << " of " << reach.steps << " steps changed the image; up to " << reach.meshes << " meshes, "
                       << reach.lights << " lights, " << reach.materials << " materials; " << kindsText(reach));
    CHECK(reach.changed * 2 > reach.steps);
    CHECK(reach.meshes >= 4);
    CHECK(reach.lights >= 2);
    CHECK(reach.materials >= 1);
}

// The hunt: NOVA_FUZZ_SEEDS seeds from NOVA_FUZZ_FIRST, NOVA_FUZZ_STEPS steps
// each. Every failing seed is reported, the first NOVA_FUZZ_SHOW with their stages.
TEST_CASE("Edited stages of many features render as loaded ones over many seeds", "[.][fuzz]") {
    const int seeds = environmentInt("NOVA_FUZZ_SEEDS", 50), steps = environmentInt("NOVA_FUZZ_STEPS", 15);
    const int first = environmentInt("NOVA_FUZZ_FIRST", 1000), show = environmentInt("NOVA_FUZZ_SHOW", 1);
    const std::string only = environmentText("NOVA_FUZZ_ONLY");
    int failed = 0;
    Reach reach;
    for (int i = 0; i < seeds; ++i) {
        std::string log;
        if (!runSeed(uint32_t(first + i), steps, only, log, reach)) {
            const size_t stage = log.find("#usda");
            WARN((++failed <= show ? log : log.substr(0, stage)));
        }
    }
    WARN(reach.changed << " of " << reach.steps << " steps changed the image; up to " << reach.meshes << " meshes, "
                       << reach.lights << " lights, " << reach.materials << " materials; " << reach.treeShapes
                       << " steps with another light tree shape; " << kindsText(reach));
    INFO(failed << " of " << seeds << " seeds differ");
    CHECK(failed == 0);
}

// ---------------------------------------------------------------------------
// What the fuzz found (docs/bug-audit-2026-11.md, section 23), each by itself.
// ---------------------------------------------------------------------------

namespace {

const char *kInstanced = R"(#usda 1.0
(
    upAxis = "Y"
)
class "_c"
{
    def Mesh "geo"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)]
        uniform token subdivisionScheme = "none"
    }
}
class "_outer"
{
    def Xform "inner" (
        instanceable = true
        prepend references = </_c>
    )
    {
    }
    def Sphere "ball"
    {
        double radius = 0.2
    }
    def PointInstancer "pi"
    {
        point3f[] positions = [(0, 0, 0.5), (1, 0, 0.5)]
        int[] protoIndices = [0, 0]
        rel prototypes = </_outer/pi/proto>
        def Cube "proto"
        {
            double size = 0.2
        }
    }
}
def Xform "World"
{
    def Xform "ni_b" (
        instanceable = true
        prepend references = </_c>
    )
    {
    }
    def Xform "ni_a" (
        instanceable = true
        prepend references = </_c>
    )
    {
        double3 xformOp:translate = (1.2, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Xform "nest2" (
        instanceable = true
        prepend references = </_outer>
    )
    {
        double3 xformOp:translate = (0, 1.2, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Xform "nest1" (
        instanceable = true
        prepend references = </_outer>
    )
    {
        double3 xformOp:translate = (0, -1.2, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def SphereLight "l"
    {
        float inputs:intensity = 5
        float inputs:radius = 0.3
        double3 xformOp:translate = (0, 0, 2)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
)";

bool hasKey(nova::Scene const &scene, std::string const &name) {
    const uint64_t key = nova::objectKey(name);
    for (uint32_t id = 0; id < scene.meshCount(); ++id)
        if (scene.meshRecord(id).alive && scene.meshRecord(id).key == key)
            return true;
    return false;
}

} // namespace

// pxr's light-linking index dirties the categories of the instance prim when
// a collection takes it in, and not the instancer whose instanceCategories
// hold them: the edit reached no renderer. HdNovaNoticeSceneIndex adds the
// instancer's notice.
TEST_CASE("A light link that takes a native instance in or out reaches its geometry", "[hydra][ipr][instancing]") {
    Rig rig(kInstanced);
    CHECK(rig.scene().receiverSetCount() <= 1); // no links yet
    UsdPrim light = rig.stage->GetPrimAtPath(SdfPath("/World/l"));
    light.CreateAttribute(TfToken("collection:lightLink:includeRoot"), SdfValueTypeNames->Bool, SdfVariabilityUniform)
        .Set(false);
    light.CreateRelationship(TfToken("collection:lightLink:includes")).SetTargets({SdfPath("/World/ni_a")});
    rig.sync();
    {
        Rig loaded(rig.text());
        INFO(difference(rig.scene(), loaded.scene()));
        CHECK(describe(rig.scene()) == describe(loaded.scene()));
        CHECK(loaded.scene().receiverSetCount() >= 2); // the linked instance, and the others
        CHECK(rig.scene().receiverSetCount() == loaded.scene().receiverSetCount());
    }
    // Another instance in its place, then none.
    light.GetRelationship(TfToken("collection:lightLink:includes")).SetTargets({SdfPath("/World/ni_b")});
    rig.sync();
    {
        Rig loaded(rig.text());
        INFO(difference(rig.scene(), loaded.scene()));
        CHECK(describe(rig.scene()) == describe(loaded.scene()));
    }
    light.GetRelationship(TfToken("collection:lightLink:includes")).SetTargets({});
    rig.sync();
    {
        Rig loaded(rig.text());
        INFO(difference(rig.scene(), loaded.scene()));
        CHECK(describe(rig.scene()) == describe(loaded.scene()));
    }
}

// USD numbers the prototypes of native instancing anew at every load
// (/__Prototype_N), and their prims reach Hydra below paths with those
// numbers: an object key made of such a path changed from run to run, and
// with it the stochastic opacity of instanced geometry, the order of
// instanced lights and emissive meshes, and the cryptomatte's names of
// instances in instances. The key is made of the first instance's place.
TEST_CASE("Prims of native instancing are keyed by their first instance", "[hydra][instancing][determinism]") {
    Rig rig(kInstanced);
    const HdSceneIndexBaseRefPtr terminal = rig.index->GetTerminalSceneIndex();
    size_t instanced = 0;
    for (SdfPath const &id : rig.index->GetRprimIds()) {
        const std::string stable = HdNovaStablePath(terminal, id);
        INFO(id << " -> " << stable);
        CHECK(stable.find("__Prototype_") == std::string::npos);
        instanced += id.GetString().find("__Prototype_") != std::string::npos;
    }
    CHECK(instanced >= 4); // the quad, and the quad, the ball and the cube of the nested prototype
    const nova::Scene &scene = rig.scene();
    CHECK(hasKey(scene, "/World/ni_a/geo"));                                  // the least of ni_a and ni_b
    CHECK(hasKey(scene, "/World/nest1/inner/geo"));                           // an instance in an instance
    CHECK(hasKey(scene, "/World/nest1/ball"));
    CHECK(hasKey(scene, "/World/nest1/pi/proto/ForInstancer{/World/nest1/pi}")); // a point instancer's prototype in one
    CHECK_FALSE(hasKey(scene, "/World/ni_b/geo"));

    // A new first instance names the prototype: the object is keyed anew, as a loaded stage keys it.
    UsdPrim first = rig.stage->DefinePrim(SdfPath("/World/n0"), TfToken("Xform"));
    first.GetReferences().AddInternalReference(SdfPath("/_c"));
    first.SetInstanceable(true);
    rig.sync();
    CHECK(hasKey(rig.scene(), "/World/n0/geo"));
    CHECK_FALSE(hasKey(rig.scene(), "/World/ni_a/geo"));
    Rig loaded(rig.text());
    CHECK(hasKey(loaded.scene(), "/World/n0/geo"));
    INFO(difference(rig.scene(), loaded.scene()));
    CHECK(describe(rig.scene()) == describe(loaded.scene()));
}

// Normals per face (uniform) were ignored: the faces were shaded by their
// own geometry. Karma shades by them (measured).
TEST_CASE("Normals per face reach the mesh", "[hydra][normals]") {
    Rig rig(R"(#usda 1.0
def Mesh "tent"
{
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(-2, 0, 1), (0, 1, 1), (2, 0, 1), (-2, 0, -1), (0, 1, -1), (2, 0, -1)]
    normal3f[] normals = [(-0.6, 0.8, 0), (0.6, 0.8, 0)] (
        interpolation = "uniform"
    )
    uniform token subdivisionScheme = "none"
}
)");
    const nova::Scene &scene = rig.scene();
    const nova::MeshDesc *mesh = nullptr;
    for (uint32_t id = 0; id < scene.meshCount(); ++id)
        if (scene.meshRecord(id).alive)
            mesh = &scene.meshRecord(id).desc;
    REQUIRE(mesh);
    REQUIRE(mesh->triangleCount() == 4);
    REQUIRE(mesh->normals.size() == mesh->pointCount());
    // Every corner has the normal of its face: the left face's points have x <= 0.
    for (size_t i = 0; i < mesh->pointCount(); ++i) {
        const nova::Vec3f p = mesh->points[i], n = mesh->normals[i];
        const size_t t = i / 3; // one point per triangle corner
        float centre = 0.0f;
        for (size_t c = 0; c < 3; ++c)
            centre += mesh->points[mesh->indices[t * 3 + c]].x;
        INFO("corner " << i << " at x " << p.x);
        CHECK(n.x == (centre < 0.0f ? -0.6f : 0.6f));
        CHECK(n.y == 0.8f);
    }
}

// interpolateBoundary "none" makes the faces at a boundary holes: a mesh whose
// faces all touch its boundary is nothing. Its control mesh was rendered.
TEST_CASE("A subdivision surface of holes alone renders nothing", "[hydra][subdiv]") {
    const auto triangles = [](const char *boundary) {
        Rig rig(std::string(R"(#usda 1.0
def Mesh "grid"
{
    int[] faceVertexCounts = [4, 4, 4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4, 3, 4, 7, 6, 4, 5, 8, 7]
    point3f[] points = [(-1, -1, 0), (0, -1, 0), (1, -1, 0), (-1, 0, 0), (0, 0, 0.5), (1, 0, 0), (-1, 1, 0), (0, 1, 0), (1, 1, 0)]
    uniform token subdivisionScheme = "catmullClark"
    token interpolateBoundary = ")") + boundary + "\"\n}\n");
        size_t n = 0;
        for (uint32_t id = 0; id < rig.scene().meshCount(); ++id)
            if (rig.scene().meshRecord(id).alive)
                n += rig.scene().meshRecord(id).desc.triangleCount();
        return n;
    };
    CHECK(triangles("edgeAndCorner") == 4 * 128); // level 3: 64 quads a face
    CHECK(triangles("none") == 0);
}

// The Light LOP's type "Line" is a CylinderLight with treatAsLine, which was
// not read: the light was the cylinder of its radius (19 times darker than
// Karma's line at a radius of 0.1). Karma's law is in tests/test_line_light.cpp.
TEST_CASE("A cylinder light with treatAsLine is a line light", "[hydra][lights][line]") {
    Rig rig(R"(#usda 1.0
def CylinderLight "line"
{
    float inputs:intensity = 10
    float inputs:exposure = 1
    color3f inputs:color = (1, 0.5, 0.25)
    float inputs:radius = 0.2
    float inputs:length = 3
    bool inputs:normalize = 1
    bool treatAsLine = 1
    double3 xformOp:translate = (0, 2, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)");
    REQUIRE(rig.scene().lights().size() == 1);
    {
        const nova::LightData &l = rig.scene().lights()[0];
        CHECK(l.type == nova::LightType::Cylinder);
        CHECK(l.delta);
        CHECK_FALSE(l.cameraVisible);
        CHECK(l.emission.x == 20.0f); // colour x intensity x 2^exposure, whatever radius and normalize say
        CHECK(l.emission.y == 10.0f);
        CHECK(l.emission.z == 5.0f);
        CHECK(std::fabs(nova::length(l.axisU) - 1.5f) < 1e-6f);
    }
    // Without the switch it is the cylinder. (A new stage: UsdImaging sends no notice when
    // treatAsLine or treatAsPoint change, which are no inputs of the light, so a running
    // render does not follow that edit; docs/limitations.md.)
    rig.stage->GetPrimAtPath(SdfPath("/line")).GetAttribute(TfToken("treatAsLine")).Set(false);
    Rig cylinder(rig.text());
    REQUIRE(cylinder.scene().lights().size() == 1);
    CHECK_FALSE(cylinder.scene().lights()[0].delta);
    CHECK(cylinder.scene().lights()[0].radius == 0.2f);
}

// UsdLux MeshLightAPI. In Karma a mesh with the API is a light: it emits the
// light's colour times its intensity as radiance, whatever material is bound
// to it, and is sampled as a light (measured). In Nova the mesh rendered with
// no material at all: the prim's own `material` data source, which holds the
// light's parameters, stood in for its binding.
TEST_CASE("A mesh with MeshLightAPI lights by its material's glow times its light's colour", "[hydra][lights][meshlight]") {
    Rig rig(R"(#usda 1.0
def Mesh "card" (
    prepend apiSchemas = ["MeshLightAPI", "MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)]
    uniform token subdivisionScheme = "none"
    float inputs:intensity = 3
    color3f inputs:color = (0.2, 1, 0.2)
    bool inputs:normalize = 1
    rel material:binding = </glow>
}
def Material "glow"
{
    token outputs:surface.connect = </glow/s.outputs:surface>
    def Shader "s"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:emissiveColor = (9, 0, 0)
        token outputs:surface
    }
}
)");
    const auto card = [](nova::Scene const &scene) -> nova::MeshRecord const * {
        for (uint32_t id = 0; id < scene.meshCount(); ++id)
            if (scene.meshRecord(id).alive)
                return &scene.meshRecord(id);
        return nullptr;
    };
    const auto emission = [&](nova::Scene const &scene) {
        const nova::MeshRecord *rec = card(scene);
        REQUIRE(rec);
        REQUIRE(rec->materialSlots.size() == 1);
        REQUIRE(scene.hasMaterial(rec->materialSlots[0]));
        return scene.material(rec->materialSlots[0]).uniformEmission;
    };
    const auto tint = [&](nova::Scene const &scene) {
        REQUIRE(scene.lights().size() == 2); // its two triangles are lights
        REQUIRE(scene.lights()[0].type == nova::LightType::Triangle);
        CHECK(scene.lights()[0].tint.x == scene.lights()[1].tint.x);
        return scene.lights()[0].tint;
    };
    const auto asLoaded = [&] {
        Rig loaded(rig.text());
        INFO(difference(rig.scene(), loaded.scene()));
        CHECK(describe(rig.scene()) == describe(loaded.scene()));
        const nova::Spectrum a = tint(rig.scene()), b = tint(loaded.scene());
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
        CHECK(a.z == b.z);
        const nova::Spectrum ea = emission(rig.scene()), eb = emission(loaded.scene());
        CHECK(ea.x == eb.x);
        CHECK(ea.y == eb.y);
        CHECK(ea.z == eb.z);
    };
    {
        // The material's glow is what the camera sees; the light's colour times its intensity tints its light.
        const nova::Spectrum e = emission(rig.scene());
        CHECK(e.x == 9.0f);
        CHECK(e.y == 0.0f);
        CHECK(e.z == 0.0f);
        const nova::Spectrum t = tint(rig.scene());
        CHECK(t.x == 0.6f);
        CHECK(t.y == 3.0f);
        CHECK(t.z == 0.6f);
    }
    // The light's values edited: they reach the mesh (Hydra tells an rprim nothing of its `light`).
    UsdPrim prim = rig.stage->GetPrimAtPath(SdfPath("/card"));
    prim.GetAttribute(TfToken("inputs:intensity")).Set(5.0f);
    prim.CreateAttribute(TfToken("inputs:exposure"), SdfValueTypeNames->Float).Set(1.0f);
    rig.sync();
    {
        const nova::Spectrum t = tint(rig.scene());
        CHECK(t.x == 2.0f);
        CHECK(t.y == 10.0f);
        CHECK(t.z == 2.0f);
        CHECK(emission(rig.scene()).x == 9.0f);
        asLoaded();
    }
    // Without a material its glow is white, which the light's colour tints.
    UsdRelationship binding = prim.GetRelationship(TfToken("material:binding"));
    binding.SetTargets({});
    rig.sync();
    {
        const nova::Spectrum e = emission(rig.scene());
        CHECK(e.x == 1.0f);
        CHECK(e.y == 1.0f);
        CHECK(e.z == 1.0f);
        const nova::Spectrum t = tint(rig.scene());
        CHECK(t.x == 2.0f);
        CHECK(t.y == 10.0f);
        asLoaded();
    }
    binding.SetTargets({SdfPath("/glow")});
    rig.sync();
    CHECK(emission(rig.scene()).x == 9.0f);
    CHECK(emission(rig.scene()).y == 0.0f);
    asLoaded();
    // noMaterialResponse: the light's colour is all it emits, to the camera too. (A stage loaded so: whether a
    // change of the mode alone is told to Hydra is UsdImaging's.)
    prim.CreateAttribute(TfToken("light:materialSyncMode"), SdfValueTypeNames->Token, false, SdfVariabilityUniform)
        .Set(TfToken("noMaterialResponse"));
    {
        Rig loaded(rig.text());
        const nova::Spectrum e = emission(loaded.scene());
        CHECK(e.x == 2.0f);
        CHECK(e.y == 10.0f);
        CHECK(e.z == 2.0f);
        const nova::Spectrum t = tint(loaded.scene());
        CHECK(t.x == 1.0f);
        CHECK(t.y == 1.0f);
        CHECK(t.z == 1.0f);
    }
    prim.RemoveProperty(TfToken("light:materialSyncMode"));
    // Without the API it is the mesh of its material again.
    prim.RemoveAppliedSchema(TfToken("MeshLightAPI"));
    rig.sync();
    {
        const nova::Spectrum e = emission(rig.scene());
        CHECK(e.x == 9.0f);
        CHECK(e.y == 0.0f);
        for (nova::LightData const &light : rig.scene().lights())
            CHECK(light.tint.y == 1.0f);
        Rig loaded(rig.text());
        INFO(difference(rig.scene(), loaded.scene()));
        CHECK(describe(rig.scene()) == describe(loaded.scene()));
    }
}

TEST_CASE("Polygons that are not flat are shaded by their face's normal", "[hydra][normals]") {
    const auto only = [](nova::Scene const &scene) -> nova::MeshDesc const & {
        for (uint32_t id = 0; id < scene.meshCount(); ++id)
            if (scene.meshRecord(id).alive)
                return scene.meshRecord(id).desc;
        FAIL("no mesh");
        static nova::MeshDesc none;
        return none;
    };
    const auto stage = [](const char *points, const char *counts, const char *indices, std::string const &extra) {
        return std::string("#usda 1.0\ndef Mesh \"m\"\n{\n    int[] faceVertexCounts = ") + counts +
               "\n    int[] faceVertexIndices = " + indices + "\n    point3f[] points = " + points + "\n" + extra + "}\n";
    };
    const std::string polygons = "    uniform token subdivisionScheme = \"none\"\n";
    // A flat quad needs none: its triangles' normals are the face's.
    {
        Rig rig(stage("[(-1, 0, 1), (1, 0, 1), (1, 0, -1), (-1, 0, -1)]", "[4]", "[0, 1, 2, 3]", polygons));
        CHECK(only(rig.scene()).normals.empty());
        CHECK(only(rig.scene()).pointCount() == 4);
    }
    // A saddle: two triangles of other normals, one normal for both (0, 1, 0 by its symmetry).
    {
        Rig rig(stage("[(-2, 0.2, 1.5), (2, 1.8, 1.5), (2, 0.2, -1.5), (-2, 1.8, -1.5)]", "[4]", "[0, 1, 2, 3]", polygons));
        const nova::MeshDesc &m = only(rig.scene());
        REQUIRE(m.triangleCount() == 2);
        REQUIRE(m.normals.size() == m.pointCount());
        for (const nova::Vec3f &n : m.normals) {
            CHECK(std::fabs(n.x) < 1e-6f);
            CHECK(std::fabs(n.y - 1.0f) < 1e-6f);
            CHECK(std::fabs(n.z) < 1e-6f);
        }
    }
    // A bent pentagon next to a flat triangle: Newell's normal on the one, the triangle's own on the other;
    // listed clockwise (leftHanded) the same normals.
    for (const bool leftHanded : {false, true}) {
        INFO("left-handed " << leftHanded);
        const std::string extra = polygons + (leftHanded ? "    uniform token orientation = \"leftHanded\"\n" : "");
        Rig rig(stage("[(-2, 0.2, 1.2), (0, 0.9, 2), (2, 0.2, 1.2), (1.3, 1.5, -1.5), (-1.3, 0.3, -1.5), (4, 0, 0), (5, 0, 0), (4, 0, -1)]",
                      "[5, 3]", leftHanded ? "[4, 3, 2, 1, 0, 7, 6, 5]" : "[0, 1, 2, 3, 4, 5, 6, 7]", extra));
        const nova::MeshDesc &m = only(rig.scene());
        REQUIRE(m.triangleCount() == 4);
        REQUIRE(m.normals.size() == m.pointCount());
        const GfVec3d p[5] = {{-2, 0.2, 1.2}, {0, 0.9, 2}, {2, 0.2, 1.2}, {1.3, 1.5, -1.5}, {-1.3, 0.3, -1.5}};
        GfVec3d newell(0.0);
        for (int i = 0; i < 5; ++i)
            newell += GfCross(p[i], p[(i + 1) % 5]);
        newell.Normalize();
        int pentagon = 0, triangle = 0;
        for (size_t i = 0; i < m.pointCount(); ++i) {
            const nova::Vec3f n = m.normals[i];
            if (m.points[i].x > 3.0f) { // the triangle, flat on the ground
                CHECK(std::fabs(n.y - 1.0f) < 1e-6f);
                ++triangle;
            } else {
                CHECK(std::fabs(n.x - float(newell[0])) < 1e-5f);
                CHECK(std::fabs(n.y - float(newell[1])) < 1e-5f);
                CHECK(std::fabs(n.z - float(newell[2])) < 1e-5f);
                ++pentagon;
            }
        }
        CHECK(pentagon == 9);
        CHECK(triangle == 3);
    }
    // Authored normals stay what they are; on a mesh of the bilinear scheme Karma does not read them.
    {
        const std::string normals = "    normal3f[] normals = [(0.6, 0.8, 0), (0.6, 0.8, 0), (0.6, 0.8, 0), (0.6, 0.8, 0)] (\n"
                                    "        interpolation = \"vertex\"\n    )\n";
        Rig authored(stage("[(-2, 0.2, 1.5), (2, 1.8, 1.5), (2, 0.2, -1.5), (-2, 1.8, -1.5)]", "[4]", "[0, 1, 2, 3]",
                           polygons + normals));
        REQUIRE(only(authored.scene()).normals.size() == 4);
        CHECK(only(authored.scene()).normals[0].x == 0.6f);
        Rig bilinear(stage("[(-1, 0, 1), (1, 0, 1), (1, 0, -1), (-1, 0, -1)]", "[4]", "[0, 1, 2, 3]",
                           "    uniform token subdivisionScheme = \"bilinear\"\n" + normals));
        CHECK(only(bilinear.scene()).normals.empty());
    }
}

// displayOpacity per instance on geometry without a material: Karma renders every instance see-through by its own
// value (a point instancer's primvar, a native instance's own), so such a prototype has the default material that
// reads the primvar (section 23).
TEST_CASE("An opacity per instance makes prototypes without a material see-through", "[hydra][materials][opacity][instances]") {
    Rig rig(R"(#usda 1.0
def PointInstancer "pi"
{
    point3f[] positions = [(-3, 0, 0), (-1, 0, 0), (1, 0, 0)]
    int[] protoIndices = [0, 0, 0]
    rel prototypes = </pi/proto>
    float[] primvars:displayOpacity = [1, 0.5, 0.2] (
        interpolation = "vertex"
    )
    def Mesh "proto"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-0.5, 0, 0.5), (0.5, 0, 0.5), (0.5, 0, -0.5), (-0.5, 0, -0.5)]
        uniform token subdivisionScheme = "none"
    }
}
def PointInstancer "plain"
{
    point3f[] positions = [(-3, 0, 4), (-1, 0, 4)]
    int[] protoIndices = [0, 0]
    rel prototypes = </plain/proto>
    def Sphere "proto"
    {
        double radius = 0.5
    }
}
def Xform "card"
{
    def Mesh "geo"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-0.5, 0, 0.5), (0.5, 0, 0.5), (0.5, 0, -0.5), (-0.5, 0, -0.5)]
        uniform token subdivisionScheme = "none"
    }
}
def Xform "a" (
    instanceable = true
    prepend references = </card>
)
{
    double3 xformOp:translate = (0, 0, 8)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def Xform "b" (
    instanceable = true
    prepend references = </card>
)
{
    float[] primvars:displayOpacity = [0.5]
    double3 xformOp:translate = (2, 0, 8)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)");
    // The scene's meshes by how many instances they have: the instancer's three cards, the two spheres, and the
    // native instances' card (two instances) or cards.
    const auto veiled = [](nova::Scene const &scene, nova::MeshRecord const &rec) {
        return rec.materialSlots.size() == 1 && scene.hasMaterial(rec.materialSlots[0]) &&
               scene.material(rec.materialSlots[0]).hasOpacity;
    };
    const auto count = [&](nova::Scene const &scene, bool seeThrough) {
        int n = 0;
        for (uint32_t id = 0; id < scene.meshCount(); ++id)
            if (scene.meshRecord(id).alive && veiled(scene, scene.meshRecord(id)) == seeThrough)
                ++n;
        return n;
    };
    const auto find = [&](nova::Scene const &scene, nova::MeshShape shape, size_t instances) -> nova::MeshRecord const * {
        for (uint32_t id = 0; id < scene.meshCount(); ++id) {
            nova::MeshRecord const &rec = scene.meshRecord(id);
            if (rec.alive && rec.desc.shape == shape && rec.instances.size() == instances)
                return &rec;
        }
        return nullptr;
    };
    {
        nova::MeshRecord const *cards = find(rig.scene(), nova::MeshShape::Triangles, 3);
        REQUIRE(cards);
        CHECK(veiled(rig.scene(), *cards));
        CHECK(cards->filtered);
        nova::MeshRecord const *spheres = find(rig.scene(), nova::MeshShape::Sphere, 2);
        REQUIRE(spheres);
        CHECK_FALSE(veiled(rig.scene(), *spheres)); // no opacity anywhere: the plain default material
        CHECK(spheres->materialSlots.empty());
        // The native instances: the one with an opacity of its own is see-through, in whatever way UsdImaging
        // groups them.
        CHECK(count(rig.scene(), true) >= 2);
    }
    // The instancer's primvar gone: opaque again, as a stage loaded so.
    rig.stage->GetAttributeAtPath(SdfPath("/pi.primvars:displayOpacity")).Block();
    rig.sync();
    {
        nova::MeshRecord const *cards = find(rig.scene(), nova::MeshShape::Triangles, 3);
        REQUIRE(cards);
        CHECK_FALSE(veiled(rig.scene(), *cards));
        Rig loaded(rig.text());
        INFO(difference(rig.scene(), loaded.scene()));
        CHECK(describe(rig.scene()) == describe(loaded.scene()));
    }
    // An opacity per instance authored on the other instancer.
    UsdAttribute opacity = rig.stage->GetPrimAtPath(SdfPath("/plain"))
                               .CreateAttribute(TfToken("primvars:displayOpacity"), SdfValueTypeNames->FloatArray);
    opacity.Set(VtFloatArray{0.25f, 0.75f});
    opacity.SetMetadata(TfToken("interpolation"), TfToken("vertex"));
    rig.sync();
    {
        nova::MeshRecord const *spheres = find(rig.scene(), nova::MeshShape::Sphere, 2);
        REQUIRE(spheres);
        CHECK(veiled(rig.scene(), *spheres));
        Rig loaded(rig.text());
        INFO(difference(rig.scene(), loaded.scene()));
        CHECK(describe(rig.scene()) == describe(loaded.scene()));
    }
}

// A light's link taken away in the same step that resyncs the light (an API schema applied to it: a Light LOP
// that turns its shaping on while a Light Linker LOP is bypassed). pxr's light-linking scene index then kept
// the light's old category (section 23).
TEST_CASE("A light link removed in the step that resyncs its light is gone", "[hydra][lights][links]") {
    for (const char *which : {"lightLink", "shadowLink"}) {
        for (int resync = 0; resync < 3; ++resync) {
            CAPTURE(which, resync);
            Rig rig(kStart);
            UsdPrim light = rig.stage->GetPrimAtPath(SdfPath("/World/key"));
            REQUIRE(light);
            const std::string base = std::string("collection:") + which;
            light.CreateAttribute(TfToken(base + ":includeRoot"), SdfValueTypeNames->Bool, SdfVariabilityUniform).Set(false);
            light.CreateRelationship(TfToken(base + ":includes")).SetTargets({SdfPath("/World/ground")});
            rig.sync();
            const auto category = [&](Rig &r) {
                const HdSceneIndexPrim prim = r.index->GetTerminalSceneIndex()->GetPrim(SdfPath("/World/key"));
                const HdContainerDataSourceHandle data = HdLightSchema::GetFromParent(prim.dataSource).GetContainer();
                const HdSampledDataSourceHandle value =
                    data ? HdSampledDataSource::Cast(data->Get(TfToken(which))) : HdSampledDataSourceHandle();
                const VtValue v = value ? value->GetValue(0.0f) : VtValue();
                return v.IsHolding<TfToken>() ? v.UncheckedGet<TfToken>().GetString() : std::string();
            };
            CHECK_FALSE(category(rig).empty());
            {
                Rig loaded(rig.text());
                INFO(difference(rig.scene(), loaded.scene()));
                CHECK(describe(rig.scene()) == describe(loaded.scene()));
            }
            // The link goes; with it, an edit that resyncs the prim (1: before the removal, 2: after it).
            if (resync == 1)
                light.AddAppliedSchema(TfToken("ShapingAPI"));
            light.RemoveProperty(TfToken(base + ":includeRoot"));
            light.RemoveProperty(TfToken(base + ":includes"));
            if (resync == 2)
                light.AddAppliedSchema(TfToken("ShadowAPI"));
            rig.sync();
            CHECK(category(rig).empty());
            Rig loaded(rig.text());
            CHECK(category(loaded).empty());
            INFO(difference(rig.scene(), loaded.scene()));
            CHECK(describe(rig.scene()) == describe(loaded.scene()));
        }
    }
}
