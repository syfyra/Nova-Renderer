// Random USD edits through the delegate against a freshly loaded stage (the
// viewport's case). A stage that is edited step by step (meshes and implicit
// spheres added, reshaped, moved, hidden, deactivated and removed; display
// colours; materials made, changed, bound and unbound; lights added, changed
// and removed) reaches Nova's scene through UsdImaging and the delegate's
// syncs. After every step that scene must render what the scene of a new
// render index over the same stage renders, bit for bit: a sync may not
// leave anything of an earlier state behind, and may not miss a change. The
// core's counterpart, without Hydra, is tests/test_ipr_fuzz.cpp.
//
// The default runs a few seeds; "[.][fuzz]" runs NOVA_FUZZ_SEEDS seeds of
// NOVA_FUZZ_STEPS steps from NOVA_FUZZ_FIRST.

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"
#include "hydra/resyncSceneIndex.h"
#include "hydra/velocitySceneIndex.h"

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
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int environmentInt(const char *name, int fallback) {
    char *value = nullptr;
    size_t size = 0;
    const bool has = _dupenv_s(&value, &size, name) == 0 && value && *value;
    const int result = has ? std::atoi(value) : fallback;
    std::free(value);
    return result;
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

// A stage through UsdImaging and the renderer plugin's scene indices into a
// render index of its own, synced into the delegate's scene.
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
        const std::string instance = "novaFuzz" + std::to_string(++counter);
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(text);
        stage = UsdStage::Open(layer);
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector(), instance));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        sceneIndices.stageSceneIndex->SetTime(UsdTimeCode(0.0));
        index->InsertSceneIndex(
            HdNovaResyncSceneIndex::New(HdNovaVelocityModeSceneIndex::Append(sceneIndices.finalSceneIndex), instance),
            SdfPath::AbsoluteRootPath(), false);
        pass = delegate->CreateRenderPass(index.get(),
                                          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        tasks.push_back(std::make_shared<SyncTask>(pass));
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
    nova::Scene &scene() { return static_cast<HdNovaRenderParam *>(delegate->GetRenderParam())->GetScene(); }
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
    explicit Host(uint32_t seed) : m_rng(seed), m_rig(kStart) {}

    Rig &rig() { return m_rig; }

    std::string edit() {
        UsdStageRefPtr stage = m_rig.stage;
        switch (pick(23)) {
        case 0:
        case 1: { // a mesh or an implicit sphere
            if (m_geometry.size() >= 8)
                return remove(m_geometry, "geometry");
            const SdfPath path("/World/g" + std::to_string(++m_names));
            if (chance(0.25f)) {
                UsdGeomSphere sphere = UsdGeomSphere::Define(stage, path);
                sphere.CreateRadiusAttr().Set(double(uniform(0.15f, 0.4f)));
            } else {
                UsdGeomMesh mesh = UsdGeomMesh::Define(stage, path);
                mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
                shape(mesh);
            }
            place(path);
            if (chance(0.6f))
                colour(path);
            m_geometry.push_back(path);
            if (!m_materials.empty() && chance(0.6f))
                bind(path, m_materials[size_t(pick(int(m_materials.size())))]);
            return "add " + path.GetName();
        }
        case 2: { // new points or a new radius
            if (m_geometry.empty())
                return "none";
            const SdfPath path = any(m_geometry);
            UsdPrim prim = stage->GetPrimAtPath(path);
            if (UsdGeomMesh mesh{prim})
                shape(mesh);
            else
                UsdGeomSphere(prim).CreateRadiusAttr().Set(double(uniform(0.15f, 0.4f)));
            return "reshape " + path.GetName();
        }
        case 3: {
            if (m_geometry.empty())
                return "none";
            const SdfPath path = any(m_geometry);
            place(path);
            return "move " + path.GetName();
        }
        case 4: { // visibility
            if (m_geometry.empty())
                return "none";
            const SdfPath path = any(m_geometry);
            UsdGeomImageable imageable(stage->GetPrimAtPath(path));
            TfToken now;
            imageable.GetVisibilityAttr().Get(&now);
            if (now == UsdGeomTokens->invisible)
                imageable.MakeVisible();
            else
                imageable.MakeInvisible();
            return (now == UsdGeomTokens->invisible ? "show " : "hide ") + path.GetName();
        }
        case 5: { // deactivated prims leave the scene and come back
            if (m_geometry.empty())
                return "none";
            const SdfPath path = any(m_geometry);
            UsdPrim prim = stage->GetPrimAtPath(path);
            const bool active = prim.IsActive();
            prim.SetActive(!active);
            return (active ? "deactivate " : "activate ") + path.GetName();
        }
        case 6:
            return remove(m_geometry, "geometry");
        case 7: {
            if (m_geometry.empty())
                return "none";
            const SdfPath path = any(m_geometry);
            colour(path);
            return "colour " + path.GetName();
        }
        case 8:
        case 9: { // a material, or new parameters for one
            const bool fresh = m_materials.size() < 5 && (m_materials.empty() || chance(0.5f));
            const SdfPath path = fresh ? SdfPath("/World/Looks/m" + std::to_string(++m_names)) : any(m_materials);
            const std::string kind = surface(path);
            if (fresh)
                m_materials.push_back(path);
            return (fresh ? "add " : "change ") + path.GetName() + " (" + kind + ")";
        }
        case 10:
        case 11: { // a binding
            if (m_geometry.empty())
                return "none";
            const SdfPath path = any(m_geometry);
            if (m_materials.empty() || chance(0.25f)) {
                UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(path)).UnbindAllBindings();
                return "unbind " + path.GetName();
            }
            const SdfPath material = any(m_materials);
            bind(path, material);
            return "bind " + material.GetName() + " to " + path.GetName();
        }
        case 12:
            return remove(m_materials, "material");
        case 13: { // a light
            if (m_lights.size() >= 4)
                return remove(m_lights, "light");
            static const char *const kinds[5] = {"RectLight", "SphereLight", "DiskLight", "DistantLight", "DomeLight"};
            const char *kind = kinds[pick(5)];
            const SdfPath path("/World/l" + std::to_string(++m_names));
            UsdPrim prim = stage->DefinePrim(path, TfToken(kind));
            light(prim);
            UsdGeomXformCommonAPI(prim).SetTranslate(
                GfVec3d(uniform(-1.5f, 1.5f), uniform(-1.5f, 1.5f), uniform(1.5f, 3.0f)));
            m_lights.push_back(path);
            return std::string("add ") + kind + " " + path.GetName();
        }
        case 14: {
            if (m_lights.empty())
                return "none";
            const SdfPath path = any(m_lights);
            light(stage->GetPrimAtPath(path));
            if (chance(0.5f))
                UsdGeomXformCommonAPI(stage->GetPrimAtPath(path))
                    .SetTranslate(GfVec3d(uniform(-1.5f, 1.5f), uniform(-1.5f, 1.5f), uniform(1.5f, 3.0f)));
            return "change " + path.GetName();
        }
        case 16: { // a point instancer, or new instances for one
            const bool fresh = m_instancers.size() < 2 && (m_instancers.empty() || chance(0.4f));
            const SdfPath path = fresh ? SdfPath("/World/pi" + std::to_string(++m_names)) : any(m_instancers);
            UsdPrim prim = stage->DefinePrim(path, TfToken("PointInstancer"));
            if (fresh) {
                UsdGeomMesh proto = UsdGeomMesh::Define(stage, path.AppendChild(TfToken("proto")));
                proto.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
                shape(proto);
                prim.CreateRelationship(TfToken("prototypes")).SetTargets({proto.GetPath()});
                m_instancers.push_back(path);
            }
            const int count = pick(5); // also none
            VtVec3fArray positions, scales;
            VtIntArray indices;
            for (int i = 0; i < count; ++i) {
                positions.push_back({uniform(-1.2f, 1.2f), uniform(-1.2f, 1.2f), uniform(-0.3f, 0.8f)});
                scales.push_back(GfVec3f(uniform(0.3f, 1.0f)));
                indices.push_back(0);
            }
            prim.CreateAttribute(TfToken("positions"), SdfValueTypeNames->Point3fArray).Set(positions);
            prim.CreateAttribute(TfToken("scales"), SdfValueTypeNames->Float3Array).Set(scales);
            prim.CreateAttribute(TfToken("protoIndices"), SdfValueTypeNames->IntArray).Set(indices);
            return (fresh ? "add " : "re-instance ") + path.GetName() + " x" + std::to_string(count);
        }
        case 17: { // an instancer's prototype, its place or its visibility
            if (m_instancers.empty())
                return "none";
            const SdfPath path = any(m_instancers);
            const SdfPath protoPath = path.AppendChild(TfToken("proto"));
            switch (pick(4)) {
            case 0: {
                UsdGeomMesh proto(stage->GetPrimAtPath(protoPath));
                shape(proto);
                return "reshape prototype of " + path.GetName();
            }
            case 1:
                place(path);
                return "move " + path.GetName();
            case 2: {
                if (m_materials.empty())
                    return "none";
                const SdfPath material = any(m_materials);
                bind(protoPath, material);
                return "bind " + material.GetName() + " to prototype of " + path.GetName();
            }
            default: {
                UsdGeomImageable imageable(stage->GetPrimAtPath(path));
                TfToken now;
                imageable.GetVisibilityAttr().Get(&now);
                if (now == UsdGeomTokens->invisible)
                    imageable.MakeVisible();
                else
                    imageable.MakeInvisible();
                return (now == UsdGeomTokens->invisible ? "show " : "hide ") + path.GetName();
            }
            }
        }
        case 18:
            return remove(m_instancers, "instancer");
        case 19: { // points or a curve, or new ones in place of one
            const bool fresh = m_strands.size() < 3 && (m_strands.empty() || chance(0.5f));
            const SdfPath path = fresh ? SdfPath("/World/s" + std::to_string(++m_names)) : any(m_strands);
            const bool curve = fresh ? chance(0.5f) : stage->GetPrimAtPath(path).GetTypeName() == TfToken("BasisCurves");
            UsdPrim prim = stage->DefinePrim(path, TfToken(curve ? "BasisCurves" : "Points"));
            const int count = 2 + pick(4);
            VtVec3fArray points;
            VtFloatArray widths;
            for (int i = 0; i < count; ++i) {
                points.push_back({uniform(-1.2f, 1.2f), uniform(-1.2f, 1.2f), uniform(-0.2f, 0.8f)});
                widths.push_back(uniform(0.05f, 0.25f));
            }
            prim.CreateAttribute(TfToken("points"), SdfValueTypeNames->Point3fArray).Set(points);
            prim.CreateAttribute(TfToken("widths"), SdfValueTypeNames->FloatArray).Set(widths);
            if (curve) {
                prim.CreateAttribute(TfToken("type"), SdfValueTypeNames->Token, SdfVariabilityUniform).Set(TfToken("linear"));
                prim.CreateAttribute(TfToken("curveVertexCounts"), SdfValueTypeNames->IntArray).Set(VtIntArray{count});
            }
            if (fresh)
                m_strands.push_back(path);
            if (chance(0.5f))
                colour(path);
            return (fresh ? "add " : "change ") + std::string(curve ? "curve " : "points ") + path.GetName();
        }
        case 20: { // a face subset with its own material: made, changed or removed
            if (m_geometry.empty() || m_materials.empty())
                return "none";
            const SdfPath mesh = any(m_geometry);
            // (No prim can be defined below a deactivated one.)
            if (!UsdGeomMesh(stage->GetPrimAtPath(mesh)) || !stage->GetPrimAtPath(mesh).IsActive())
                return "none";
            const SdfPath path = mesh.AppendChild(TfToken("part"));
            if (stage->GetPrimAtPath(path) && chance(0.3f)) {
                stage->RemovePrim(path);
                return "remove subset of " + mesh.GetName();
            }
            UsdPrim sub = stage->DefinePrim(path, TfToken("GeomSubset"));
            sub.CreateAttribute(TfToken("elementType"), SdfValueTypeNames->Token, SdfVariabilityUniform).Set(TfToken("face"));
            sub.CreateAttribute(TfToken("familyName"), SdfValueTypeNames->Token, SdfVariabilityUniform)
                .Set(TfToken("materialBind"));
            VtIntArray faces;
            for (int f = 0; f < 6; ++f) // a box has six faces; on a quad the indices past its one face are wrong
                if (chance(0.4f))
                    faces.push_back(f);
            sub.CreateAttribute(TfToken("indices"), SdfValueTypeNames->IntArray).Set(faces);
            const SdfPath material = any(m_materials);
            bind(path, material);
            return "subset of " + mesh.GetName() + " (" + std::to_string(faces.size()) + " faces, " + material.GetName() + ")";
        }
        case 21:
            return remove(m_strands, "strands");
        default:
            return remove(m_lights, "light");
        }
    }

private:
    int pick(int n) { return int(std::uniform_int_distribution<int>(0, n - 1)(m_rng)); }
    float uniform(float a, float b) { return std::uniform_real_distribution<float>(a, b)(m_rng); }
    bool chance(float p) { return uniform(0.0f, 1.0f) < p; }
    SdfPath any(std::vector<SdfPath> const &list) { return list[size_t(pick(int(list.size())))]; }

    std::string remove(std::vector<SdfPath> &list, const char *what) {
        if (list.empty())
            return "none";
        const size_t i = size_t(pick(int(list.size())));
        const SdfPath path = list[i];
        m_rig.stage->RemovePrim(path);
        list.erase(list.begin() + ptrdiff_t(i));
        return std::string("remove ") + what + " " + path.GetName();
    }
    // A quad or a box of a random size.
    void shape(UsdGeomMesh &mesh) {
        const float x = uniform(0.15f, 0.5f), y = uniform(0.15f, 0.5f), z = uniform(0.05f, 0.4f);
        if (chance(0.4f)) {
            mesh.CreatePointsAttr().Set(VtVec3fArray{{-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0}});
            mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{4});
            mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{0, 1, 2, 3});
        } else {
            mesh.CreatePointsAttr().Set(VtVec3fArray{{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z},
                                                     {-x, -y, z},  {x, -y, z},  {x, y, z},  {-x, y, z}});
            mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{4, 4, 4, 4, 4, 4});
            mesh.CreateFaceVertexIndicesAttr().Set(
                VtIntArray{0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5});
        }
    }
    void place(SdfPath const &path) {
        UsdGeomXformCommonAPI api(m_rig.stage->GetPrimAtPath(path));
        api.SetTranslate(GfVec3d(uniform(-1.1f, 1.1f), uniform(-1.1f, 1.1f), uniform(-0.3f, 0.9f)));
        api.SetRotate(GfVec3f(uniform(0.0f, 90.0f), uniform(0.0f, 90.0f), uniform(0.0f, 90.0f)));
    }
    void colour(SdfPath const &path) {
        UsdGeomGprim(m_rig.stage->GetPrimAtPath(path))
            .CreateDisplayColorAttr()
            .Set(VtVec3fArray{{uniform(0.1f, 0.9f), uniform(0.1f, 0.9f), uniform(0.1f, 0.9f)}});
    }
    void bind(SdfPath const &prim, SdfPath const &material) {
        UsdShadeMaterialBindingAPI::Apply(m_rig.stage->GetPrimAtPath(prim))
            .Bind(UsdShadeMaterial(m_rig.stage->GetPrimAtPath(material)));
    }
    // A UsdPreviewSurface material of a random kind at `path` (made or changed).
    std::string surface(SdfPath const &path) {
        UsdStageRefPtr stage = m_rig.stage;
        UsdShadeMaterial material = UsdShadeMaterial::Define(stage, path);
        UsdShadeShader shader = UsdShadeShader::Define(stage, path.AppendChild(TfToken("s")));
        shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
        material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));
        const auto color = [&](const char *name, GfVec3f v) {
            shader.CreateInput(TfToken(name), SdfValueTypeNames->Color3f).Set(v);
        };
        const auto number = [&](const char *name, float v) {
            shader.CreateInput(TfToken(name), SdfValueTypeNames->Float).Set(v);
        };
        static const char *const kinds[5] = {"diffuse", "metal", "cut out", "emitter", "glass"};
        const int kind = pick(5);
        color("diffuseColor", GfVec3f(uniform(0.1f, 0.9f), uniform(0.1f, 0.9f), uniform(0.1f, 0.9f)));
        number("roughness", uniform(0.05f, 0.8f));
        number("metallic", kind == 1 ? 1.0f : 0.0f);
        number("opacity", kind == 2 ? uniform(0.2f, 0.8f) : kind == 4 ? 0.1f : 1.0f);
        color("emissiveColor", kind == 3 ? GfVec3f(uniform(0.5f, 2.0f), uniform(0.3f, 1.0f), uniform(0.3f, 1.0f))
                                         : GfVec3f(0.0f));
        return kinds[kind];
    }
    void light(UsdPrim const &prim) {
        const bool dome = prim.GetTypeName() == TfToken("DomeLight");
        const auto number = [&](const char *name, float v) {
            prim.CreateAttribute(TfToken(name), SdfValueTypeNames->Float).Set(v);
        };
        number("inputs:intensity", dome ? uniform(0.2f, 0.8f) : uniform(1.0f, 6.0f));
        prim.CreateAttribute(TfToken("inputs:color"), SdfValueTypeNames->Color3f)
            .Set(GfVec3f(uniform(0.5f, 1.0f), uniform(0.5f, 1.0f), uniform(0.5f, 1.0f)));
        number("inputs:width", uniform(0.3f, 1.5f));
        number("inputs:height", uniform(0.3f, 1.5f));
        number("inputs:radius", uniform(0.1f, 0.5f));
    }

    std::mt19937 m_rng;
    Rig m_rig;
    int m_names = 0;
    std::vector<SdfPath> m_geometry, m_materials, m_lights, m_instancers, m_strands;
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

// What the edits of a run did to the scene: a rig that synced nothing would
// render the same image at every step and compare equal trivially.
struct Reach {
    int steps = 0, changed = 0; // steps, and those whose image differs from the step before
    uint32_t meshes = 0, lights = 0, materials = 0; // the most at any step
    // Steps with instances of a prototype, with curves, with points, with a mesh of several material slots.
    int instanced = 0, curves = 0, points = 0, slotted = 0;
};

// One seed: after each step of one to three edits the edited stage's scene
// against that of a new render index over the same stage. False at the first
// difference, with the story and the stage in `log`.
bool runSeed(uint32_t seed, int steps, std::string &log, Reach &reach) {
    Host host(seed);
    std::mt19937 counts(seed ^ 0x5bd1e995u);
    std::ostringstream story;
    std::vector<nova::Spectrum> previous;
    for (int step = 0; step < steps; ++step) {
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
        const size_t n = differing(live.film(), fresh.film(), largest);
        if (n != 0) {
            story << "  -> " << n << " of " << live.film().colorSum.size() << " pixels differ (largest " << largest
                  << "); meshes " << host.rig().scene().meshCount() << " / " << loaded.scene().meshCount()
                  << ", lights " << host.rig().scene().lights().size() << " / " << loaded.scene().lights().size()
                  << "\n";
            log = "seed " + std::to_string(seed) + ":\n" + story.str() + text;
            return false;
        }
        const nova::Scene &scene = host.rig().scene();
        uint32_t alive = 0;
        bool instanced = false, curves = false, points = false, slotted = false;
        for (uint32_t id = 0; id < scene.meshCount(); ++id) {
            const nova::MeshRecord &rec = scene.meshRecord(id);
            if (!rec.alive)
                continue;
            ++alive;
            instanced = instanced || (rec.instanced && rec.instances.size() >= 2);
            curves = curves || rec.desc.shape == nova::MeshShape::Curves;
            points = points || rec.desc.shape == nova::MeshShape::Points;
            slotted = slotted || rec.materialSlots.size() >= 2;
        }
        reach.instanced += instanced;
        reach.curves += curves;
        reach.points += points;
        reach.slotted += slotted;
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
    return true;
}

} // namespace

TEST_CASE("Edited stages render as loaded ones", "[hydra][ipr][fuzz]") {
    Reach reach;
    for (uint32_t seed = 1; seed <= 4; ++seed) {
        std::string log;
        const bool same = runSeed(seed, 8, log, reach);
        INFO(log);
        CHECK(same);
    }
    // The edits reached the scene: geometry, lights and materials arrived, and most steps changed the image.
    INFO(reach.changed << " of " << reach.steps << " steps changed the image; up to " << reach.meshes << " meshes, "
                       << reach.lights << " lights, " << reach.materials << " materials");
    CHECK(reach.changed * 2 > reach.steps);
    CHECK(reach.meshes >= 4);
    CHECK(reach.lights >= 2);
    CHECK(reach.materials >= 1);
}

// The hunt: NOVA_FUZZ_SEEDS seeds from NOVA_FUZZ_FIRST, NOVA_FUZZ_STEPS steps
// each. Every failing seed is reported, the first with its stage.
TEST_CASE("Edited stages render as loaded ones over many seeds", "[.][fuzz]") {
    const int seeds = environmentInt("NOVA_FUZZ_SEEDS", 50), steps = environmentInt("NOVA_FUZZ_STEPS", 15);
    const int first = environmentInt("NOVA_FUZZ_FIRST", 1000);
    int failed = 0;
    Reach reach;
    for (int i = 0; i < seeds; ++i) {
        std::string log;
        if (!runSeed(uint32_t(first + i), steps, log, reach)) {
            const size_t stage = log.find("#usda");
            WARN((++failed <= 1 ? log : log.substr(0, stage)));
        }
    }
    WARN(reach.changed << " of " << reach.steps << " steps changed the image; up to " << reach.meshes << " meshes, "
                       << reach.lights << " lights, " << reach.materials << " materials; steps with instances "
                       << reach.instanced << ", curves " << reach.curves << ", points " << reach.points
                       << ", several material slots " << reach.slotted);
    INFO(failed << " of " << seeds << " seeds differ");
    CHECK(failed == 0);
}
