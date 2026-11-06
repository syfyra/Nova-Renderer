// Karma's shading nodes and conventions (docs/design/M4.md, M4.0;
// docs/research/m4/karma-nodes.md): the kma_* node definitions from Karma's
// registry, the ray switch, ray imports (kma_rayimport and MaterialX dot
// nodes noted "ray:<var>"), kma_camerafalloff (built on those), the AOV
// pass-through, dot surface shaders, displacement warnings, and the coverage
// of Houdini's own material libraries.
//
// Translated graphs are evaluated for one hit with chosen globals (ray type,
// hit distance, ...) by the VM directly.

#include "hydra/material/networkSource.h"
#include "hydra/material/nodeDefDatabase.h"
#include "hydra/material/translator.h"

#include "core/bsdf/closure.h"
#include "core/shading/compiler.h"
#include "core/shading/vm.h"

#include <MaterialXCore/Document.h>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/js/json.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace sh = nova::shading;
namespace sf = nova::surface_field;
using sh::Global;

namespace {

HdMaterialNode2 node(char const *id) {
    HdMaterialNode2 n;
    n.nodeTypeId = TfToken(id);
    return n;
}

// A network around a surface_unlit whose emission_color is connected.
struct Network {
    HdNovaNetworkSource src;
    Network() {
        src.context = TfToken("kma");
        src.net.nodes[SdfPath("surface")] = node("ND_surface_unlit");
        src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("surface"), TfToken()};
    }
    HdMaterialNode2 &add(char const *name, HdMaterialNode2 n) { return src.net.nodes[SdfPath(name)] = std::move(n); }
    void connect(char const *to, char const *input, char const *from, char const *output = "out") {
        src.net.nodes[SdfPath(to)].inputConnections[TfToken(input)] = {{SdfPath(from), TfToken(output)}};
    }
    void emission(char const *from, char const *output = "out") { connect("surface", "emission_color", from, output); }
};

using Globals = std::map<Global, std::vector<float>>;
using Primvars = std::map<std::string, std::vector<float>>;

// Surface field `field` (width w) of a translated material for one hit with
// `globals` (the others 0) and `primvars` (the others their fallbacks).
std::vector<float> evaluate(HdNovaTranslatedMaterial const &tr, uint16_t field, int w, Globals const &globals,
                            Primvars const &primvars = {}, nova::TextureContext const *tex = nullptr) {
    REQUIRE(tr.ok);
    const sh::CompileResult r = sh::compile(tr.graph);
    REQUIRE(r.program);
    const sh::Program &p = *r.program;
    std::vector<float> U;
    p.initUniforms(tr.params.data(), U);
    std::vector<float> R(std::max<size_t>(p.rowCount, 1) + 4, 0.0f);
    for (sh::GlobalBinding const &g : p.globals) {
        const auto it = globals.find(g.global);
        if (it != globals.end())
            for (size_t k = 0; k < it->second.size(); ++k)
                R[g.row + k] = it->second[k];
    }
    for (sh::PrimvarBinding const &b : p.primvarBindings) {
        const auto given = primvars.find(p.primvars[b.name]);
        for (int k = 0; k < b.width; ++k)
            R[size_t(b.row) + size_t(k)] = given != primvars.end() && size_t(k) < given->second.size()
                                                ? given->second[size_t(k)]
                                                : U[size_t(b.fallback) + size_t(k)];
    }
    sh::execute<1>(p.code.data(), p.code.size(), R.data(), U.data(), tex);
    std::vector<float> v(static_cast<size_t>(w), 0.0f);
    for (int k = 0; k < w; ++k)
        v[size_t(k)] = tr.defaults.v[field + k];
    for (sh::OutputBinding const &o : p.outputs)
        if (o.field == field)
            for (int k = 0; k < std::min<int>(o.width, w); ++k)
                v[size_t(k)] = o.uniform ? U[size_t(o.index) + size_t(k)] : R[size_t(o.index) + size_t(k)];
    return v;
}

std::vector<float> emission(HdNovaTranslatedMaterial const &tr, Globals const &g) {
    return evaluate(tr, sf::emissionColor, 3, g);
}

Globals camera(float hitDist = 5.0f) {
    return {{Global::RayType, {0.0f}},
            {Global::RayDepth, {0.0f}},
            {Global::HitDist, {hitDist}},
            {Global::I, {0.0f, 0.0f, -1.0f}},
            {Global::N, {0.0f, 0.0f, 1.0f}},
            {Global::Ng, {0.0f, 0.0f, 1.0f}},
            {Global::P, {0.0f, 0.0f, 0.0f}}};
}

int typeWidth(std::string const &type) {
    if (type == "float" || type == "integer" || type == "boolean")
        return 1;
    if (type == "vector2")
        return 2;
    if (type == "vector3" || type == "color3")
        return 3;
    return type == "vector4" || type == "color4" ? 4 : 0;
}

bool hasWarning(HdNovaTranslatedMaterial const &tr, std::string const &part) {
    for (std::string const &w : tr.warnings)
        if (w.find(part) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE("Karma's shading nodes come from its registry", "[hydra][karma]") {
    HdNovaNodeDefDatabase const &db = HdNovaNodeDefDatabase::Get();
    CHECK(db.GetKarmaNodeCount() >= 50);
    const MaterialX::ConstNodeDefPtr nd = db.GetLibrary()->getNodeDef("kma_rayswitch_color");
    REQUIRE(nd);
    CHECK(nd->getNodeString() == "kma_rayswitch_color");
    CHECK(nd->getActiveInputs().size() == 7);
    REQUIRE(nd->getActiveInput("shadow"));
    CHECK(nd->getActiveInput("shadow")->getValueString() == "1, 1, 1");
    CHECK(nd->getActiveOutputs().size() == 2);
    // The one kma node with a real MaterialX definition keeps it.
    REQUIRE(db.GetLibrary()->getNodeDef("ND_kma_camerafalloff"));
}

TEST_CASE("The ray switch selects by Karma's ray classes", "[hydra][karma]") {
    Network net;
    HdMaterialNode2 &rs = net.add("switch", node("kma_rayswitch_color"));
    const char *inputs[] = {"camera", "diffuse", "glossy_reflection", "glossy_transmission", "sss", "volume", "shadow"};
    for (int k = 0; k < 7; ++k)
        rs.parameters[TfToken(inputs[k])] = VtValue(GfVec3f(float(k), float(k) * 0.5f, 1.0f));
    net.emission("switch", "result");
    const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
    for (int k = 0; k < 7; ++k) {
        Globals g = camera();
        g[Global::RayType] = {float(k)};
        const std::vector<float> e = emission(tr, g);
        CHECK(e[0] == float(k));
        CHECK(e[1] == float(k) * 0.5f);
    }
    // The raytype output.
    Network net2;
    net2.add("switch", node("kma_rayswitch_float"));
    net2.emission("switch", "raytype");
    const HdNovaTranslatedMaterial tr2 = HdNovaTranslateMaterial(net2.src);
    Globals g = camera();
    g[Global::RayType] = {float(nova::RayClass::Refract)};
    CHECK(emission(tr2, g)[0] == 3.0f);
}

TEST_CASE("Ray imports and dot notes read the ray state", "[hydra][karma]") {
    auto imported = [](char const *id, char const *nameInput, char const *name, Globals const &g) {
        Network net;
        HdMaterialNode2 &n = net.add("imp", node(id));
        n.parameters[TfToken(nameInput)] = VtValue(std::string(name));
        n.parameters[TfToken("in")] = VtValue(0.25f);
        net.emission("imp");
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
        return std::make_pair(emission(tr, g)[0], tr);
    };
    Globals g = camera(5.0f);
    g[Global::RayDepth] = {2.0f};
    CHECK(imported("kma_rayimport_float", "name", "hitdist", g).first == 5.0f);
    CHECK(imported("kma_rayimport_float", "name", "ray:level", g).first == 2.0f);
    CHECK(imported("ND_dot_float", "note", "ray:hitdist", g).first == 5.0f);
    CHECK(imported("ND_dot_float", "note", "ray:level", g).first == 2.0f);
    // Other notes are plain pass-throughs; unknown variables warn and pass
    // the input through.
    CHECK(imported("ND_dot_float", "note", "shadowcolor", g).first == 0.25f);
    const auto unknown = imported("kma_rayimport_float", "name", "ray:velocity", g);
    CHECK(unknown.first == 0.25f);
    CHECK(hasWarning(unknown.second, "ray:velocity"));
    // isshadowray: 1 on shadow rays only.
    Globals shadow = g;
    shadow[Global::RayType] = {float(nova::RayClass::Shadow)};
    CHECK(imported("ND_dot_float", "note", "ray:isshadowray", shadow).first == 1.0f);
    CHECK(imported("ND_dot_float", "note", "ray:isshadowray", g).first == 0.0f);
}

TEST_CASE("kma_camerafalloff follows the ray direction", "[hydra][karma]") {
    // Head-on: the base color; at grazing incidence: the edge color.
    Network net;
    HdMaterialNode2 &n = net.add("falloff", node("ND_kma_camerafalloff"));
    n.parameters[TfToken("basecolor")] = VtValue(GfVec3f(0.0f, 1.0f, 0.0f));
    n.parameters[TfToken("edgecolor")] = VtValue(GfVec3f(1.0f, 0.0f, 0.0f));
    net.emission("falloff", "color");
    const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
    CHECK(tr.warnings.empty());
    const std::vector<float> headOn = emission(tr, camera());
    CHECK_THAT(headOn[0], WithinAbs(0.0, 1e-5));
    CHECK_THAT(headOn[1], WithinAbs(1.0, 1e-5));
    Globals grazing = camera();
    grazing[Global::I] = {1.0f, 0.0f, 0.0f};
    const std::vector<float> edge = emission(tr, grazing);
    CHECK_THAT(edge[0], WithinAbs(1.0, 1e-5));
    CHECK_THAT(edge[1], WithinAbs(0.0, 1e-5));
}

TEST_CASE("Karma's pass-through conventions", "[hydra][karma]") {
    // kma_aov 1.0: the output named after the AOV passes the input through.
    Network net;
    HdMaterialNode2 &c = net.add("c", node("ND_constant_color3"));
    c.parameters[TfToken("value")] = VtValue(GfVec3f(0.1f, 0.2f, 0.3f));
    net.add("aov", node("kma_aov"));
    net.connect("aov", "input", "c");
    net.emission("aov", "myfalloff1");
    const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
    CHECK(tr.warnings.empty());
    CHECK_THAT(emission(tr, camera())[2], WithinAbs(0.3, 1e-6));

    // A dot surface shader between the terminal and the surface (Karma's
    // fake-caustics and nested-dielectrics node graphs).
    Network dot;
    dot.add("passthrough", node("ND_dot_surfaceshader"));
    dot.connect("passthrough", "in", "surface");
    dot.src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("passthrough"), TfToken()};
    const HdNovaTranslatedMaterial trDot = HdNovaTranslateMaterial(dot.src);
    CHECK(trDot.ok);
    CHECK(trDot.graph.model == nova::SurfaceModel::Unlit);

    // Material inputs in Karma's namespaces (material properties, AOVs) are
    // ignored without warnings.
    Network props;
    props.src.net.nodes[SdfPath("surface")].parameters[TfToken("karma:diffuselimit")] = VtValue(3.0f);
    props.connect("surface", "aov:myaov", "c");
    props.add("c", c);
    CHECK(HdNovaTranslateMaterial(props.src).warnings.empty());
}

TEST_CASE("Displacement compiles only when it displaces", "[hydra][karma]") {
    // M4.6: a displacement program exists only for a displacing terminal;
    // Karma's builder always wires one, 0 by default.
    auto displaces = [](HdMaterialNode2 const &displacement) {
        Network net;
        net.add("disp", displacement);
        net.src.net.terminals[HdMaterialTerminalTokens->displacement] = {SdfPath("disp"), TfToken()};
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
        REQUIRE(tr.ok);
        CHECK(tr.warnings.empty());
        const sh::CompileResult r = sh::compile(tr.graph);
        REQUIRE(r.program);
        return r.program->displacement != nullptr;
    };
    CHECK_FALSE(displaces(node("ND_displacement_float")));
    HdMaterialNode2 zero = node("ND_displacement_float");
    zero.parameters[TfToken("displacement")] = VtValue(0.0f);
    CHECK_FALSE(displaces(zero));
    HdMaterialNode2 scaled = node("ND_displacement_float");
    scaled.parameters[TfToken("displacement")] = VtValue(0.1f);
    scaled.parameters[TfToken("scale")] = VtValue(0.0f);
    CHECK_FALSE(displaces(scaled));
    HdMaterialNode2 real = node("ND_displacement_float");
    real.parameters[TfToken("displacement")] = VtValue(0.1f);
    CHECK(displaces(real));
    HdMaterialNode2 vec = node("ND_displacement_vector3");
    vec.parameters[TfToken("displacement")] = VtValue(GfVec3f(0.0f, 0.2f, 0.0f));
    CHECK(displaces(vec));
}

TEST_CASE("Houdini's material libraries translate", "[hydra][karma][coverage]") {
    // Every material of $HFS/houdini/usd/materials renders its first
    // context; unsupported nodes are only those deferred to later M4 steps.
    const std::set<std::string> deferred = {};
    for (char const *file : {"/houdini/usd/materials/basic_materials/basic_materials.usd",
                             "/houdini/usd/materials/quick_materials/quick_materials.usd"}) {
        UsdStageRefPtr stage = UsdStage::Open(std::string(NOVA_HFS) + file);
        REQUIRE(stage);
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        const UsdImagingSceneIndices indices = UsdImagingCreateSceneIndices(info);
        int materials = 0, translated = 0;
        std::set<std::string> unsupported;
        for (UsdPrim const &prim : stage->Traverse()) {
            if (!prim.IsA<UsdShadeMaterial>())
                continue;
            ++materials;
            TfTokenVector chain = HdNovaMaterialRenderContexts();
            chain.push_back(TfToken());
            for (TfToken const &context : chain) {
                HdNovaNetworkSource src;
                if (!HdNovaReadNetwork(indices.finalSceneIndex, prim.GetPath(), context, &src))
                    continue;
                if (!src.net.terminals.count(HdMaterialTerminalTokens->surface))
                    break; // a volume-only material (M8)
                const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
                INFO(prim.GetPath().GetString() << " (" << context.GetString() << "): " << tr.error);
                CHECK(tr.ok);
                translated += tr.ok;
                for (std::string const &w : tr.warnings) {
                    if (w.find("displacement is not supported yet") != std::string::npos)
                        continue; // M4.6
                    const size_t q = w.find('\'');
                    const std::string name = q != std::string::npos ? w.substr(q + 1, w.find('\'', q + 1) - q - 1) : w;
                    if (!deferred.count(name))
                        unsupported.insert(w);
                }
                break;
            }
        }
        INFO(file);
        CHECK(materials > 0);
        CHECK(translated > 0);
        for (std::string const &w : unsupported)
            WARN(w);
        CHECK(unsupported.empty());
    }
}

TEST_CASE("Houdini ramps match Karma", "[hydra][karma][ramps]") {
    // tests/data/karma_ramps.json (tests/karma/probe_ramps.py): Karma's
    // kma_rampconst_float per basis at t = (x + 0.5) / 200. The ramp's t is a
    // ray import of the hit distance here, so one translation serves every t.
    std::ifstream file(std::string(NOVA_TEST_DIR) + "/data/karma_ramps.json");
    REQUIRE(file);
    std::stringstream text;
    text << file.rdbuf();
    const JsValue root = JsParseString(text.str());
    REQUIRE(root.IsObject());
    JsObject const &golden = root.GetJsObject();
    std::vector<double> ts;
    for (JsValue const &t : golden.at("t").GetJsArray())
        ts.push_back(t.GetReal());
    for (auto const &[key, row] : golden.at("karma").GetJsObject()) {
        const std::string setName = key.substr(0, key.find(':')), basis = key.substr(key.find(':') + 1);
        JsObject const &set = golden.at("sets").GetJsObject().at(setName).GetJsObject();
        VtFloatArray keys, values;
        for (JsValue const &k : set.at("keys").GetJsArray())
            keys.push_back(float(k.GetReal()));
        for (JsValue const &v : set.at("values").GetJsArray())
            values.push_back(float(v.GetReal()));
        if (basis == "bezier" && (keys.size() - 1) % 3 != 0)
            continue; // incomplete Bezier segments: Nova approximates them as linear
        Network net;
        HdMaterialNode2 &ramp = net.add("ramp", node("kma_rampconst_float"));
        ramp.parameters[TfToken("fkeys")] = VtValue(keys);
        ramp.parameters[TfToken("fvalues")] = VtValue(values);
        ramp.parameters[TfToken("fbasis")] = VtValue(VtArray<TfToken>(keys.size(), TfToken(basis)));
        HdMaterialNode2 &t = net.add("t", node("kma_rayimport_float"));
        t.parameters[TfToken("name")] = VtValue(std::string("hitdist"));
        net.connect("ramp", "t", "t");
        net.emission("ramp");
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
        CHECK(tr.warnings.empty());
        // monotonecubic: Karma uses Kupan's slopes, Nova the averaged ones.
        const double tolerance = basis == "monotonecubic" ? 7e-3 : 2e-5;
        JsArray const &karma = row.GetJsArray();
        double worst = 0.0;
        for (size_t i = 0; i < ts.size(); ++i) {
            Globals g = camera(float(ts[i]));
            worst = std::max(worst, std::fabs(double(emission(tr, g)[1]) - karma[i].GetReal()));
        }
        INFO(key << ": worst difference " << worst);
        CHECK(worst <= tolerance);
    }
}

TEST_CASE("Color ramps interpolate every channel", "[hydra][karma][ramps]") {
    Network net;
    HdMaterialNode2 &ramp = net.add("ramp", node("kma_rampconst_color"));
    ramp.parameters[TfToken("vkeys")] = VtValue(VtFloatArray{0.0f, 1.0f});
    ramp.parameters[TfToken("vvalues")] = VtValue(VtVec3fArray{GfVec3f(0.0f, 1.0f, 0.2f), GfVec3f(1.0f, 0.0f, 0.4f)});
    ramp.parameters[TfToken("vbasis")] = VtValue(VtArray<TfToken>{TfToken("linear"), TfToken("linear")});
    HdMaterialNode2 &t = net.add("t", node("kma_rayimport_float"));
    t.parameters[TfToken("name")] = VtValue(std::string("hitdist"));
    net.connect("ramp", "t", "t");
    net.emission("ramp");
    const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
    const std::vector<float> e = emission(tr, camera(0.25f));
    CHECK_THAT(e[0], WithinAbs(0.25, 1e-6));
    CHECK_THAT(e[1], WithinAbs(0.75, 1e-6));
    CHECK_THAT(e[2], WithinAbs(0.25, 1e-6));
}

TEST_CASE("MikkT normal maps use the tangentu and signs primvars", "[hydra][karma][normals]") {
    // Karma's measurements (docs/research/m4/nodes-and-limits.md 3.1) on a
    // quad facing +z, identity transform, uv along x and y.
    auto mapped = [](GfVec3f in, float scale, bool mikkt, Primvars const &primvars) {
        HdNovaNetworkSource src;
        src.context = TfToken("mtlx");
        HdMaterialNode2 surface = node("ND_standard_surface_surfaceshader");
        surface.inputConnections[TfToken("normal")] = {{SdfPath("nmap"), TfToken("out")}};
        src.net.nodes[SdfPath("surface")] = surface;
        src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("surface"), TfToken()};
        HdMaterialNode2 nmap = node("ND_normalmap_float");
        nmap.parameters[TfToken("in")] = VtValue(in);
        nmap.parameters[TfToken("scale")] = VtValue(scale);
        if (mikkt)
            nmap.parameters[TfToken("karma_mikkt")] = VtValue(1);
        src.net.nodes[SdfPath("nmap")] = nmap;
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
        Globals g = camera();
        g[Global::ObjX] = {1.0f, 0.0f, 0.0f};
        g[Global::ObjY] = {0.0f, 1.0f, 0.0f};
        g[Global::ObjZ] = {0.0f, 0.0f, 1.0f};
        g[Global::DPdu] = {1.0f, 0.0f, 0.0f};
        g[Global::DPdv] = {0.0f, 1.0f, 0.0f};
        return evaluate(tr, sf::normal, 3, g, primvars);
    };
    auto check = [](std::vector<float> const &r, float x, float y, float z) {
        CHECK_THAT(r[0], WithinAbs(x, 1e-4));
        CHECK_THAT(r[1], WithinAbs(y, 1e-4));
        CHECK_THAT(r[2], WithinAbs(z, 1e-4));
    };
    // T = (2, 0, 1) (unnormalized), v = (1, 0, 0).
    check(mapped(GfVec3f(1.0f, 0.5f, 0.5f), 1.0f, true, {{"tangentu", {2, 0, 1}}, {"signs", {1}}}), 0.8944f, 0.0f, 0.4472f);
    // v = (0.5, 0.5, 1), signs missing (-1).
    check(mapped(GfVec3f(0.75f, 0.75f, 1.0f), 1.0f, true, {{"tangentu", {2, 0, 1}}}), 0.4851f, -0.4851f, 0.7276f);
    // scale 2 multiplies v.xy.
    check(mapped(GfVec3f(0.75f, 0.75f, 1.0f), 2.0f, true, {{"tangentu", {1, 0, 0}}}), 0.57735f, -0.57735f, 0.57735f);
    // No tangentu: Karma's uv tangent, signs -1 (only the bitangent flips).
    check(mapped(GfVec3f(0.5f, 1.0f, 0.5f), 1.0f, true, {}), 0.0f, -1.0f, 0.0f);
    check(mapped(GfVec3f(0.5f, 1.0f, 0.5f), 1.0f, false, {}), 0.0f, 1.0f, 0.0f);
}

TEST_CASE("Flakes match Karma", "[hydra][karma][flakes]") {
    // tests/data/karma_flakes.json (tests/karma/probe_flakes.py): Karma's
    // flake2d at 32 x 32 pixel centers of a unit quad; Nova evaluates the same
    // network at Karma's st. Presence, rand and id agree bit for bit (the
    // MaterialX hashes), the flake normal to float precision.
    std::ifstream file(std::string(NOVA_TEST_DIR) + "/data/karma_flakes.json");
    REQUIRE(file);
    std::stringstream text;
    text << file.rdbuf();
    const JsValue root = JsParseString(text.str());
    REQUIRE(root.IsObject());
    JsObject const &golden = root.GetJsObject();
    auto flakeNetwork = [&](char const *roughness, bool normal) {
        Network net;
        HdMaterialNode2 &f = net.add("f", node("ND_flake2d"));
        f.parameters[TfToken("size")] = VtValue(float(golden.at("size").GetReal()));
        f.parameters[TfToken("coverage")] = VtValue(float(golden.at("coverage").GetReal()));
        f.parameters[TfToken("roughness")] = VtValue(float(std::atof(roughness)));
        if (normal) {
            net.add("e", node("ND_convert_vector3_color3"));
            net.connect("e", "in", "f", "flakenormal");
        } else {
            net.add("idf", node("ND_convert_integer_float"));
            net.connect("idf", "in", "f", "id");
            HdMaterialNode2 &idn = net.add("idn", node("ND_divide_float"));
            idn.parameters[TfToken("in2")] = VtValue(16777216.0f);
            net.connect("idn", "in1", "idf");
            net.add("e", node("ND_combine3_color3"));
            net.connect("e", "in1", "f", "presence");
            net.connect("e", "in2", "f", "rand");
            net.connect("e", "in3", "idn");
        }
        net.emission("e");
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
        CHECK(tr.warnings.empty());
        return tr;
    };
    JsArray const &st = golden.at("st").GetJsArray();
    auto run = [&](HdNovaTranslatedMaterial const &tr, char const *key, double tolerance) {
        JsArray const &karma = golden.at(key).GetJsArray();
        int wrong = 0;
        double worst = 0.0;
        for (size_t i = 0; i < st.size(); ++i) {
            JsArray const &uv = st[i].GetJsArray();
            Globals g = camera();
            g[Global::DPdu] = {1.0f, 0.0f, 0.0f};
            g[Global::DPdv] = {0.0f, 1.0f, 0.0f};
            const std::vector<float> e =
                evaluate(tr, sf::emissionColor, 3, g, {{"st", {float(uv[0].GetReal()), float(uv[1].GetReal())}}});
            JsArray const &k = karma[i].GetJsArray();
            for (int c = 0; c < 3; ++c) {
                const double d = std::fabs(double(e[size_t(c)]) - k[size_t(c)].GetReal());
                worst = std::max(worst, d);
                wrong += d > tolerance;
            }
        }
        INFO(key << ": worst " << worst);
        CHECK(wrong == 0);
    };
    run(flakeNetwork("0.1", false), "ids", 1e-6);
    run(flakeNetwork("0.1", true), "normal0.1", 2e-5);
    run(flakeNetwork("0.5", true), "normal0.5", 2e-5);
}

TEST_CASE("Karma's nodes lower or are deferred", "[hydra][karma][coverage]") {
    // Every kma_* definition with a numeric output, connected to an unlit
    // surface. Deferred: shaders of later milestones (hair, fur, volumes),
    // what is not a material (light filters, lenses), and the nodes whose
    // data Nova does not read yet.
    static const std::set<std::string> deferred = {
        "kma_hair", "kma_fur", "kma_fur_2", "kma_volume", "kma_whitewater", "kma_pyropreview",           // M8, M9
        "kma_lfilter_attenuation", "kma_lfilter_barndoor", "kma_lfilter_blocker", "kma_lfilter_gel",     // lights
        "kma_lfilter_gobo", "kma_light_projection", "kma_physicallens",                                  // not materials
        "kma_pcread", "kma_roommap", "kma_room_multimap", "kma_pathswitch",                              // later
        "kma_rayimport_mat2", "kma_rayimport_mat3", "kma_rayimport_mat4",                                // matrices
    };
    MaterialX::ConstDocumentPtr lib = HdNovaNodeDefDatabase::Get().GetLibrary();
    std::vector<std::string> failures;
    int lowered = 0;
    for (MaterialX::NodeDefPtr const &nd : lib->getNodeDefs()) {
        if (nd->getName().rfind("kma_", 0) != 0 || deferred.count(nd->getName()))
            continue;
        MaterialX::OutputPtr out;
        for (MaterialX::OutputPtr const &o : nd->getActiveOutputs())
            if (typeWidth(o->getType()) > 0)
                out = o;
        if (!out)
            continue;
        Network net;
        net.add("probe", node(nd->getName().c_str()));
        net.emission("probe", out->getName().c_str());
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
        if (!tr.ok || !tr.warnings.empty())
            failures.push_back(nd->getName() + ": " + (tr.ok ? tr.warnings.front() : tr.error));
        else if (!sh::compile(tr.graph).program)
            failures.push_back(nd->getName() + ": does not compile");
        else
            ++lowered;
    }
    for (std::string const &f : failures)
        UNSCOPED_INFO(f);
    CHECK(failures.empty());
    CHECK(lowered >= 35);
    WARN("Karma nodes lowered: " << lowered);
}

namespace {

// Records a query's direction and parameters and answers 0.3, 0.4, 0.5, 0.6.
struct RecordingQueries final : nova::IShadingQueries {
    mutable nova::QueryKind kind = nova::QueryKind::Occlusion;
    mutable float dir[3] = {}, params[4] = {};
    mutable int calls = 0;
    void run(nova::QueryKind k, int count, nova::QueryLane const *, float const *d, float const *p,
             float *out) const override {
        kind = k;
        ++calls;
        for (int i = 0; i < 3; ++i)
            dir[i] = d[i];
        for (int i = 0; i < 4; ++i)
            params[i] = p[i];
        for (int l = 0; l < count; ++l)
            for (int i = 0; i < 4; ++i)
                out[l * 4 + i] = 0.3f + 0.1f * float(i);
    }
};

} // namespace

TEST_CASE("Karma's ray queries lower to query ops", "[hydra][karma][queries]") {
    RecordingQueries q;
    nova::QueryLane lane{};
    nova::TextureContext tex;
    tex.queries = &q;
    tex.lanes = &lane;
    auto run = [&](Network &net, char const *output, int w = 3) {
        net.emission("probe", output);
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
        CHECK(tr.warnings.empty());
        return std::make_pair(evaluate(tr, sf::emissionColor, w, camera(), {}, &tex),
                              evaluate(tr, sf::emissionColor, w, camera()));
    };

    // kma_ao: Karma's defaults; an authored constant nN is ignored, the
    // shading normal traced.
    Network ao;
    ao.add("probe", node("kma_ao")).parameters[TfToken("nN")] = VtValue(GfVec3f(1.0f, 0.0f, 0.0f));
    auto [aoQ, aoDefault] = run(ao, "visibility");
    CHECK(q.kind == nova::QueryKind::Occlusion);
    CHECK(q.dir[2] == 1.0f);
    CHECK(q.params[0] == 90.0f);
    CHECK(q.params[1] == 1e38f);
    CHECK(q.params[2] == 4.0f);
    CHECK(q.params[3] == 1.0f);
    CHECK_THAT(aoQ[0], WithinAbs(0.3, 1e-6));
    CHECK(aoDefault[0] == 1.0f); // without a scene: fully visible
    // A connected nN is traced.
    Network connected;
    connected.add("probe", node("kma_ao"));
    connected.add("view", node("ND_viewdirection_vector3"));
    connected.connect("probe", "nN", "view");
    run(connected, "visibility");
    CHECK(q.dir[2] == -1.0f);

    // kma_distance: flags mode | normalize << 1 | frontface << 2.
    Network dist;
    dist.add("probe", node("kma_distance")).parameters[TfToken("mode")] = VtValue(1);
    auto [distQ, distDefault] = run(dist, "dist");
    CHECK(q.kind == nova::QueryKind::Distance);
    CHECK(q.params[1] == 1.0f);
    CHECK(q.params[2] == 16.0f);
    CHECK(q.params[3] == 7.0f);
    CHECK_THAT(distQ[0], WithinAbs(0.3, 1e-6));
    CHECK(distDefault[0] == 1.0f); // nothing within the distance
    Network distDir;
    distDir.add("probe", node("kma_distance"));
    CHECK_THAT(run(distDir, "dir").first[2], WithinAbs(0.6, 1e-6));

    // kma_curvature: convex 0.3, concave 0.4.
    Network curv;
    curv.add("probe", node("kma_curvature"));
    auto [curvQ, curvDefault] = run(curv, "curvature");
    CHECK(q.kind == nova::QueryKind::Curvature);
    CHECK_THAT(q.params[0], WithinAbs(0.1, 1e-7));
    CHECK(q.params[1] == 16.0f);
    CHECK_THAT(curvQ[0], WithinAbs(0.45, 1e-5));
    CHECK_THAT(curvDefault[0], WithinAbs(0.5, 1e-6)); // flat
    Network signedCurv;
    signedCurv.add("probe", node("kma_curvature")).parameters[TfToken("space")] = VtValue(1);
    CHECK_THAT(run(signedCurv, "curvature").first[0], WithinAbs(-0.1, 1e-5));
    // Bias above 0.5 adds contrast: Perlin's gain(0.8, 0.3) = 0.1527.
    Network contrast;
    contrast.add("probe", node("kma_curvature")).parameters[TfToken("convexbias")] = VtValue(0.8f);
    CHECK_THAT(run(contrast, "convex").first[0], WithinAbs(0.1527, 1e-3));

    // kma_roundededge: without a scene the normal stays.
    Network round;
    round.add("probe", node("kma_roundededge"));
    auto [roundQ, roundDefault] = run(round, "roundN");
    CHECK(q.kind == nova::QueryKind::RoundedEdge);
    CHECK_THAT(q.params[0], WithinAbs(0.01, 1e-7));
    CHECK(q.params[1] == 4.0f);
    CHECK(q.params[3] == 30.0f);
    CHECK_THAT(roundQ[1], WithinAbs(0.4, 1e-6));
    CHECK(roundDefault[2] == 1.0f);
    Network roundMask;
    roundMask.add("probe", node("kma_roundededge"));
    CHECK_THAT(run(roundMask, "roundMask", 1).first[0], WithinAbs(0.6, 1e-6));

    // MaterialX's withdrawn ambientocclusion: Karma's AO.
    Network mx;
    mx.add("probe", node("ND_ambientocclusion_float")).parameters[TfToken("maxdistance")] = VtValue(2.0f);
    auto [mxQ, mxDefault] = run(mx, "out");
    CHECK(q.kind == nova::QueryKind::Occlusion);
    CHECK(q.params[1] == 2.0f);
    CHECK_THAT(mxQ[0], WithinAbs(0.3, 1e-6));
    CHECK(mxDefault[0] == 1.0f);
}

TEST_CASE("Karma's graph nodes: tangentrotate and melanin", "[hydra][karma]") {
    // From Karma's own graphs (karmaNodeGraphs.json). tangentrotate turns
    // the tangent (default: Tworld) by 360 * amount degrees about N.
    Network rot;
    HdMaterialNode2 &t = rot.add("t", node("kma_tangentrotate"));
    t.parameters[TfToken("amount")] = VtValue(0.25f);
    rot.emission("t");
    const HdNovaTranslatedMaterial trRot = HdNovaTranslateMaterial(rot.src);
    CHECK(trRot.warnings.empty());
    Globals g = camera();
    g[Global::DPdu] = {1.0f, 0.0f, 0.0f};
    g[Global::DPdv] = {0.0f, 1.0f, 0.0f};
    const std::vector<float> e = emission(trRot, g);
    CHECK_THAT(e[0], WithinAbs(0.0, 1e-5));
    CHECK_THAT(std::fabs(e[1]), WithinAbs(1.0, 1e-5));
    // melanin, mode 0: exp(-melanin scale (eumelanin 15 (1 - redness) + pheomelanin 30 redness)),
    // eumelanin sigma (0.419, 0.697, 1.37) for pheomelanin 0.
    Network mel;
    HdMaterialNode2 &m = mel.add("m", node("kma_melanin"));
    m.parameters[TfToken("melanin")] = VtValue(0.5f);
    mel.emission("m", "color");
    const HdNovaTranslatedMaterial trMel = HdNovaTranslateMaterial(mel.src);
    CHECK(trMel.warnings.empty());
    const std::vector<float> c = emission(trMel, camera());
    CHECK_THAT(c[0], WithinRel(std::exp(-7.5 * 0.419), 1e-4));
    CHECK_THAT(c[1], WithinRel(std::exp(-7.5 * 0.697), 1e-4));
}

TEST_CASE("Blackbody matches Karma's measured ND_blackbody", "[hydra][karma][blackbody]") {
    // tests/data/karma_blackbody.json (tests/karma/probe_blackbody.py): 257
    // temperatures log-uniform from 500 K to 40000 K; the even ones are
    // Nova's table, the odd ones test its interpolation (docs/design/M8.md D14).
    std::ifstream file(std::string(NOVA_TEST_DIR) + "/data/karma_blackbody.json");
    REQUIRE(file);
    std::stringstream text;
    text << file.rdbuf();
    const JsValue root = JsParseString(text.str());
    REQUIRE(root.IsObject());
    JsArray const &kelvin = root.GetJsObject().at("kelvin").GetJsArray();
    JsArray const &rgb = root.GetJsObject().at("rgb").GetJsArray();
    REQUIRE(kelvin.size() == 257);
    double worst = 0.0;
    for (size_t i = 0; i < kelvin.size(); ++i) {
        const float t = float(kelvin[i].GetReal());
        JsArray const &c = rgb[i].GetJsArray();
        const nova::Spectrum n = nova::blackbody(t);
        const double ref[3] = {c[0].GetReal(), c[1].GetReal(), c[2].GetReal()};
        const double y = 0.2126 * ref[0] + 0.7152 * ref[1] + 0.0722 * ref[2];
        const double got[3] = {n.x, n.y, n.z};
        for (int k = 0; k < 3; ++k)
            worst = std::max(worst, std::fabs(got[k] - ref[k]) / y); // relative to the luminance
    }
    INFO("worst deviation relative to the luminance: " << worst);
    CHECK(worst < 2e-3);
    // Outside the table: power-scaled ends; nothing at 0 K.
    CHECK(nova::blackbody(0.0f).x == 0.0f);
    CHECK_THAT(nova::blackbody(80000.0f).y / nova::blackbody(40000.0f).y, WithinRel(16.0, 1e-4));
}

TEST_CASE("Karma's spaces: current is world, positions in NDC", "[hydra][karma]") {
    // The Background Plate's projection (docs/design/M11-P2.md): the ray
    // direction from space:current to space:world, the hit in space:ndc.
    Network net;
    HdMaterialNode2 &dir = net.add("dir", node("ND_dot_vector3"));
    dir.parameters[TfToken("note")] = VtValue(std::string("ray:direction"));
    HdMaterialNode2 &xf = net.add("xf", node("ND_transformvector_vector3"));
    xf.parameters[TfToken("fromspace")] = VtValue(std::string("space:current"));
    xf.parameters[TfToken("tospace")] = VtValue(std::string("space:world"));
    net.connect("xf", "in", "dir");
    net.emission("xf");
    const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
    CHECK(tr.warnings.empty());
    Globals g = camera();
    g[Global::I] = {0.6f, 0.0f, -0.8f};
    const std::vector<float> e = emission(tr, g);
    CHECK_THAT(e[0], WithinAbs(0.6, 1e-6));
    CHECK_THAT(e[2], WithinAbs(-0.8, 1e-6));

    Network ndc;
    HdMaterialNode2 &pos = ndc.add("pos", node("ND_position_vector3"));
    pos.parameters[TfToken("space")] = VtValue(std::string("space:ndc"));
    ndc.emission("pos");
    const HdNovaTranslatedMaterial trn = HdNovaTranslateMaterial(ndc.src);
    CHECK(trn.warnings.empty());
    Globals gn = camera();
    gn[Global::Pndc] = {0.25f, 0.75f, 3.0f};
    const std::vector<float> p = emission(trn, gn);
    CHECK(p == std::vector<float>{0.25f, 0.75f, 3.0f});

    // An unknown space warns and falls back.
    Network cam;
    HdMaterialNode2 &pc = cam.add("pos", node("ND_position_vector3"));
    pc.parameters[TfToken("space")] = VtValue(std::string("space:camera"));
    cam.emission("pos");
    CHECK(hasWarning(HdNovaTranslateMaterial(cam.src), "space 'camera'"));
}
