// VEX materials (docs/design/M5.md):
//   * M5.0: Nova reads the "vex" render context after "mtlx", identifies VEX
//     shaders by their sourceAsset or sourceCode (Hydra leaves the node type
//     id empty and keeps both in the node type info, namespaced when authored
//     with a source type), and falls back to the next render context for VEX
//     shaders it does not run;
//   * M5.1: the Principled Shader renders natively, rewritten into a
//     MaterialX Standard Surface network (hydra/material/principled.h).

#include "hydra/material/principled.h"
#include "hydra/material/registry.h"
#include "hydra/material/vexShaders.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include "core/scene/scene.h"

#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;
namespace sf = nova::surface_field;

TEST_CASE("VEX shaders are identified by their source", "[hydra][vex]") {
    auto info = [](char const *source, char const *key, VtValue const &v) {
        VtDictionary d;
        d["implementationSource"] = VtValue(TfToken(source));
        d[key] = v;
        return d;
    };
    const VtValue principled(SdfAssetPath("opdef:/Vop/principledshader::2.0?SurfaceVexCode"));
    CHECK(HdNovaVexShaderOf(info("sourceAsset", "sourceAsset", principled)).kind == HdNovaVexKind::PrincipledSurface);
    CHECK(HdNovaVexShaderOf(info("sourceAsset", "VEX:sourceAsset", principled)).kind ==
          HdNovaVexKind::PrincipledSurface);
    CHECK(HdNovaVexShaderOf(info("sourceAsset", "vex:sourceAsset",
                                 VtValue(SdfAssetPath("opdef:/Vop/principledshader::2.0?DisplacementVexCode"))))
              .kind == HdNovaVexKind::PrincipledDisplacement);
    const HdNovaVexShader other =
        HdNovaVexShaderOf(info("sourceAsset", "sourceAsset", VtValue(SdfAssetPath("opdef:/Vop/classicshader::3.0?SurfaceVexCode"))));
    CHECK(other.kind == HdNovaVexKind::Asset);
    CHECK(other.asset == "opdef:/Vop/classicshader::3.0?SurfaceVexCode");
    const HdNovaVexShader code = HdNovaVexShaderOf(info("sourceCode", "vex:sourceCode", VtValue(std::string("surface s() {}"))));
    CHECK(code.kind == HdNovaVexKind::Code);
    CHECK(code.code == "surface s() {}");
    CHECK(HdNovaVexShaderOf(info("sourceCode", "sourceCode", VtValue(std::string()))).kind == HdNovaVexKind::NotVex);
    CHECK(HdNovaVexShaderOf(info("id", "sourceAsset", principled)).kind == HdNovaVexKind::NotVex);
    CHECK(HdNovaVexShaderOf(VtDictionary()).kind == HdNovaVexKind::NotVex);
}

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

// As Houdini writes them: a Principled Shader with its preview, a VEX
// Material Builder (source code, no preview), and a material with both a
// MaterialX and a VEX network.
const char *kScene = R"(#usda 1.0
def Mesh "quad" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    rel material:binding = </mat/principled>
}
def Scope "mat"
{
    def Material "principled"
    {
        token outputs:surface.connect = </mat/principled/preview.outputs:surface>
        token outputs:vex:surface.connect = </mat/principled/principled_surface.outputs:surface>
        def Shader "principled_surface"
        {
            uniform token info:implementationSource = "sourceAsset"
            uniform asset info:sourceAsset = @opdef:/Vop/principledshader::2.0?SurfaceVexCode@
            vector3f inputs:basecolor = (0.8, 0.1, 0.1)
            token outputs:surface
        }
        def Shader "preview"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.8, 0.1, 0.1)
            token outputs:surface
        }
    }
    def Material "builder"
    {
        token outputs:vex:surface.connect = </mat/builder/builder_surface.outputs:surface>
        def Shader "builder_surface"
        {
            uniform token info:implementationSource = "sourceCode"
            string info:sourceCode = "surface\nbuilder(vector tint = {0.2, 0.6, 0.2})\n{\n    F = 2.0 * diffuse(N) * tint;\n}\n"
            vector3f inputs:tint = (0.2, 0.2, 0.8)
            token outputs:surface
        }
    }
    def Material "reader"
    {
        token outputs:vex:surface.connect = </mat/reader/v.outputs:surface>
        def Shader "v"
        {
            uniform token info:implementationSource = "sourceCode"
            string info:sourceCode = "surface\nreader(vector tint = {1, 1, 1})\n{\n    F = diffuse(N) * tint;\n}\n"
            vector3f inputs:tint.connect = </mat/reader/pv.outputs:result>
            token outputs:surface
        }
        def Shader "pv"
        {
            uniform token info:id = "UsdPrimvarReader_float3"
            string inputs:varname = "mycolor"
            float3 outputs:result
        }
    }
    def Material "broken"
    {
        token outputs:vex:surface.connect = </mat/broken/s.outputs:surface>
        def Shader "s"
        {
            uniform token info:implementationSource = "sourceCode"
            string info:sourceCode = "surface\nbroken()\n{\n    F = no_such_function(N);\n}\n"
            token outputs:surface
        }
    }
    def Material "both"
    {
        token outputs:mtlx:surface.connect = </mat/both/s.outputs:out>
        token outputs:vex:surface.connect = </mat/both/v.outputs:surface>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            token outputs:out
        }
        def Shader "v"
        {
            uniform token info:implementationSource = "sourceCode"
            string info:sourceCode = "surface\nv()\n{\n}\n"
            token outputs:surface
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

    Driver() {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(kScene);
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
        param().GetScene().commit();
    }
    ~Driver() {
        tasks.clear();
        pass.reset();
        index.reset();
        delegate.reset();
    }
    HdNovaRenderParam &param() { return *static_cast<HdNovaRenderParam *>(delegate->GetRenderParam()); }
    nova::MaterialRecord const &material(char const *path) {
        return param().GetScene().material(param().GetMaterials().Acquire(SdfPath(path), param().GetScene()));
    }
};

} // namespace

TEST_CASE("VEX materials render natively, through the bridge, or fall back", "[hydra][vex]") {
    Driver d;
    HdNovaMaterialRegistry &registry = d.param().GetMaterials();
    const nova::shading::ProgramPtr fallback = nova::defaultMaterial().program;

    // The Principled Shader renders natively (M5.1), not its preview.
    CHECK_FALSE(registry.Warned(SdfPath("/mat/principled"), "does not run in Nova"));
    nova::MaterialRecord const &principled = d.material("/mat/principled");
    REQUIRE(principled.desc.program);
    CHECK(principled.desc.inputs.model == nova::SurfaceModel::StandardSurface);
    CHECK_THAT(principled.resolved.f(sf::specularIor), WithinAbs(1.5, 1e-5));
    // A builder runs through the CVEX bridge (M5.3), its authored tint as the
    // per-hit parameter's value.
    CHECK_FALSE(registry.Warned(SdfPath("/mat/builder"), "vex"));
    nova::MaterialRecord const &builder = d.material("/mat/builder");
    CHECK(builder.desc.inputs.model == nova::SurfaceModel::External);
    REQUIRE(builder.desc.external);
    REQUIRE(builder.desc.external->shader->params().size() == 1);
    CHECK(builder.desc.external->slots[0] == nova::surface_field::kCount);
    REQUIRE(builder.desc.params.size() == 3);
    CHECK(builder.desc.params[2] == 0.8f);
    REQUIRE(builder.desc.program);
    CHECK(std::find(builder.desc.program->primvars.begin(), builder.desc.program->primvars.end(), "tint") !=
          builder.desc.program->primvars.end()); // the primvar of the parameter's name (D10)
    // A UsdPrimvarReader connected to a VEX input binds its varname (D10).
    nova::MaterialRecord const &reader = d.material("/mat/reader");
    REQUIRE(reader.desc.external);
    REQUIRE(reader.desc.program);
    CHECK(std::find(reader.desc.program->primvars.begin(), reader.desc.program->primvars.end(), "mycolor") !=
          reader.desc.program->primvars.end());
    // Code that does not compile warns; with no preview, the default material.
    CHECK(registry.Warned(SdfPath("/mat/broken"), "VEX compile failed"));
    CHECK(d.material("/mat/broken").desc.program == fallback);
    // MaterialX comes before VEX: no VEX warning.
    CHECK_FALSE(registry.Warned(SdfPath("/mat/both"), "vex"));
    CHECK(d.material("/mat/both").desc.program != fallback);
}

TEST_CASE("Principled Shader value edits keep the program", "[hydra][vex][ipr]") {
    Driver d;
    const nova::shading::ProgramPtr program = d.material("/mat/principled").desc.program;
    REQUIRE(program);
    UsdPrim shader = d.stage->GetPrimAtPath(SdfPath("/mat/principled/principled_surface"));
    auto set = [&](char const *name, VtValue const &v) {
        shader.CreateAttribute(TfToken(std::string("inputs:") + name),
                               v.IsHolding<float>() ? SdfValueTypeNames->Float : SdfValueTypeNames->Vector3f)
            .Set(v);
        d.sceneIndices.stageSceneIndex->ApplyPendingUpdates();
        d.engine.Execute(d.index.get(), &d.tasks);
        d.param().GetScene().commit();
    };
    // (Anisotropy is left out: from zero to non-zero it adds the tangent
    // frame, a recompile as for any Standard Surface.)
    for (char const *name : {"rough", "metallic", "sheen", "coat", "reflect", "emitint"}) {
        set(name, VtValue(0.75f));
        CHECK(d.material("/mat/principled").desc.program == program);
    }
    set("basecolor", VtValue(GfVec3f(0.1f, 0.2f, 0.3f)));
    CHECK(d.material("/mat/principled").desc.program == program);
    CHECK_THAT(d.material("/mat/principled").resolved.f(sf::metalness), WithinAbs(0.75, 1e-6));
    CHECK_THAT(d.material("/mat/principled").resolved.f(sf::emissionWeight), WithinAbs(0.75, 1e-6));
}

namespace {

// The Principled Shader's parameters on one node, as Hydra delivers them.
HdNovaNetworkSource principledNetwork(std::map<TfToken, VtValue> params) {
    HdNovaNetworkSource src;
    src.context = TfToken("vex");
    const SdfPath shader("principled_surface");
    src.net.nodes[shader].parameters = std::move(params);
    src.net.terminals[HdMaterialTerminalTokens->surface] = {shader, TfToken("surface")};
    VtDictionary info;
    info["implementationSource"] = VtValue(TfToken("sourceAsset"));
    info["sourceAsset"] = VtValue(SdfAssetPath("opdef:/Vop/principledshader::2.0?SurfaceVexCode"));
    src.nodeTypeInfo[shader] = info;
    return src;
}

HdMaterialNode2 const &surfaceOf(HdNovaNetworkSource const &net) {
    return net.net.nodes.at(net.net.terminals.at(HdMaterialTerminalTokens->surface).upstreamNode);
}

float paramF(HdMaterialNode2 const &n, char const *name) { return n.parameters.at(TfToken(name)).Get<float>(); }

} // namespace

TEST_CASE("The Principled Shader maps onto Standard Surface", "[hydra][vex]") {
    std::vector<std::string> warnings;
    HdNovaNetworkSource out;
    SECTION("values") {
        REQUIRE(HdNovaRewritePrincipled(principledNetwork({{TfToken("reflect"), VtValue(0.5f)},
                                                           {TfToken("rough"), VtValue(0.4f)},
                                                           {TfToken("aniso"), VtValue(-0.5f)},
                                                           {TfToken("anisodir"), VtValue(0.2f)},
                                                           {TfToken("emitint"), VtValue(3.0f)}}),
                                        &out, &warnings));
        HdMaterialNode2 const &ss = surfaceOf(out);
        CHECK(ss.nodeTypeId == TfToken("ND_standard_surface_surfaceshader"));
        // Half the facing reflectance of IOR 1.5 (0.04 -> 0.02); grazing stays 1.
        CHECK_THAT(paramF(ss, "specular_IOR"), WithinAbs(1.32938, 1e-4));
        CHECK(paramF(ss, "specular") == 1.0f);
        CHECK(paramF(ss, "specular_roughness") == 0.4f);
        CHECK(paramF(ss, "diffuse_roughness") == 0.4f);
        CHECK_THAT(paramF(ss, "specular_anisotropy"), WithinAbs(0.45, 1e-6)); // 0.9 |aniso|
        CHECK_THAT(paramF(ss, "specular_rotation"), WithinAbs(0.15, 1e-6)); // -0.2 / 2 + a quarter turn
        CHECK(paramF(ss, "emission") == 3.0f);
        // The base colour: 0.2 (the default) x the Cd|displayColor primvar.
        CHECK(ss.inputConnections.count(TfToken("base_color")) == 1);
        bool primvar = false;
        for (auto const &[path, node] : out.net.nodes)
            if (node.nodeTypeId == TfToken("ND_geompropvalue_color3") &&
                node.parameters.at(TfToken("geomprop")) == VtValue(std::string("Cd|displayColor")))
                primvar = true;
        CHECK(primvar);
        CHECK(warnings.empty());
    }
    SECTION("refraction keeps the IOR and weights the reflectivity") {
        REQUIRE(HdNovaRewritePrincipled(principledNetwork({{TfToken("reflect"), VtValue(0.5f)},
                                                           {TfToken("transparency"), VtValue(1.0f)}}),
                                        &out, &warnings));
        HdMaterialNode2 const &ss = surfaceOf(out);
        CHECK(paramF(ss, "specular_IOR") == 1.5f);
        CHECK(paramF(ss, "specular") == 0.5f);
        CHECK(paramF(ss, "transmission") == 1.0f);
    }
    SECTION("textures") {
        REQUIRE(HdNovaRewritePrincipled(
            principledNetwork({{TfToken("rough_useTexture"), VtValue(1)},
                               {TfToken("rough_texture"), VtValue(SdfAssetPath("r.png", "C:/r.png"))},
                               {TfToken("rough_textureWrap"), VtValue(std::string("decal"))},
                               {TfToken("rough_monoChannel"), VtValue(2)},
                               {TfToken("uvscale"), VtValue(GfVec2f(2.0f, 2.0f))}}),
            &out, &warnings));
        HdMaterialNode2 const *image = nullptr;
        SdfPath imagePath;
        bool extract = false, divide = false;
        for (auto const &[path, node] : out.net.nodes) {
            if (node.nodeTypeId == TfToken("ND_image_color4")) {
                image = &node;
                imagePath = path;
            }
            extract |= node.nodeTypeId == TfToken("ND_extract_color4") &&
                       node.parameters.at(TfToken("index")) == VtValue(1); // channel 2 = green
            divide |= node.nodeTypeId == TfToken("ND_divide_vector2");    // the inverse uv scale
        }
        REQUIRE(image);
        CHECK(image->parameters.at(TfToken("file")).Get<SdfAssetPath>().GetResolvedPath() == "C:/r.png");
        CHECK(image->parameters.at(TfToken("uaddressmode")) == VtValue(std::string("black"))); // decal: a black border
        CHECK(out.meta.at(imagePath).at(TfToken("file")).colorSpace == TfToken("raw")); // "linear" default
        CHECK(extract);
        CHECK(divide);
        CHECK(surfaceOf(out).inputConnections.count(TfToken("specular_roughness")) == 1);
    }
    SECTION("normal and bump maps, opacity") {
        REQUIRE(HdNovaRewritePrincipled(
            principledNetwork({{TfToken("baseBumpAndNormal_enable"), VtValue(1)},
                               {TfToken("baseNormal_texture"), VtValue(SdfAssetPath("n.png", "C:/n.png"))},
                               {TfToken("baseNormal_flipY"), VtValue(1)},
                               {TfToken("opac"), VtValue(0.5f)}}),
            &out, &warnings));
        HdMaterialNode2 const &ss = surfaceOf(out);
        REQUIRE(ss.inputConnections.count(TfToken("normal")) == 1);
        const SdfPath normal = ss.inputConnections.at(TfToken("normal")).front().upstreamNode;
        CHECK(out.net.nodes.at(normal).nodeTypeId == TfToken("ND_normalmap_float"));
        CHECK(ss.inputConnections.count(TfToken("coat_normal")) == 1); // the coat shares it
        // Opacity: 0.5 x the Alpha|displayOpacity primvar.
        REQUIRE(ss.inputConnections.count(TfToken("opacity")) == 1);
        out = HdNovaNetworkSource{};
        REQUIRE(HdNovaRewritePrincipled(
            principledNetwork({{TfToken("baseBumpAndNormal_enable"), VtValue(1)},
                               {TfToken("baseBumpAndNormal_type"), VtValue(std::string("bump"))},
                               {TfToken("baseBump_bumpTexture"), VtValue(SdfAssetPath("h.png", "C:/h.png"))}}),
            &out, &warnings));
        const SdfPath bump = surfaceOf(out).inputConnections.at(TfToken("normal")).front().upstreamNode;
        CHECK(out.net.nodes.at(bump).nodeTypeId == TfToken("ND_bump_vector3"));
        CHECK(paramF(out.net.nodes.at(bump), "scale") == 0.05f);
    }
    SECTION("not the Principled Shader") {
        HdNovaNetworkSource src = principledNetwork({});
        src.nodeTypeInfo.clear();
        CHECK_FALSE(HdNovaRewritePrincipled(src, &out, &warnings));
    }
}
