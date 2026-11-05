// M4.5 generic MaterialX surfaces through the translator (docs/design/M4.md
// 3.4, docs/research/m4/bsdf-graphs.md 7.2):
// - equivalence: Standard Surface, OpenPBR and UsdPreviewSurface expanded
//   through their own MaterialX nodegraphs (NOVA_MTLX_EXPAND) build the
//   same BSDF as the native models, outside the documented omissions;
// - glTF PBR, Disney principled, Lama and user graphs translate and keep
//   energy where their definitions do;
// - mix_surfaceshader of native models, legacy thin film, pass-throughs.

#include "hydra/material/networkSource.h"
#include "hydra/material/nodeDefDatabase.h"
#include "hydra/material/translator.h"

#include "core/shading/compiler.h"
#include "core/shading/vm.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>

#include <MaterialXCore/Document.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;
namespace sh = nova::shading;
namespace sf = nova::surface_field;
using sh::Global;

namespace {

HdMaterialNode2 node(char const *id) {
    HdMaterialNode2 n;
    n.nodeTypeId = TfToken(id);
    return n;
}

struct Network {
    HdNovaNetworkSource src;
    explicit Network(char const *surfaceId) {
        src.context = TfToken("mtlx");
        src.net.nodes[SdfPath("/surface")] = node(surfaceId);
        src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("/surface"), TfToken()};
    }
    HdMaterialNode2 &add(char const *path, char const *id) { return src.net.nodes[SdfPath(path)] = node(id); }
    HdMaterialNode2 &surface() { return src.net.nodes[SdfPath("/surface")]; }
    void connect(char const *to, char const *input, char const *from, char const *output = "out") {
        src.net.nodes[SdfPath(to)].inputConnections[TfToken(input)] = {{SdfPath(from), TfToken(output)}};
    }
};

// Sets NOVA_MTLX_EXPAND while alive.
struct Expand {
    explicit Expand(char const *categories) { _putenv_s("NOVA_MTLX_EXPAND", categories); }
    ~Expand() { _putenv_s("NOVA_MTLX_EXPAND", ""); }
};

// A translated material evaluated at the flat hit (+z) seen from wo.
struct Evaluated {
    sh::ProgramPtr program;
    nova::SurfaceInputs in;
};
Evaluated evaluate(HdNovaTranslatedMaterial const &tr, nova::Vec3f wo) {
    REQUIRE(tr.ok);
    const sh::CompileResult r = sh::compile(tr.graph);
    REQUIRE(r.program);
    const sh::Program &p = *r.program;
    std::vector<float> U;
    p.initUniforms(tr.params.data(), U);
    std::vector<float> R(std::max<size_t>(p.rowCount, 1) + 4, 0.0f);
    const std::map<Global, std::vector<float>> globals = {
        {Global::N, {0.0f, 0.0f, 1.0f}},   {Global::Ng, {0.0f, 0.0f, 1.0f}}, {Global::P, {0.0f, 0.0f, 0.0f}},
        {Global::I, {-wo.x, -wo.y, -wo.z}}, {Global::DPdu, {1.0f, 0.0f, 0.0f}}, {Global::DPdv, {0.0f, 1.0f, 0.0f}},
        {Global::ObjX, {1.0f, 0.0f, 0.0f}}, {Global::ObjY, {0.0f, 1.0f, 0.0f}}, {Global::ObjZ, {0.0f, 0.0f, 1.0f}},
        {Global::Nobject, {0.0f, 0.0f, 1.0f}}};
    for (sh::GlobalBinding const &g : p.globals)
        if (const auto it = globals.find(g.global); it != globals.end())
            for (size_t k = 0; k < it->second.size(); ++k)
                R[g.row + k] = it->second[k];
    for (sh::PrimvarBinding const &b : p.primvarBindings)
        for (int k = 0; k < b.width; ++k)
            R[size_t(b.row) + size_t(k)] = U[size_t(b.fallback) + size_t(k)];
    sh::execute<1>(p.code.data(), p.code.size(), R.data(), U.data());
    Evaluated e;
    e.program = r.program;
    e.in = tr.defaults;
    e.in.model = tr.graph.model;
    e.in.graph = r.program->closure.get();
    e.in.size = sf::kBlockSize;
    for (sh::OutputBinding const &o : p.outputs)
        for (int k = 0; k < o.width; ++k)
            e.in.v[o.field + k] = o.uniform ? U[size_t(o.index) + size_t(k)] : R[size_t(o.index) + size_t(k)];
    return e;
}

nova::ClosureContext context() { return {&nova::bsdfTables(), nova::kXyzToRec709Grey}; }
const nova::ShadingGeometry kFlat{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, true};

nova::ClosureList closure(Evaluated const &e, nova::Vec3f wo) {
    nova::ClosureList cl;
    nova::buildClosure(e.in, kFlat, wo, context(), cl);
    return cl;
}

nova::Vec3f direction(float mu, float phi = 0.7f) {
    const float s = std::sqrt(std::fmax(0.0f, 1.0f - mu * mu));
    return {s * std::cos(phi), s * std::sin(phi), mu};
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    float next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return float(s >> 40) * (1.0f / 16777216.0f);
    }
    float in(float lo, float hi) { return lo + (hi - lo) * next(); }
};

// The native and the expanded material give the same BSDF, albedo and
// emission at wo.
void compareExpanded(HdNovaNetworkSource const &src, char const *category, nova::Vec3f wo, std::string const &what) {
    const HdNovaTranslatedMaterial native = HdNovaTranslateMaterial(src);
    HdNovaTranslatedMaterial expanded;
    {
        Expand expand(category);
        expanded = HdNovaTranslateMaterial(src);
    }
    INFO(what);
    for (std::string const &w : expanded.warnings)
        INFO("warning: " << w);
    REQUIRE(native.graph.model != nova::SurfaceModel::Graph);
    REQUIRE(expanded.graph.model == nova::SurfaceModel::Graph);
    const Evaluated a = evaluate(native, wo), b = evaluate(expanded, wo);
    const nova::ClosureList ca = closure(a, wo), cb = closure(b, wo);
    for (int c = 0; c < 3; ++c) {
        CHECK_THAT(cb.albedo[c], WithinAbs(ca.albedo[c], 2e-4 + 2e-4 * std::fabs(ca.albedo[c])));
        CHECK_THAT(cb.emission[c], WithinAbs(ca.emission[c], 1e-5 + 1e-4 * std::fabs(ca.emission[c])));
    }
    Rng rng(17);
    for (int i = 0; i < 48; ++i) {
        const float z = rng.in(-1.0f, 1.0f), phi = rng.in(0.0f, 6.2831853f), r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
        const nova::Vec3f wi{r * std::cos(phi), r * std::sin(phi), z};
        const nova::ClosureEval ea = nova::evalClosure(ca, wo, wi), eb = nova::evalClosure(cb, wo, wi);
        for (int c = 0; c < 3; ++c)
            CHECK_THAT(eb.fcos[c], WithinAbs(ea.fcos[c], 1e-5 + 2e-4 * std::fabs(ea.fcos[c])));
    }
}

void setFloat(HdMaterialNode2 &n, char const *name, float v) { n.parameters[TfToken(name)] = VtValue(v); }
void setColor(HdMaterialNode2 &n, char const *name, float r, float g, float b) {
    n.parameters[TfToken(name)] = VtValue(GfVec3f(r, g, b));
}

} // namespace

TEST_CASE("Standard Surface expanded equals native", "[hydra][graphs]") {
    Rng rng(3);
    for (int set = 0; set < 12; ++set) {
        Network net("ND_standard_surface_surfaceshader");
        HdMaterialNode2 &s = net.surface();
        const bool transmissive = set % 3 == 1;
        setFloat(s, "base", rng.in(0.5f, 1.0f));
        setColor(s, "base_color", rng.next(), rng.next(), rng.next());
        setFloat(s, "diffuse_roughness", set % 2 ? rng.next() : 0.0f);
        setFloat(s, "metalness", set % 4 == 2 ? rng.next() : 0.0f);
        setFloat(s, "specular", rng.next());
        setColor(s, "specular_color", rng.in(0.5f, 1.0f), rng.in(0.5f, 1.0f), rng.in(0.5f, 1.0f));
        setFloat(s, "specular_roughness", rng.in(0.15f, 0.9f));
        setFloat(s, "specular_IOR", rng.in(1.2f, 2.2f));
        setFloat(s, "transmission", transmissive ? rng.next() : 0.0f);
        setColor(s, "transmission_color", rng.in(0.3f, 1.0f), rng.in(0.3f, 1.0f), rng.in(0.3f, 1.0f));
        setFloat(s, "transmission_extra_roughness", set % 2 ? 0.2f : 0.0f);
        setFloat(s, "subsurface", set % 3 == 0 ? rng.next() : 0.0f);
        setColor(s, "subsurface_color", rng.next(), rng.next(), rng.next());
        setFloat(s, "sheen", set % 2 ? rng.next() : 0.0f);
        setColor(s, "sheen_color", rng.next(), rng.next(), rng.next());
        setFloat(s, "sheen_roughness", rng.in(0.1f, 0.9f));
        setFloat(s, "coat", set % 3 == 2 ? rng.next() : 0.0f);
        setColor(s, "coat_color", rng.in(0.5f, 1.0f), rng.in(0.5f, 1.0f), rng.in(0.5f, 1.0f));
        setFloat(s, "coat_roughness", rng.in(0.1f, 0.6f));
        setFloat(s, "coat_IOR", rng.in(1.3f, 1.8f));
        setFloat(s, "coat_affect_color", rng.next());
        setFloat(s, "coat_affect_roughness", rng.next());
        setFloat(s, "thin_film_thickness", !transmissive && set % 4 == 3 ? 450.0f : 0.0f);
        setFloat(s, "emission", rng.next());
        setColor(s, "emission_color", rng.next(), rng.next(), rng.next());
        for (const float mu : {0.3f, 0.85f})
            compareExpanded(net.src, "standard_surface", direction(mu),
                            "set " + std::to_string(set) + " mu " + std::to_string(mu));
    }
}

TEST_CASE("OpenPBR expanded equals native", "[hydra][graphs]") {
    Rng rng(5);
    for (int set = 0; set < 12; ++set) {
        Network net("ND_open_pbr_surface_surfaceshader");
        HdMaterialNode2 &s = net.surface();
        const bool transmissive = set % 3 == 1;
        setFloat(s, "base_weight", rng.in(0.5f, 1.0f));
        setColor(s, "base_color", rng.next(), rng.next(), rng.next());
        setFloat(s, "base_diffuse_roughness", rng.next());
        setFloat(s, "base_metalness", set % 4 == 2 ? rng.next() : 0.0f);
        setFloat(s, "specular_weight", rng.next());
        setColor(s, "specular_color", rng.in(0.5f, 1.0f), rng.in(0.5f, 1.0f), rng.in(0.5f, 1.0f));
        setFloat(s, "specular_roughness", rng.in(0.15f, 0.9f));
        setFloat(s, "specular_ior", rng.in(1.2f, 2.2f));
        setFloat(s, "transmission_weight", transmissive ? rng.next() : 0.0f);
        setColor(s, "transmission_color", rng.in(0.3f, 1.0f), rng.in(0.3f, 1.0f), rng.in(0.3f, 1.0f));
        setFloat(s, "subsurface_weight", set % 3 == 0 ? rng.next() : 0.0f);
        setColor(s, "subsurface_color", rng.next(), rng.next(), rng.next());
        setFloat(s, "fuzz_weight", set % 2 ? rng.next() : 0.0f);
        setColor(s, "fuzz_color", rng.next(), rng.next(), rng.next());
        setFloat(s, "fuzz_roughness", rng.in(0.1f, 0.9f));
        setFloat(s, "coat_weight", set % 3 == 2 ? rng.next() : 0.0f);
        setColor(s, "coat_color", rng.in(0.5f, 1.0f), rng.in(0.5f, 1.0f), rng.in(0.5f, 1.0f));
        setFloat(s, "coat_roughness", rng.in(0.1f, 0.6f));
        setFloat(s, "coat_ior", rng.in(1.3f, 1.8f));
        setFloat(s, "coat_darkening", rng.next());
        setFloat(s, "thin_film_weight", !transmissive && set % 4 == 3 ? 0.8f : 0.0f);
        setFloat(s, "thin_film_thickness", 0.45f);
        setFloat(s, "emission_luminance", rng.next());
        setColor(s, "emission_color", rng.next(), rng.next(), rng.next());
        for (const float mu : {0.3f, 0.85f})
            compareExpanded(net.src, "open_pbr_surface", direction(mu),
                            "set " + std::to_string(set) + " mu " + std::to_string(mu));
    }
}

TEST_CASE("UsdPreviewSurface expanded equals native", "[hydra][graphs]") {
    Rng rng(11);
    for (int set = 0; set < 8; ++set) {
        Network net("UsdPreviewSurface");
        HdMaterialNode2 &s = net.surface();
        setColor(s, "diffuseColor", rng.next(), rng.next(), rng.next());
        setFloat(s, "metallic", set % 2 ? rng.next() : 0.0f);
        setFloat(s, "roughness", rng.in(0.15f, 0.9f));
        setFloat(s, "clearcoat", set % 3 == 0 ? rng.next() : 0.0f);
        setFloat(s, "clearcoatRoughness", rng.in(0.1f, 0.5f));
        setFloat(s, "ior", rng.in(1.2f, 2.0f));
        s.parameters[TfToken("useSpecularWorkflow")] = VtValue(set % 4 == 3 ? 1 : 0);
        setColor(s, "specularColor", rng.next(), rng.next(), rng.next());
        setColor(s, "emissiveColor", rng.next(), rng.next(), rng.next());
        for (const float mu : {0.3f, 0.85f})
            compareExpanded(net.src, "UsdPreviewSurface", direction(mu),
                            "set " + std::to_string(set) + " mu " + std::to_string(mu));
    }
}

namespace {

// Directional albedo of a material at wo by sampling its closure.
nova::Spectrum furnace(Evaluated const &e, nova::Vec3f wo, uint32_t n = 1u << 17) {
    const nova::ClosureList cl = closure(e, wo);
    Rng rng(29);
    nova::Spectrum sum = nova::spectrum(0.0f);
    for (uint32_t i = 0; i < n; ++i) {
        nova::ClosureSample s;
        if (nova::sampleClosure(cl, wo, {rng.next(), rng.next(), rng.next()}, s))
            sum += s.weight * (s.refract ? s.eta * s.eta : 1.0f);
    }
    return sum * (1.0f / float(n));
}

bool unsupported(HdNovaTranslatedMaterial const &tr) {
    for (std::string const &w : tr.warnings)
        if (w.find("not supported") != std::string::npos || w.find("unknown") != std::string::npos ||
            w.find("no definition") != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE("Every closure definition lowers", "[hydra][graphs][coverage]") {
    // Each BSDF, EDF and VDF definition inside a surface, each surface
    // shader as the terminal, with its default inputs.
    static const std::map<std::string, char const *> deferred = {
        {"ND_chiang_hair_bsdf", "M9 (curves)"},
        {"ND_thin_surface", "legacy 1.38 (later)"},
        {"ND_disney_brdf_2012_surface", "legacy, no MaterialX implementation"},
        {"ND_disney_bsdf_2015_surface", "legacy, no MaterialX implementation"},
        {"ND_dot_surfaceshader", "a pass-through (needs an input)"},
    };
    MaterialX::ConstDocumentPtr lib = HdNovaNodeDefDatabase::Get().GetLibrary();
    std::vector<std::string> failures;
    int lowered = 0;
    for (MaterialX::NodeDefPtr const &nd : lib->getNodeDefs()) {
        const std::vector<MaterialX::OutputPtr> outputs = nd->getActiveOutputs();
        if (outputs.size() != 1 || deferred.count(nd->getName()))
            continue;
        const std::string type = outputs[0]->getType();
        if (type != "BSDF" && type != "EDF" && type != "VDF" && type != "surfaceshader")
            continue;
        Network net(type == "surfaceshader" ? nd->getName().c_str() : "ND_surface");
        if (type == "BSDF" || type == "EDF") {
            net.add("/probe", nd->getName().c_str());
            net.connect("/surface", type == "BSDF" ? "bsdf" : "edf", "/probe");
        } else if (type == "VDF") {
            HdMaterialNode2 &t = net.add("/glass", "ND_dielectric_bsdf");
            t.parameters[TfToken("scatter_mode")] = VtValue(std::string("T"));
            net.add("/probe", nd->getName().c_str());
            net.add("/layer", "ND_layer_vdf");
            net.connect("/layer", "top", "/glass");
            net.connect("/layer", "base", "/probe");
            net.connect("/surface", "bsdf", "/layer");
        }
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
        if (!tr.ok || unsupported(tr)) {
            std::string why = tr.error;
            for (std::string const &w : tr.warnings)
                why += " | " + w;
            failures.push_back(nd->getName() + ": " + why);
            continue;
        }
        if (!sh::compile(tr.graph).program) {
            failures.push_back(nd->getName() + ": does not compile");
            continue;
        }
        ++lowered;
    }
    for (std::string const &f : failures)
        UNSCOPED_INFO(f);
    CHECK(failures.empty());
    WARN("closure definitions lowered: " << lowered);
    CHECK(lowered >= 40);
}

TEST_CASE("glTF, Disney and Lama surfaces render", "[hydra][graphs]") {
    const nova::Vec3f wo = direction(0.6f);
    // glTF PBR, white dielectric: a white furnace.
    Network gltf("ND_gltf_pbr_surfaceshader");
    setColor(gltf.surface(), "base_color", 1.0f, 1.0f, 1.0f);
    setFloat(gltf.surface(), "metallic", 0.0f);
    setFloat(gltf.surface(), "roughness", 0.5f);
    const HdNovaTranslatedMaterial tg = HdNovaTranslateMaterial(gltf.src);
    CHECK_FALSE(unsupported(tg));
    const nova::Spectrum eg = furnace(evaluate(tg, wo), wo);
    CHECK_THAT(eg.x, WithinAbs(1.0, 0.01));

    // Disney principled: Burley diffuse under a Schlick specular (Burley
    // is not energy conserving by definition: about 1, never NaN).
    Network disney("ND_disney_principled");
    setColor(disney.surface(), "baseColor", 1.0f, 1.0f, 1.0f);
    const HdNovaTranslatedMaterial td = HdNovaTranslateMaterial(disney.src);
    CHECK_FALSE(unsupported(td));
    const nova::Spectrum ed = furnace(evaluate(td, wo), wo);
    CHECK(std::isfinite(ed.x));
    CHECK(ed.x > 0.8f);
    CHECK(ed.x < 1.3f);

    // LamaLayer(dielectric, white diffuse, topMix 0.5): multiply inside the
    // top is spec-linear (D5): still a white furnace (Karma: 0.5).
    Network lama("ND_lama_surface");
    HdMaterialNode2 &layer = lama.add("/layer", "ND_lama_layer_bsdf");
    setFloat(layer, "topMix", 0.5f);
    lama.add("/top", "ND_lama_dielectric");
    HdMaterialNode2 &base = lama.add("/base", "ND_lama_diffuse");
    setColor(base, "color", 1.0f, 1.0f, 1.0f);
    lama.connect("/layer", "materialTop", "/top");
    lama.connect("/layer", "materialBase", "/base");
    lama.connect("/surface", "materialFront", "/layer");
    const HdNovaTranslatedMaterial tl = HdNovaTranslateMaterial(lama.src);
    std::string warnings;
    for (std::string const &w : tl.warnings)
        warnings += w + "; ";
    INFO(warnings);
    CHECK_FALSE(unsupported(tl));
    const nova::Spectrum el = furnace(evaluate(tl, wo), wo);
    CHECK_THAT(el.x, WithinAbs(1.0, 0.01));
}

TEST_CASE("Surface mixes and legacy thin film translate", "[hydra][graphs]") {
    const nova::Vec3f wo = direction(0.55f);
    // mix_surfaceshader(Standard Surface copper, OpenPBR defaults, 0.25).
    Network mix("ND_mix_surfaceshader");
    setFloat(mix.surface(), "mix", 0.25f);
    HdMaterialNode2 &ss = mix.add("/ss", "ND_standard_surface_surfaceshader");
    setFloat(ss, "metalness", 1.0f);
    setColor(ss, "base_color", 0.95f, 0.64f, 0.54f);
    mix.add("/op", "ND_open_pbr_surface_surfaceshader");
    mix.connect("/surface", "fg", "/ss");
    mix.connect("/surface", "bg", "/op");
    const HdNovaTranslatedMaterial tm = HdNovaTranslateMaterial(mix.src);
    REQUIRE(tm.ok);
    REQUIRE(tm.graph.closure);
    CHECK(tm.graph.closure->segmentCount == 2);
    Network ssOnly("ND_standard_surface_surfaceshader");
    ssOnly.surface().parameters = ss.parameters;
    Network opOnly("ND_open_pbr_surface_surfaceshader");
    const Evaluated em = evaluate(tm, wo), es = evaluate(HdNovaTranslateMaterial(ssOnly.src), wo),
                    eo = evaluate(HdNovaTranslateMaterial(opOnly.src), wo);
    const nova::ClosureList cm = closure(em, wo), cs = closure(es, wo), co = closure(eo, wo);
    Rng rng(41);
    for (int i = 0; i < 32; ++i) {
        const float z = rng.in(0.0f, 1.0f), phi = rng.in(0.0f, 6.2831853f), r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
        const nova::Vec3f wi{r * std::cos(phi), r * std::sin(phi), z};
        const nova::Spectrum f = nova::evalClosure(cm, wo, wi).fcos;
        const nova::Spectrum g =
            nova::evalClosure(cs, wo, wi).fcos * 0.25f + nova::evalClosure(co, wo, wi).fcos * 0.75f;
        for (int c = 0; c < 3; ++c)
            CHECK_THAT(f[c], WithinAbs(g[c], 1e-5 + 1e-4 * g[c]));
    }

    // layer(thin_film_bsdf, dielectric) = the dielectric with the film
    // (MaterialX's 1.39 upgrade).
    auto filmed = [&](bool legacy) {
        Network net("ND_surface");
        HdMaterialNode2 &d = net.add("/d", "ND_dielectric_bsdf");
        d.parameters[TfToken("roughness")] = VtValue(GfVec2f(0.1f, 0.1f));
        if (legacy) {
            HdMaterialNode2 &film = net.add("/film", "ND_thin_film_bsdf");
            setFloat(film, "thickness", 400.0f);
            setFloat(film, "ior", 1.8f);
            net.add("/layer", "ND_layer_bsdf");
            net.connect("/layer", "top", "/film");
            net.connect("/layer", "base", "/d");
            net.connect("/surface", "bsdf", "/layer");
        } else {
            setFloat(d, "thinfilm_thickness", 400.0f);
            setFloat(d, "thinfilm_ior", 1.8f);
            net.connect("/surface", "bsdf", "/d");
        }
        const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(net.src);
        CHECK_FALSE(unsupported(tr));
        return closure(evaluate(tr, wo), wo);
    };
    const nova::ClosureList a = filmed(true), b = filmed(false);
    for (int c = 0; c < 3; ++c)
        CHECK_THAT(a.albedo[c], WithinAbs(b.albedo[c], 1e-6));
    const nova::Vec3f wi = direction(0.4f, 3.5f);
    for (int c = 0; c < 3; ++c)
        CHECK_THAT(nova::evalClosure(a, wo, wi).fcos[c], WithinAbs(nova::evalClosure(b, wo, wi).fcos[c], 1e-6));
}

TEST_CASE("Expanded glass seen from inside equals native", "[hydra][graphs]") {
    // Full transmission with a coat and sheen: from inside only the
    // interface remains (1/ior).
    Network net("ND_standard_surface_surfaceshader");
    HdMaterialNode2 &s = net.surface();
    setFloat(s, "transmission", 1.0f);
    setFloat(s, "specular_roughness", 0.3f);
    setFloat(s, "coat", 0.6f);
    setFloat(s, "sheen", 0.4f);
    for (const float mu : {0.3f, 0.85f})
        compareExpanded(net.src, "standard_surface", -direction(mu), "inside, mu " + std::to_string(mu));
}
