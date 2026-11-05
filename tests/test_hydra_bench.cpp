// M3 performance gates (docs/design/M3.md section 8), measured, not
// asserted: run with the tag [.bench] (hidden from the default run) and copy
// the numbers into docs/benchmarks.md.
//   VM: ns per instruction and lane at N = 32;
//   closure build: ns per hit for a default Standard Surface;
//   translation + compile: ms per material (Standard Surface + 3 textures);
//   IPR value edit: ms for a parameter edit (record rebuild and commit).

#include "test_helpers.h"

#include "hydra/material/translator.h"

#include "core/bsdf/surface_models.h"
#include "core/bsdf/energy.h"
#include "core/shading/compiler.h"
#include "core/shading/vm.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

double seconds(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
}

HdMaterialNode2 node(char const *id) {
    HdMaterialNode2 n;
    n.nodeTypeId = TfToken(id);
    return n;
}

} // namespace

TEST_CASE("M3 performance numbers", "[.bench]") {
    namespace sh = nova::shading;
    // ---- VM: a 4-op chain repeated, all varying
    {
        sh::IrGraph g;
        sh::IrBuilder b(g);
        sh::Ref v = b.global(sh::Global::P);
        for (int i = 0; i < 60; ++i)
            v = b.add(b.mul(v, b.constant(1.0001f)), b.unary(sh::Op::Abs, b.swizzle(v, 2, 0, 1)));
        b.output(nova::surface_field::baseColor, 3, v);
        const sh::CompileResult r = sh::compile(g);
        REQUIRE(r.program);
        const sh::Program &p = *r.program;
        std::vector<float> U;
        p.initUniforms(nullptr, U);
        std::vector<float> R(size_t(p.rowCount) * 32, 0.5f);
        const int runs = 20000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < runs; ++i)
            sh::execute<32>(p.code.data(), p.code.size(), R.data(), U.data());
        const double s = seconds(t0);
        const double perInstrLane = s * 1e9 / (double(runs) * double(p.code.size()) * 32.0 * 3.0); // width 3
        std::printf("VM: %zu instructions (width 3), %.3f ns per instruction component and lane at N = 32\n",
                    p.code.size(), perInstrLane);
    }
    // ---- closure build: default Standard Surface, varying view
    {
        const nova::SurfaceInputs in = nova::standardSurfaceDefaults(false);
        const nova::ClosureContext ctx{&nova::bsdfTables(), nova::kXyzToRec709Grey};
        const nova::ShadingGeometry geom{{0, 0, 1}, {0, 0, 1}, {1, 0, 0}, false};
        nova::ClosureList cl;
        std::vector<nova::Vec3f> views(1000);
        for (size_t i = 0; i < views.size(); ++i)
            views[i] = {std::sin(float(i) * 0.0015f), 0.0f, std::cos(float(i) * 0.0015f)};
        const int runs = 2000000;
        float sink = 0.0f;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < runs; ++i) {
            nova::buildClosure(in, geom, views[size_t(i) % views.size()], ctx, cl);
            sink += cl.albedo.x;
        }
        const double s = seconds(t0);
        std::printf("closure build (default Standard Surface): %.1f ns per hit (%g)\n", s * 1e9 / runs, double(sink) * 0.0);
    }
    // ---- closure build: the same Standard Surface as a generic graph (M4.5)
    {
        HdNovaNetworkSource src;
        src.context = TfToken("mtlx");
        src.net.nodes[SdfPath("/surface")] = node("ND_standard_surface_surfaceshader");
        src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("/surface"), TfToken()};
        _putenv_s("NOVA_MTLX_EXPAND", "standard_surface");
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
        _putenv_s("NOVA_MTLX_EXPAND", "");
        REQUIRE(tr.ok);
        nova::MaterialDesc desc;
        desc.inputs = tr.defaults;
        desc.program = sh::compile(tr.graph).program;
        desc.params = tr.params;
        REQUIRE(desc.program);
        const nova::MaterialRecord rec = nova::makeMaterialRecord(desc);
        const nova::ClosureContext ctx{&nova::bsdfTables(), nova::kXyzToRec709Grey};
        const nova::ShadingGeometry geom{{0, 0, 1}, {0, 0, 1}, {1, 0, 0}, false};
        nova::ClosureList cl;
        std::vector<nova::Vec3f> views(1000);
        for (size_t i = 0; i < views.size(); ++i)
            views[i] = {std::sin(float(i) * 0.0015f), 0.0f, std::cos(float(i) * 0.0015f)};
        const int runs = 1000000;
        float sink = 0.0f;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < runs; ++i) {
            nova::buildClosure(rec.resolved, geom, views[size_t(i) % views.size()], ctx, cl);
            sink += cl.albedo.x;
        }
        const double s = seconds(t0);
        std::printf("closure build (default Standard Surface as a generic graph, %d nodes): %.1f ns per hit (%g)\n",
                    int(rec.resolved.graph ? rec.resolved.graph->nodeCount : 0), s * 1e9 / runs, double(sink) * 0.0);
    }
    // ---- translation + compile: Standard Surface with three image inputs
    {
        HdNovaNetworkSource src;
        src.context = TfToken("mtlx");
        HdMaterialNode2 surface = node("ND_standard_surface_surfaceshader");
        surface.inputConnections[TfToken("base_color")] = {{SdfPath("albedo"), TfToken("out")}};
        surface.inputConnections[TfToken("specular_roughness")] = {{SdfPath("rough"), TfToken("out")}};
        surface.inputConnections[TfToken("normal")] = {{SdfPath("nmap"), TfToken("out")}};
        src.net.nodes[SdfPath("surface")] = surface;
        src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("surface"), TfToken()};
        HdMaterialNode2 albedo = node("ND_image_color3");
        albedo.parameters[TfToken("file")] = VtValue(SdfAssetPath("a.png", "C:/a.png"));
        HdMaterialNode2 rough = node("ND_image_float");
        rough.parameters[TfToken("file")] = VtValue(SdfAssetPath("r.png", "C:/r.png"));
        HdMaterialNode2 nimg = node("ND_image_vector3");
        nimg.parameters[TfToken("file")] = VtValue(SdfAssetPath("n.png", "C:/n.png"));
        HdMaterialNode2 nmap = node("ND_normalmap_float");
        nmap.inputConnections[TfToken("in")] = {{SdfPath("nimg"), TfToken("out")}};
        src.net.nodes[SdfPath("albedo")] = albedo;
        src.net.nodes[SdfPath("rough")] = rough;
        src.net.nodes[SdfPath("nimg")] = nimg;
        src.net.nodes[SdfPath("nmap")] = nmap;
        HdNovaTranslateMaterial(src); // libraries loaded
        const int runs = 200;
        size_t instructions = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < runs; ++i) {
            const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
            const sh::CompileResult r = sh::compile(tr.graph);
            instructions = r.program ? r.program->code.size() : 0;
        }
        const double s = seconds(t0);
        std::printf("translation + compile (Standard Surface + 3 textures, %zu instructions): %.2f ms per material\n",
                    instructions, s * 1e3 / runs);
    }
    // ---- IPR value edit: a parameter changes, the record is rebuilt, commit
    {
        nova::Scene scene;
        const nova::MeshId mesh = scene.createMesh();
        scene.updateMesh(mesh, nova::test::makeSphere(1.0f, 64, 32));
        sh::IrGraph g;
        g.model = nova::SurfaceModel::StandardSurface;
        sh::IrBuilder b(g);
        b.output(nova::surface_field::baseColor, 3, b.newParam(3));
        nova::MaterialDesc desc;
        desc.inputs = nova::standardSurfaceDefaults(false);
        desc.program = sh::compile(g).program;
        desc.params = {0.5f, 0.5f, 0.5f};
        const nova::MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, desc);
        scene.setMeshMaterials(mesh, {m});
        scene.commit();
        const int runs = 2000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < runs; ++i) {
            desc.params[0] = float(i % 100) * 0.01f;
            scene.updateMaterial(m, desc);
            scene.commit();
        }
        const double s = seconds(t0);
        std::printf("IPR value edit (engine side): %.2f us per edit\n", s * 1e6 / runs);
    }
}
