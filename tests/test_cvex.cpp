// The CVEX bridge (docs/design/M5.md M5.2): nova_cvex.dll loaded like hdNova
// loads it, VEX surface code compiled and run per batch, the conventions of
// D6 against closed forms (a VEX diffuse is Lambert: f cos = tint cos / pi,
// pdf = cos / pi, a sample's weight the tint), several threads at once, and
// builder-style code (tests/data/vex_builder_like.vfl) through the shims.
// Stock VEX Material Builder code is tested through hython (M5.4).
// Needs $HFS/bin on PATH (Houdini's DLLs).

#include "core/shading/external.h"
#include "houdini/cvex_bridge/transform.h"

#include "core/integrator/renderer.h"
#include "core/scene/scene.h"

#include "test_helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <windows.h>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

nova::ExternalBridge *bridge() {
    static nova::ExternalBridge *b = [] {
        HMODULE dll = LoadLibraryW(std::filesystem::path(NOVA_CVEX_DLL).wstring().c_str());
        if (!dll)
            return static_cast<nova::ExternalBridge *>(nullptr);
        auto factory = reinterpret_cast<nova::ExternalBridgeFactory>(GetProcAddress(dll, nova::kExternalBridgeEntry));
        return factory ? factory() : nullptr;
    }();
    return b;
}

nova::ExternalLane lane(nova::Vec3f wi, float sx, float sy) {
    nova::ExternalLane l;
    l.P = {0.0f, 0.0f, 0.0f};
    l.I = {0.0f, 0.0f, -1.0f}; // looking down at a surface facing +z
    l.N = l.Ng = {0.0f, 0.0f, 1.0f};
    l.dPds = {1.0f, 0.0f, 0.0f};
    l.dPdt = {0.0f, 1.0f, 0.0f};
    l.wi = wi;
    l.sx = sx;
    l.sy = sy;
    return l;
}

constexpr const char *kDiffuse = "surface\ntinted(vector tint = {0.2, 0.6, 0.2}; string label = \"diffuse\")\n{\n"
                                 "    F = 2.0 * diffuse(N, \"label\", label) * tint;\n}\n";

} // namespace

TEST_CASE("VEX surface code is rewritten for CVEX", "[cvex][transform]") {
    const std::vector<nova::cvex::ParamDecl> params =
        nova::cvex::parameters("vector a = {0, 0, 0}; export float b, c = 1; string label = \"x;y\"; float d[]");
    REQUIRE(params.size() == 5);
    CHECK(params[0].name == "a");
    CHECK(params[0].defaultText == "{0, 0, 0}");
    CHECK(params[1].name == "b");
    CHECK(params[1].exported);
    CHECK(params[2].name == "c");
    CHECK(params[3].name == "label");
    CHECK(nova::cvex::unquote(params[3].defaultText) == "x;y");
    CHECK(params[4].name == "d");

    const std::string code = "#include <voplib.h>\nimport pbrlighting;\nsurface\nshade(vector P = 0; float t = 1)\n{\n"
                             "    pbrlighting(\"direct\", d);\n    F = diffuse(N);\n}\n";
    const nova::cvex::Transformed t = nova::cvex::transformSurface(code, "layer.P = P;\nlayer.N = normalize(N);\n", {"Ng"});
    REQUIRE(t.ok);
    CHECK(t.function == "shade");
    CHECK(t.cvexFunction.rfind("shade_nova", 0) == 0); // unique per code
    CHECK(nova::cvex::transformSurface(code + "\n", "").cvexFunction != t.cvexFunction);
    CHECK(t.source.find("cvex\n" + t.cvexFunction + "(") != std::string::npos);
    CHECK(t.source.find("pbrlighting") == std::string::npos);  // statement and import gone
    CHECK(t.source.find("layer.P = 0;") != std::string::npos); // init_layer's reads replaced
    CHECK(t.source.find("#define Ng set(0, 0, 1)") < t.source.find("#include <voplib.h>"));
    CHECK(t.source.find("#undef Ng") < t.source.find("cvex\n" + t.cvexFunction + "("));
    // The shader's own P and t are not declared twice; the other globals are.
    const std::string signature = t.source.substr(t.source.find("cvex\n" + t.cvexFunction + "("));
    CHECK(signature.find("vector P = 0") == signature.rfind("vector P = 0")); // once: the shader's own
    CHECK(signature.find("float t = 0;") == std::string::npos);
    CHECK(signature.find("vector N = {0, 0, 1};") != std::string::npos);
    CHECK(signature.find("bsdf F;") != std::string::npos);
    CHECK_FALSE(nova::cvex::transformSurface("cvex nothing() {}", "", {}).ok);

    // Ray tracing in the body fails the shader (D5); names that only
    // contain such a call's name do not.
    const nova::cvex::Transformed traced = nova::cvex::transformSurface(
        "surface\nmirror()\n{\n    vector c = 0;\n    c = trace (P, reflect(I, N), Time);\n    F = diffuse(N) * c;\n}\n", "");
    CHECK_FALSE(traced.ok);
    CHECK(traced.error.find("trace()") != std::string::npos);
    CHECK(nova::cvex::transformSurface("surface\nok(float mytrace = 1)\n{\n    F = diffuse(N) * retrace(mytrace);\n}\n", "")
              .ok);
}

TEST_CASE("VEX displacement code is rewritten for CVEX and runs", "[cvex][transform]") {
    // docs/design/M5.md D8: P and N become exported parameters.
    const std::string code = "displace\nbumps(float amp = 0.1; vector dir = {0, 0, 0})\n{\n"
                             "    P += normalize(N) * amp * (1 + noise(P * 4));\n    N = computenormal(P);\n}\n";
    const nova::cvex::Transformed t = nova::cvex::transformDisplacement(code, "");
    REQUIRE(t.ok);
    CHECK(t.function == "bumps");
    const std::string signature = t.source.substr(t.source.find("cvex\n" + t.cvexFunction + "("));
    CHECK(signature.find("export vector P = 0; export vector N = {0, 0, 1};") != std::string::npos);
    CHECK(signature.find("float amp = 0.1; vector dir = {0, 0, 0})") != std::string::npos);
    CHECK(signature.find("nova_") == std::string::npos);
    CHECK(signature.find("bsdf F;") == std::string::npos);
    CHECK_FALSE(nova::cvex::transformDisplacement("surface s() { F = diffuse(N); }", "").ok);

    nova::ExternalBridge *b = bridge();
    REQUIRE(b);
    std::string error;
    const char *flat = "displace\nlift(float amp = 0.1)\n{\n    P += normalize(N) * amp;\n    N = computenormal(P);\n}\n";
    std::shared_ptr<nova::ExternalDisplacer> d = b->compileDisplacement(flat, &error);
    INFO(error);
    REQUIRE(d);
    CHECK(b->compileDisplacement(flat, &error) == d); // cached
    REQUIRE(d->params().size() == 1);
    CHECK(d->params()[0].name == "amp");
    nova::ExternalLane lanes[3];
    const nova::Vec3f normals[3] = {{0, 0, 1}, {1, 0, 0}, {0, 0.6f, 0.8f}};
    for (int k = 0; k < 3; ++k) {
        lanes[k].P = {float(k), 1.0f, 2.0f};
        lanes[k].N = lanes[k].Ng = normals[k];
        lanes[k].I = -normals[k];
    }
    const float amps[3] = {0.5f, 0.0f, 2.0f};
    const nova::ExternalInput varying{amps, true};
    nova::Vec3f P[3];
    REQUIRE(d->run(3, lanes, &varying, nullptr, P, &error));
    for (int k = 0; k < 3; ++k) {
        const nova::Vec3f expected = lanes[k].P + normals[k] * amps[k];
        CHECK_THAT(P[k].x, WithinAbs(expected.x, 1e-5));
        CHECK_THAT(P[k].y, WithinAbs(expected.y, 1e-5));
        CHECK_THAT(P[k].z, WithinAbs(expected.z, 1e-5));
    }
    // The noise variant compiles too (VEX library functions in CVEX).
    CHECK(b->compileDisplacement(code, &error));
    CHECK_FALSE(b->compileDisplacement("displace\nbroken()\n{\n    P += undefined_thing;\n}\n", &error));
    CHECK(error.find("undefined_thing") != std::string::npos);
}

TEST_CASE("The CVEX bridge loads and compiles", "[cvex]") {
    nova::ExternalBridge *b = bridge();
    REQUIRE(b);
    std::string error;
    std::shared_ptr<nova::ExternalShader> s = b->compileSurface(kDiffuse, &error);
    INFO(error);
    REQUIRE(s);
    REQUIRE(s->params().size() == 2);
    CHECK(s->params()[0].name == "tint");
    CHECK(s->params()[0].width == 3);
    CHECK_THAT(s->params()[0].defaults[1], WithinAbs(0.6, 1e-6));
    CHECK(s->params()[1].width == 0);
    CHECK(s->params()[1].stringDefault == "diffuse");
    CHECK(b->compileSurface(kDiffuse, &error) == s); // cached
    const nova::ExternalBounces bounces = b->bounces();
    CHECK(bounces.diffuse != 0);
    CHECK(bounces.reflect != 0);
    for (uint32_t m : {bounces.reflect, bounces.refract, bounces.volume, bounces.sss}) {
        CHECK(m != 0);
        CHECK((bounces.diffuse & m) == 0);
    }
    CHECK_FALSE(b->compileSurface("surface\nbroken()\n{\n    F = undefined_thing(N);\n}\n", &error));
    CHECK(error.find("VEX compile failed") != std::string::npos);
    CHECK(error.find("undefined_thing") != std::string::npos); // the compiler's own message
    // A local named like a global shadows it, as in VEX (builder code
    // declares a local Ce).
    CHECK(b->compileSurface("surface\nshadow()\n{\n    vector Ce = {0, 1, 0};\n    F = diffuse(N) * Ce;\n}\n", &error));
}

TEST_CASE("A VEX diffuse follows Nova's conventions", "[cvex]") {
    nova::ExternalBridge *b = bridge();
    REQUIRE(b);
    std::string error;
    std::shared_ptr<nova::ExternalShader> s = b->compileSurface(kDiffuse, &error);
    REQUIRE(s);
    const float cosines[] = {1.0f, 0.6f, 0.2f, -0.3f};
    nova::ExternalLane lanes[4];
    for (int k = 0; k < 4; ++k) {
        const float c = cosines[k], sn = std::sqrt(std::fmax(0.0f, 1.0f - c * c));
        lanes[k] = lane({sn, 0.0f, c}, 0.25f + 0.2f * float(k), 0.6f);
    }
    // tint from the material (uniform), the label's default (as bindValues
    // fills an unauthored string; an empty one would be passed as is).
    const float tint[3] = {0.5f, 0.25f, 0.1f};
    nova::ExternalInput inputs[2] = {{tint, false}, {}};
    std::string strings[2] = {std::string(), s->params()[1].stringDefault};
    nova::ExternalResult out[4];
    REQUIRE(s->run(4, lanes, inputs, strings, out, &error));
    constexpr float kInvPi = 0.31830988618f;
    for (int k = 0; k < 4; ++k) {
        const float c = std::fmax(cosines[k], 0.0f);
        INFO("cos " << cosines[k]);
        CHECK_THAT(out[k].lightF.x, WithinAbs(tint[0] * c * kInvPi, 1e-4));
        CHECK_THAT(out[k].lightF.y, WithinAbs(tint[1] * c * kInvPi, 1e-4));
        CHECK_THAT(out[k].lightPdf, WithinAbs(c * kInvPi, 1e-4));
        CHECK_THAT(out[k].albedo.x, WithinAbs(tint[0], 1e-4));
        // The sample: upper hemisphere, weight = the tint, MIS pdf cos / pi.
        REQUIRE((out[k].bounce & b->bounces().diffuse) != 0);
        CHECK(out[k].dir.z > 0.0f);
        CHECK_THAT(out[k].weight.x, WithinAbs(tint[0], 1e-4));
        CHECK_THAT(double(out[k].pdf), WithinRel(double(out[k].dir.z * kInvPi), 1e-3));
        CHECK_THAT(out[k].opacity.x, WithinAbs(1.0, 1e-6));
    }
    // Per-lane (varying) values.
    float tints[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0.5f, 0.5f, 0.5f};
    inputs[0] = {tints, true};
    REQUIRE(s->run(4, lanes, inputs, strings, out, &error));
    CHECK_THAT(out[1].albedo.y, WithinAbs(1.0, 1e-4));
    CHECK_THAT(out[2].albedo.z, WithinAbs(1.0, 1e-4));
    CHECK_THAT(out[3].albedo.x, WithinAbs(0.5, 1e-4));
}

TEST_CASE("CVEX runs on several threads", "[cvex]") {
    nova::ExternalBridge *b = bridge();
    REQUIRE(b);
    std::string error;
    std::shared_ptr<nova::ExternalShader> s = b->compileSurface(kDiffuse, &error);
    REQUIRE(s);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            std::vector<nova::ExternalLane> lanes(256);
            std::vector<nova::ExternalResult> out(256);
            const float tint[3] = {0.1f * float(t + 1), 0.5f, 0.5f};
            nova::ExternalInput inputs[2] = {{tint, false}, {}};
            std::string strings[2] = {std::string(), s->params()[1].stringDefault}, err;
            for (int round = 0; round < 200; ++round) {
                for (int l = 0; l < 256; ++l)
                    lanes[size_t(l)] = lane({0.0f, 0.0f, 1.0f}, float(l) / 256.0f, 0.5f);
                if (!s->run(256, lanes.data(), inputs, strings, out.data(), &err) ||
                    std::fabs(out[17].albedo.x - tint[0]) > 1e-4f)
                    ++failures;
            }
        });
    for (std::thread &th : threads)
        th.join();
    CHECK(failures == 0);
}

TEST_CASE("Builder-style VEX code runs through the shims", "[cvex]") {
    nova::ExternalBridge *b = bridge();
    REQUIRE(b);
    std::ifstream in(std::filesystem::path(NOVA_TEST_DIR) / "data" / "vex_builder_like.vfl");
    REQUIRE(in);
    std::stringstream code;
    code << in.rdbuf();
    std::string error;
    std::shared_ptr<nova::ExternalShader> s = b->compileSurface(code.str(), &error);
    INFO(error);
    REQUIRE(s);
    // Diffuse plus GGX specular, evaluated and sampled.
    nova::ExternalLane lanes[2] = {lane({0.0f, 0.6f, 0.8f}, 0.3f, 0.7f), lane({0.0f, 0.0f, 1.0f}, 0.8f, 0.2f)};
    lanes[0].P = lanes[1].P = {1.48f, 0.44f, 0.2f};
    std::vector<nova::ExternalInput> inputs(s->params().size());
    nova::ExternalResult out[2];
    REQUIRE(s->run(2, lanes, inputs.data(), nullptr, out, &error)); // the strings' defaults
    CHECK(out[0].lightF.x > 0.0f);
    CHECK(out[0].lightPdf > 0.0f);
    CHECK(nova::luminance(out[0].albedo) > 0.0f);
}

TEST_CASE("An empty VEX string parameter is passed as is", "[cvex]") {
    // An authored "" (a cleared map path) is a value, as in Karma; only null
    // strings stand for the shader's defaults.
    nova::ExternalBridge *b = bridge();
    REQUIRE(b);
    std::string error;
    std::shared_ptr<nova::ExternalShader> s = b->compileSurface(
        "surface\nlabeled(string tag = \"abcd\")\n{\n    Ce = set(strlen(tag) * 0.1, 0.0, 0.0);\n}\n", &error);
    INFO(error);
    REQUIRE(s);
    REQUIRE(s->params().size() == 1);
    REQUIRE(s->params()[0].width == 0);
    const nova::ExternalLane l = lane({0.0f, 0.0f, 1.0f}, 0.5f, 0.5f);
    nova::ExternalResult out;
    REQUIRE(s->run(1, &l, nullptr, nullptr, &out, &error));
    CHECK_THAT(out.emission.x, WithinAbs(0.4, 1e-5)); // "abcd"
    const std::string ab = "ab", empty;
    REQUIRE(s->run(1, &l, nullptr, &ab, &out, &error));
    CHECK_THAT(out.emission.x, WithinAbs(0.2, 1e-5));
    REQUIRE(s->run(1, &l, nullptr, &empty, &out, &error));
    CHECK_THAT(out.emission.x, WithinAbs(0.0, 1e-6));
}

TEST_CASE("A released VEX shader frees its contexts and compiles again", "[cvex]") {
    // The per-thread contexts go with the shader (an IPR edit of VEX code
    // releases one); the cache compiles the same code anew afterwards.
    nova::ExternalBridge *b = bridge();
    REQUIRE(b);
    const std::string code = std::string(kDiffuse) + "\n\n"; // its own cache entry
    std::weak_ptr<nova::ExternalShader> previous;
    for (int round = 0; round < 3; ++round) {
        std::string error;
        std::shared_ptr<nova::ExternalShader> s = b->compileSurface(code, &error);
        INFO(error);
        REQUIRE(s);
        CHECK(previous.expired());
        previous = s;
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&] {
                const nova::ExternalLane l = lane({0.0f, 0.0f, 1.0f}, 0.3f, 0.6f);
                const std::string strings[2] = {std::string(), s->params()[1].stringDefault};
                nova::ExternalResult r;
                std::string err;
                if (!s->run(1, &l, nullptr, strings, &r, &err) || std::fabs(r.albedo.y - 0.6f) > 1e-4f)
                    ++failures;
            });
        for (std::thread &th : threads)
            th.join();
        CHECK(failures == 0);
    }
}

// ---------------------------------------------------------------------------
// M5.3: external closures through the renderer (docs/design/M5.md D6, D7).
// ---------------------------------------------------------------------------

namespace {

// A material running `code` through the bridge with uniform parameters.
nova::MaterialDesc externalMaterial(const char *code, std::vector<std::vector<float>> values = {}) {
    nova::ExternalBridge *b = bridge();
    std::string error;
    auto binding = std::make_shared<nova::ExternalBinding>();
    binding->shader = b->compileSurface(code, &error);
    REQUIRE(binding->shader);
    binding->bounces = b->bounces();
    const size_t n = binding->shader->params().size();
    binding->slots.assign(n, nova::kNoExternalSlot);
    binding->strings.resize(n);
    binding->values.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const nova::ExternalParam &p = binding->shader->params()[i];
        binding->values[i] = i < values.size() && !values[i].empty() ? values[i] : p.defaults;
        binding->strings[i] = p.stringDefault;
    }
    nova::MaterialDesc m;
    m.inputs = nova::SurfaceInputs{};
    m.inputs.model = nova::SurfaceModel::External;
    m.inputs.size = nova::surface_field::kCount;
    m.inputs.set(nova::surface_field::opacity, nova::spectrum(1.0f));
    m.external = binding;
    return m;
}

struct Stats {
    double mean = 0.0, error = 0.0;
};

// An orthographic view of a unit sphere in a uniform white dome (not seen
// by camera rays): the mean green (or red) value inside 0.9 of the radius.
Stats renderSphere(const nova::MaterialDesc &material, uint32_t spp, nova::LightStrategy strategy, bool red = false) {
    nova::Scene scene;
    const nova::MeshId sphere = scene.createMesh();
    scene.updateMesh(sphere, nova::test::makeSphere(1.0f, 96, 48));
    const nova::MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(sphere, {m});
    nova::LightDesc dome;
    dome.type = nova::LightType::Dome;
    dome.intensity = 1.0f;
    scene.updateLight(scene.createLight(), dome);
    scene.commit();
    nova::Renderer renderer;
    nova::RenderSettings settings;
    settings.maxSamples = spp;
    settings.clampIndirect = 0.0f;
    settings.lightStrategy = strategy;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    const int size = 32;
    renderer.setCamera(nova::test::makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size));
    renderer.resize(size, size);
    nova::test::renderToConvergence(renderer);
    Stats s;
    double sq = 0.0;
    int count = 0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float px = (x + 0.5f) / size * 2.0f - 1.0f, py = (y + 0.5f) / size * 2.0f - 1.0f;
            if (px * px + py * py > 0.81f)
                continue;
            const nova::Vec3f c = nova::test::pixelMean(renderer.film(), x, y);
            const double v = red ? c.x : c.y;
            s.mean += v;
            sq += v * v;
            ++count;
        }
    s.mean /= count;
    s.error = std::sqrt(std::fmax(sq / count - s.mean * s.mean, 0.0) / count);
    return s;
}

constexpr const char *kPlastic =
    "#include \"bsdf.h\"\nsurface\nplastic(vector tint = {0.6, 0.4, 0.3}; float rough = 0.3)\n{\n"
    "    vector nn = frontface(normalize(N), I);\n"
    "    float energy = 0;\n"
    "    bsdf spec = get_bsdf(\"ggx\", nn, nn, normalize(I), {1, 0, 0}, {0, 1, 0}, {1, 1, 1}, {1, 1, 1}, 1.0 / 1.5, "
    "rough, 0, 0, 1, 0, 1, 1, 0, 0, energy, \"reflect\", \"refract\");\n"
    "    F = 2.0 * diffuse(nn, \"label\", \"diffuse\") * tint + spec;\n}\n";

constexpr const char *kMetal =
    "#include \"bsdf.h\"\nsurface\nmetal(vector color = {0.95, 0.64, 0.54}; float rough = 0.3)\n{\n"
    "    vector nn = frontface(normalize(N), I);\n"
    "    vector ng = frontface(normalize(Ng), I);\n"
    "    float energy = 0;\n"
    "    F = get_bsdf(\"ggx\", ng, nn, normalize(I), normalize(dPds), normalize(dPdt), color, {1, 1, 1}, 1.0 / 1.5, "
    "rough, 0, 0, 1, 0, 0, 1, 0, 0, energy, \"reflect\", \"refract\");\n}\n";

} // namespace

TEST_CASE("VEX opacity cuts out and Ce emits", "[cvex][render]") {
    // docs/design/M5.md D9: Of per hit when it depends on the hit, else once
    // per material; a surface that never sets Of stays opaque at no cost.
    REQUIRE(bridge());
    const nova::MaterialRecord opaque = nova::makeMaterialRecord(externalMaterial(kDiffuse));
    CHECK_FALSE(opaque.hasOpacity);
    CHECK_FALSE(opaque.externalOpacity);
    // A red emitter without a BSDF: the red channel is the emission of the
    // front and back faces the camera rays stop at.
    const char *constant = "surface\nhalfsee()\n{\n    Ce = {1, 0, 0};\n    Of = 0.25;\n}\n";
    const nova::MaterialDesc c = externalMaterial(constant);
    const nova::MaterialRecord rc = nova::makeMaterialRecord(c);
    CHECK_FALSE(rc.externalOpacity);
    CHECK_THAT(rc.uniformOpacity, WithinAbs(0.25, 1e-6));
    // The front face stops 0.25 of the rays, the back face 0.25 of the rest.
    const Stats sc = renderSphere(c, 64, nova::LightStrategy::MIS, true);
    INFO("constant: " << sc.mean << " +- " << sc.error);
    CHECK_THAT(sc.mean, WithinAbs(0.4375, 0.02));

    const char *cut = "surface\ncut()\n{\n    Ce = {1, 0, 0};\n    Of = P.x > 0 ? 0 : 1;\n}\n";
    const nova::MaterialDesc h = externalMaterial(cut);
    CHECK(nova::makeMaterialRecord(h).externalOpacity);
    const Stats sh = renderSphere(h, 16, nova::LightStrategy::MIS, true);
    INFO("cut: " << sh.mean << " +- " << sh.error);
    CHECK_THAT(sh.mean, WithinAbs(0.5, 0.02)); // the left half emits, the right half is cut away
}

TEST_CASE("A white VEX diffuse passes the furnace through the renderer", "[cvex][render]") {
    REQUIRE(bridge());
    const Stats s = renderSphere(externalMaterial(kDiffuse, {{1.0f, 1.0f, 1.0f}}), 256, nova::LightStrategy::MIS);
    INFO("mean " << s.mean << " +- " << s.error);
    CHECK_THAT(s.mean, WithinAbs(1.0, 0.01));
}

TEST_CASE("Light and BSDF sampling of a VEX surface agree", "[cvex][render][mis]") {
    REQUIRE(bridge());
    // The metal is pure GGX: VEX's ggx() runs Houdini's CVEX shaders by
    // operator path, which the bridge resolves itself outside Houdini.
    const char *code = GENERATE(kPlastic, kMetal);
    const nova::MaterialDesc m = externalMaterial(code);
    const Stats mis = renderSphere(m, 256, nova::LightStrategy::MIS);
    const Stats light = renderSphere(m, 256, nova::LightStrategy::LightOnly);
    const Stats bsdf = renderSphere(m, 256, nova::LightStrategy::BsdfOnly);
    INFO(code);
    INFO("MIS " << mis.mean << " +- " << mis.error << ", light " << light.mean << " +- " << light.error << ", BSDF "
                << bsdf.mean << " +- " << bsdf.error);
    CHECK(mis.mean > 0.2);
    CHECK(std::fabs(mis.mean - light.mean) < 4.0 * std::hypot(mis.error, light.error) + 0.005);
    CHECK(std::fabs(mis.mean - bsdf.mean) < 4.0 * std::hypot(mis.error, bsdf.error) + 0.005);
}

TEST_CASE("CVEX bridge cost per shading point", "[.bench]") {
    // docs/benchmarks.md (M5): one bridge run of 256 lanes, the time per
    // lane; NOVA_STOCK names a file with Houdini-generated builder code.
    nova::ExternalBridge *b = bridge();
    REQUIRE(b);
    std::vector<std::pair<std::string, std::string>> shaders = {
        {"VEX diffuse", kDiffuse}, {"VEX plastic (diffuse + ggx)", kPlastic}, {"VEX metal (ggx)", kMetal}};
    if (const char *stock = std::getenv("NOVA_STOCK")) {
        std::ifstream in(stock);
        std::stringstream text;
        text << in.rdbuf();
        shaders.emplace_back("stock builder (Principled Shader Core + noise)", text.str());
    }
    for (const auto &[name, code] : shaders) {
        std::string error;
        std::shared_ptr<nova::ExternalShader> s = b->compileSurface(code, &error);
        INFO(name << ": " << error);
        REQUIRE(s);
        constexpr int kCount = 256, kRuns = 400;
        std::vector<nova::ExternalLane> lanes(kCount);
        for (int l = 0; l < kCount; ++l) {
            const float a = 6.2831853f * float(l) / kCount;
            lanes[size_t(l)] = lane({0.6f * std::cos(a), 0.6f * std::sin(a), 0.8f}, float(l % 16) / 16.0f,
                                    float(l / 16) / 16.0f);
            lanes[size_t(l)].P = {std::cos(a), std::sin(a), 0.3f};
        }
        std::vector<nova::ExternalInput> inputs(s->params().size());
        std::vector<std::string> strings;
        for (const nova::ExternalParam &p : s->params())
            strings.push_back(p.stringDefault);
        std::vector<nova::ExternalResult> out(kCount);
        REQUIRE(s->run(kCount, lanes.data(), inputs.data(), strings.data(), out.data(), &error)); // warm up
        double best = 1e30;
        for (int round = 0; round < 3; ++round) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int r = 0; r < kRuns; ++r)
                s->run(kCount, lanes.data(), inputs.data(), strings.data(), out.data(), nullptr);
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
            best = std::min(best, ns / (double(kRuns) * kCount));
        }
        WARN(name << ": " << best << " ns per shading point");
    }
}
