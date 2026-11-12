// Hydra-level tests: drive the real render pass and delegate the way an
// interactive viewport does (Execute polled every few milliseconds) and check
// that progressive rendering converges after interruptions.

#include "hydra/renderBuffer.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderPass.h"

#include "hydra/denoise.h"
#include "hydra/oidnDenoiser.h"
#include "hydra/motion.h"

#include "core/denoise/denoiser.h"
#include "core/film/lpe_compile.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/tokens.h>

#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

struct TestCamera final : HdCamera {
    TestCamera() : HdCamera(SdfPath("/cam")) {
        _projection = Perspective;
        _horizontalAperture = 36.0f;
        _verticalAperture = 24.0f;
        _focalLength = 35.0f;
        _clippingRange = GfRange1f(0.1f, 1000.0f);
        SetZ(10.0);
    }
    void SetZ(double z) { _transform = GfMatrix4d(1.0).SetTranslate(GfVec3d(0.0, 0.0, z)); }
    void SetTransform(GfMatrix4d const &m) { _transform = m; }
    void SetFocalLength(float f) { _focalLength = f; }
    void SetAperture(float horizontal, float vertical) {
        _horizontalAperture = horizontal;
        _verticalAperture = vertical;
    }
    void SetClippingRange(float nearPlane, float farPlane) { _clippingRange = GfRange1f(nearPlane, farPlane); }
    void SetShutter(double open, double close) {
        _shutterOpen = open;
        _shutterClose = close;
    }
};

// A delegate with one color buffer and a render pass, without a render index.
struct Rig {
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    HdNovaRenderBuffer *buffer = nullptr;
    HdRenderPassSharedPtr pass;
    HdRenderPassStateSharedPtr state = std::make_shared<HdRenderPassState>();
    TestCamera camera;

    Rig(int width, int height, int samples) {
        delegate->SetRenderSetting(HdNovaSettingsTokens->samples, VtValue(samples));
        buffer = makeBuffer(width, height);
        pass = delegate->CreateRenderPass(nullptr, HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->hull)));
        state->SetCamera(&camera);
        state->SetViewport(GfVec4d(0, 0, width, height));
        bind(buffer);
    }
    ~Rig() {
        pass.reset();
        destroy(buffer);
        delegate.reset();
    }

    HdNovaRenderBuffer *makeBuffer(int width, int height) {
        auto *b = static_cast<HdNovaRenderBuffer *>(delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, SdfPath("/rb")));
        b->Allocate(GfVec3i(width, height, 1), HdFormatFloat32Vec4, false);
        return b;
    }
    void destroy(HdNovaRenderBuffer *b) {
        b->Finalize(delegate->GetRenderParam());
        delegate->DestroyBprim(b);
    }
    void bind(HdNovaRenderBuffer *b) {
        HdRenderPassAovBinding binding;
        binding.aovName = HdAovTokens->color;
        binding.renderBuffer = b;
        binding.clearValue = VtValue(GfVec4f(0.0f));
        state->SetAovBindings({binding});
    }
    void execute() { pass->Execute(state, TfTokenVector()); }
    int samples() const { return delegate->GetRenderStats()["samples"].Get<int>(); }

    // Polls Execute like a viewport until the pass converges. The poll
    // interval must be shorter than a whole render, otherwise a render that
    // restarts on every Execute could still converge between two polls.
    bool executeUntilConverged(double timeoutSeconds) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < timeoutSeconds) {
            execute();
            if (pass->IsConverged())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
};

} // namespace

TEST_CASE("Render pass converges", "[hydra]") {
    Rig rig(160, 120, 32);
    REQUIRE(rig.executeUntilConverged(10.0));
    CHECK(rig.samples() == 32);
}

// A restart triggered from Execute (camera, settings) must not leave a reset
// request behind, or every later Execute restarts again and the viewport
// never converges.
TEST_CASE("Interrupted renders still converge", "[hydra]") {
    // Large enough that one render (128 passes) takes far longer than a poll.
    Rig rig(640, 480, 128);
    rig.execute();

    SECTION("camera move") {
        rig.camera.SetZ(9.0);
        rig.execute();
    }
    SECTION("render setting change") {
        rig.delegate->SetRenderSetting(HdNovaSettingsTokens->maxDepth, VtValue(5));
        rig.execute();
    }
    SECTION("host restart") {
        rig.delegate->Restart();
        rig.execute();
    }
    REQUIRE(rig.executeUntilConverged(10.0));
    CHECK(rig.samples() == 128);
}

// husk passes clear values in the RenderVar's own data type, e.g. color4h or
// half; they must clear to that value without a warning.
TEST_CASE("Render buffers accept half-precision clear values", "[hydra]") {
    HdNovaRenderBuffer buffer(SdfPath("/rb"), nullptr);
    REQUIRE(buffer.Allocate(GfVec3i(4, 2, 1), HdFormatFloat16Vec4, false));
    buffer.Clear(VtValue(GfVec4h(0.25f, 0.5f, 0.75f, 1.0f)));
    const GfHalf *rgba = static_cast<const GfHalf *>(buffer.Map());
    for (int i = 0; i < 8; ++i) {
        CHECK(float(rgba[4 * i + 0]) == 0.25f);
        CHECK(float(rgba[4 * i + 1]) == 0.5f);
        CHECK(float(rgba[4 * i + 2]) == 0.75f);
        CHECK(float(rgba[4 * i + 3]) == 1.0f);
    }
    buffer.Unmap();

    REQUIRE(buffer.Allocate(GfVec3i(4, 2, 1), HdFormatFloat16, false));
    buffer.Clear(VtValue(GfHalf(-1.0f)));
    const GfHalf *scalar = static_cast<const GfHalf *>(buffer.Map());
    for (int i = 0; i < 8; ++i)
        CHECK(float(scalar[i]) == -1.0f);
    buffer.Unmap();
}

// The first-hit AOVs come from the first pass after a restart; later passes
// copy only the accumulated color.
TEST_CASE("First-hit AOVs are copied only with the first pass", "[hydra]") {
    nova::Film film;
    film.resize(4, 2);
    film.clear();
    const nova::TileRect all{0, 0, 4, 2};
    HdNovaRenderBuffer depth(SdfPath("/depth"), nullptr), color(SdfPath("/color"), nullptr);
    REQUIRE(depth.Allocate(GfVec3i(4, 2, 1), HdFormatFloat32, false));
    REQUIRE(color.Allocate(GfVec3i(4, 2, 1), HdFormatFloat32Vec4, false));

    HdNovaAovSource depthSource, colorSource;
    depthSource.kind = HdNovaAovKind::Depth;
    colorSource.kind = HdNovaAovKind::Beauty;
    film.samples = 1;
    std::fill(film.count.begin(), film.count.end(), 1u);
    std::fill(film.depth.begin(), film.depth.end(), 3.0f);
    std::fill(film.colorSum.begin(), film.colorSum.end(), nova::Spectrum{1.0f, 1.0f, 1.0f});
    depth.WriteFromFilm(film, all, depthSource, true);
    color.WriteFromFilm(film, all, colorSource, true);

    film.samples = 2;
    std::fill(film.count.begin(), film.count.end(), 2u);
    std::fill(film.depth.begin(), film.depth.end(), 5.0f); // cannot happen; must not be copied
    depth.WriteFromFilm(film, all, depthSource, false);
    color.WriteFromFilm(film, all, colorSource, false);
    const float *d = static_cast<const float *>(depth.Map());
    const float *c = static_cast<const float *>(color.Map());
    for (int i = 0; i < 8; ++i) {
        CHECK(d[i] == 3.0f);
        CHECK(c[4 * i] == 0.5f); // colorSum 1 over 2 samples
    }
    depth.Unmap();
    color.Unmap();
}

namespace {

// A binding as husk makes it from a RenderVar (docs/research/m6/karma-aovs.md 3.2).
HdRenderPassAovBinding karmaVar(char const *name, char const *type, char const *source, char const *fileFormat) {
    HdRenderPassAovBinding b;
    b.aovName = TfToken(name);
    b.aovSettings[TfToken("sourceType")] = VtValue(TfToken(type));
    b.aovSettings[TfToken("sourceName")] = VtValue(std::string(source));
    b.aovSettings[TfToken("driver:parameters:aov:husk:format")] = VtValue(TfToken(fileFormat));
    b.clearValue = VtValue(0.0f);
    return b;
}

} // namespace

// docs/design/M6.md D1, D9, D11: Karma's render vars resolve to Nova's AOV
// kinds, scalar clear values fill every channel, and int ids saved as float
// images are written as floats.
TEST_CASE("Karma's render vars resolve to Nova's AOVs", "[hydra][aov]") {
    using K = HdNovaAovKind;
    CHECK(HdNovaResolveAov(karmaVar("C", "lpe", "C.*[LO]", "color4h")).kind == K::Beauty);
    CHECK(HdNovaResolveAov(karmaVar("C", "lpe", " C.* ", "color4h")).kind == K::Beauty);
    const HdNovaAovSource diffuse = HdNovaResolveAov(karmaVar("directdiffuse", "lpe", "C<RD>L", "color3h"));
    CHECK(diffuse.kind == K::Lpe);
    CHECK(diffuse.expression == "C<RD>L");
    CHECK(HdNovaResolveAov(karmaVar("depth", "raw", "ray:hitPz", "float")).kind == K::Depth);
    CHECK(HdNovaResolveAov(karmaVar("N", "raw", "ray:hitN", "half3")).utility.kind == nova::UtilityKind::N);
    // Karma's ray:primid is the polygon within the gprim (M6.4); ray:objectid the gprim.
    const HdNovaAovSource face = HdNovaResolveAov(karmaVar("primid", "raw", "ray:primid", "float"));
    CHECK(face.kind == K::Utility);
    CHECK(face.utility.kind == nova::UtilityKind::Face);
    const HdNovaAovSource primid = HdNovaResolveAov(karmaVar("objectid", "raw", "ray:objectid", "float"));
    CHECK(primid.kind == K::PrimId);
    CHECK(primid.intAsFloat);
    CHECK(HdNovaResolveAov(karmaVar("P", "raw", "ray:hitP", "float3")).utility.kind == nova::UtilityKind::P);
    const HdNovaAovSource uv = HdNovaResolveAov(karmaVar("UV", "primvar", "st", "float3"));
    CHECK((uv.kind == K::Utility && uv.utility.kind == nova::UtilityKind::Primvar && uv.utility.name == "st"));
    const HdNovaAovSource aov = HdNovaResolveAov(karmaVar("myaov", "raw", "myaov", "float3"));
    CHECK((aov.kind == K::Utility && aov.utility.kind == nova::UtilityKind::ShaderAov && aov.utility.name == "myaov"));
    CHECK(HdNovaResolveAov(karmaVar("Facingratio_N", "raw", "ray:hitN_facingratio", "half")).kind == K::Utility);
    CHECK(HdNovaResolveAov(karmaVar("cputime", "intrinsic", "stat:cputime", "float")).kind == K::None);
    // Solaris' viewport binds Hydra's names without settings.
    HdRenderPassAovBinding viewport;
    viewport.aovName = HdAovTokens->primId;
    const HdNovaAovSource hydraId = HdNovaResolveAov(viewport);
    CHECK(hydraId.kind == K::PrimId);
    CHECK_FALSE(hydraId.intAsFloat);

    // A scalar clear value fills every channel (husk passes Karma's 0 as a float).
    HdNovaRenderBuffer vec(SdfPath("/vec"), nullptr);
    REQUIRE(vec.Allocate(GfVec3i(2, 2, 1), HdFormatFloat32Vec3, false));
    float *v = static_cast<float *>(vec.Map());
    std::fill(v, v + 12, 7.0f); // stale data from a previous frame
    vec.Unmap();
    vec.Clear(VtValue(0.5f));
    v = static_cast<float *>(vec.Map());
    for (int i = 0; i < 12; ++i)
        CHECK(v[i] == 0.5f);
    vec.Unmap();

    // Ids into a float image: float values, the background keeps its clear value.
    nova::Film film;
    film.resize(2, 1);
    film.clear();
    film.samples = 1;
    film.count = {1, 1};
    film.primId = {15, -1};
    HdNovaRenderBuffer ids(SdfPath("/ids"), nullptr);
    REQUIRE(ids.Allocate(GfVec3i(2, 1, 1), HdFormatInt32, false));
    ids.Clear(VtValue(0.0f));
    ids.WriteFromFilm(film, nova::TileRect{0, 0, 2, 1}, primid, true);
    const float *f = static_cast<const float *>(ids.Map());
    CHECK(f[0] == 15.0f);
    CHECK(f[1] == 0.0f);
    ids.Unmap();
    // Hydra's int ids: -1 on the background.
    ids.WriteFromFilm(film, nova::TileRect{0, 0, 2, 1}, hydraId, true);
    const int32_t *n = static_cast<const int32_t *>(ids.Map());
    CHECK(n[0] == 15);
    CHECK(n[1] == -1);
    ids.Unmap();
}

// docs/design/M6.md D3, D6: the LPE AOVs of a binding set compile into one
// program (deduplicated; the same expressions keep the same program, so a
// rebind does not restart); LPE buffers show the film's sums like the beauty,
// inverted for "!", and a failed expression keeps its clear value.
TEST_CASE("LPE AOVs render from one program", "[hydra][aov][lpe]") {
    HdNovaRenderDelegate delegate;
    HdRenderPassAovBindingVector bindings = {
        karmaVar("C", "lpe", "C.*[LO]", "color4h"),         karmaVar("diffuse", "lpe", "C<RD>.*L", "color3h"),
        karmaVar("diffuse2", "lpe", "C <RD> .*L", "color3h"), karmaVar("notdiffuse", "lpe", "!C<RD>.*L", "color3h"),
        karmaVar("broken", "lpe", "C<RD", "color3h"),
    };
    const std::shared_ptr<const nova::lpe::Compiled> program = delegate.GetLpeProgram(bindings);
    REQUIRE(program);
    CHECK(program->outputFlags.size() == 3); // C<RD>.*L, !C<RD>.*L, C<RD (the beauty is not an LPE output)
    CHECK(program->outputFlags[1] == nova::lpe::OutputInvert);
    CHECK_FALSE(program->errors[2].empty());
    CHECK(delegate.GetLpeProgram(bindings) == program);
    bindings.pop_back();
    CHECK(delegate.GetLpeProgram(bindings) != program);
    CHECK_FALSE(delegate.GetLpeProgram({karmaVar("C", "lpe", "C.*", "color4h")}));
    // Solaris asks for render vars by name and binds only those with a valid
    // descriptor (with a RenderSettings prim the viewport showed no beauty).
    for (char const *name : {"C", "beauty", "directdiffuse", "N", "CryptoPrimitives"})
        CHECK(delegate.GetDefaultAovDescriptor(TfToken(name)).format != HdFormatInvalid);
    CHECK(delegate.GetDefaultAovDescriptor(HdAovTokens->primId).format == HdFormatInt32);

    nova::Film film;
    film.setLpeOutputs(2);
    film.resize(2, 1);
    film.samples = 4;
    film.count = {4, 4};
    film.lpe(0)[0] = {2.0f, 1.0f, 0.0f};
    film.lpe(1)[0] = {1.0f, 1.0f, 1.0f};
    film.alphaSum[0] = 4.0f;
    HdNovaAovSource source = HdNovaResolveAov(bindings[1]);
    source.output = 0;
    HdNovaRenderBuffer rgba(SdfPath("/rgba"), nullptr);
    REQUIRE(rgba.Allocate(GfVec3i(2, 1, 1), HdFormatFloat32Vec4, false));
    rgba.Clear(VtValue(0.0f));
    rgba.WriteFromFilm(film, nova::TileRect{0, 0, 2, 1}, source, false); // every pass, like the beauty
    const float *v = static_cast<const float *>(rgba.Map());
    CHECK(v[0] == 0.5f);
    CHECK(v[1] == 0.25f);
    CHECK(v[2] == 0.0f);
    CHECK(v[3] == 1.0f);
    rgba.Unmap();
    source.output = 1;
    source.invert = true;
    HdNovaRenderBuffer rgb(SdfPath("/rgb"), nullptr);
    REQUIRE(rgb.Allocate(GfVec3i(2, 1, 1), HdFormatFloat32Vec3, false));
    rgb.WriteFromFilm(film, nova::TileRect{0, 0, 2, 1}, source, false);
    v = static_cast<const float *>(rgb.Map());
    CHECK(v[0] == 0.75f);
    CHECK(v[3] == 1.0f); // nothing there: !0 = 1
    rgb.Unmap();
    // shadow;: the occluded fraction 1 - lit / unoccluded, from two sums.
    nova::Film shadowFilm;
    shadowFilm.setLpeOutputs(1, 1);
    shadowFilm.resize(2, 1);
    shadowFilm.samples = 2;
    shadowFilm.count = {2, 2};
    shadowFilm.lpe(0)[0] = {4.0f, 2.0f, 0.0f};
    shadowFilm.lpeLitSum(0)[0] = {1.0f, 2.0f, 0.0f};
    HdNovaAovSource shadow = source;
    shadow.output = 0;
    shadow.invert = false;
    shadow.shadowSlot = 0;
    rgb.WriteFromFilm(shadowFilm, nova::TileRect{0, 0, 2, 1}, shadow, false);
    v = static_cast<const float *>(rgb.Map());
    CHECK(v[0] == 0.75f);
    CHECK(v[1] == 0.0f);
    CHECK(v[2] == 0.0f); // no light at all: no shadow
    rgb.Unmap();
    source.output = -1; // a failed expression
    rgb.Clear(VtValue(0.5f));
    rgb.WriteFromFilm(film, nova::TileRect{0, 0, 2, 1}, source, true);
    v = static_cast<const float *>(rgb.Map());
    CHECK(v[0] == 0.5f);
    rgb.Unmap();
}

// docs/design/M7.md D2: Karma's image filter (the Karma Render Settings
// LOP's Denoiser) and Nova's AOV list; buffers round-trip through the
// denoiser's top-down RGBA; a denoised buffer keeps its alpha.
TEST_CASE("Denoise requests and buffers", "[hydra][denoise]") {
    const HdNovaDenoiseRequest karma = HdNovaParseKarmaImageFilter(
        R"([["denoise", {"engine": "optix", "use_albedo_input": true, "use_n_input": false, "forcecpu": false, "aovs": ["C_denoise", "diffuse"]}]])");
    CHECK(karma.enabled);
    CHECK(karma.aovs == std::vector<std::string>{"C_denoise", "diffuse"});
    CHECK(karma.albedo);
    CHECK_FALSE(karma.normal);
    CHECK(karma.engine == "optix");
    CHECK_FALSE(HdNovaParseKarmaImageFilter("[]").enabled);
    CHECK_FALSE(HdNovaParseKarmaImageFilter(R"([["ocio", {}]])").enabled);
    CHECK(HdNovaParseKarmaImageFilter(R"([["denoise"]])").aovs == std::vector<std::string>{"C"});
    CHECK(HdNovaSplitAovNames(" C, directdiffuse  N ") == std::vector<std::string>{"C", "directdiffuse", "N"});

    HdNovaRenderBuffer rgb(SdfPath("/rgb"), nullptr);
    REQUIRE(rgb.Allocate(GfVec3i(2, 2, 1), HdFormatFloat16Vec3, false));
    const float top[16] = {1, 2, 3, 9, 4, 5, 6, 9, 7, 8, 9, 9, 10, 11, 12, 9}; // rows top-down
    rgb.WriteRgba(top);
    std::vector<float> back;
    REQUIRE(rgb.ReadRgba(back));
    CHECK(back[0] == 1.0f);
    CHECK(back[3] == 1.0f); // RGB buffers read alpha 1
    CHECK(back[8] == 7.0f);
    const auto *bottom = static_cast<const GfHalf *>(rgb.Map()); // the buffer's row 0 is the bottom
    CHECK(float(bottom[0]) == 7.0f);
    rgb.Unmap();
    HdNovaRenderBuffer ids(SdfPath("/ids"), nullptr);
    REQUIRE(ids.Allocate(GfVec3i(2, 2, 1), HdFormatInt32, false));
    CHECK_FALSE(ids.ReadRgba(back));

    // With a driver: the buffer is denoised, its alpha kept.
    if (!nova::sharedDenoiser().available())
        return;
    nova::Film film;
    film.resize(32, 32);
    film.samples = 4;
    std::fill(film.count.begin(), film.count.end(), 4u);
    HdNovaRenderBuffer color(SdfPath("/color"), nullptr);
    REQUIRE(color.Allocate(GfVec3i(32, 32, 1), HdFormatFloat32Vec4, false));
    float *v = static_cast<float *>(color.Map());
    uint32_t h = 1u;
    for (size_t i = 0; i < 32 * 32; ++i) {
        h = h * 1664525u + 1013904223u;
        const float noise = float(h >> 8) / float(1u << 24);
        v[i * 4 + 0] = v[i * 4 + 1] = v[i * 4 + 2] = 0.5f + (noise - 0.5f) * 0.6f;
        v[i * 4 + 3] = 0.25f;
        film.colorSum[i] = nova::Spectrum{v[i * 4], v[i * 4 + 1], v[i * 4 + 2]} * 4.0f;
        film.alphaSum[i] = 1.0f;
        film.albedo[i] = nova::Spectrum{0.5f, 0.5f, 0.5f};
        film.normal[i] = nova::Vec3f{0.0f, 0.0f, 1.0f};
    }
    double before = 0.0;
    for (size_t i = 0; i < 32 * 32; ++i)
        before += std::fabs(v[i * 4] - 0.5);
    color.Unmap();
    REQUIRE(HdNovaDenoiseBuffers(film, {&color}, true, true));
    v = static_cast<float *>(color.Map());
    double after = 0.0;
    bool alpha = true;
    for (size_t i = 0; i < 32 * 32; ++i) {
        after += std::fabs(v[i * 4] - 0.5);
        alpha = alpha && v[i * 4 + 3] == 0.25f;
    }
    color.Unmap();
    CHECK(after < before / 3.0);
    CHECK(alpha);
}

// Intel Open Image Denoise, Nova's second denoiser (the library Houdini
// ships): it runs without an NVIDIA driver, denoises every layer with the
// guides and keeps the alpha; nova:denoiser and Karma's Denoiser menu name it.
TEST_CASE("Open Image Denoise denoises buffers and is chosen by name", "[hydra][denoise][oidn]") {
    // The engine of a render: Nova's setting when it names one, else Karma's menu, else the automatic choice.
    HdNovaRenderDelegate delegate;
    CHECK(delegate.GetDenoiseConfig().engine.empty());
    delegate.SetRenderSetting(TfToken("karma:global:imagefilter"),
                              VtValue(std::string(R"([["denoise", {"engine": "oidn", "aovs": ["C"]}]])")));
    CHECK(delegate.GetDenoiseConfig().engine == "oidn");
    CHECK(delegate.GetDenoiseConfig().final.enabled);
    delegate.SetRenderSetting(TfToken("nova:denoiser"), VtValue(TfToken("optix")));
    CHECK(delegate.GetDenoiseConfig().engine == "optix");
    delegate.SetRenderSetting(TfToken("nova:denoiser"), VtValue(TfToken("auto")));
    CHECK(delegate.GetDenoiseConfig().engine == "oidn"); // "auto" names none: Karma's
    delegate.SetRenderSetting(TfToken("nova:denoiser"), VtValue(std::string(" OIDN ")));
    CHECK(delegate.GetDenoiseConfig().engine == "oidn");

    HdNovaOidnDenoiser &oidn = HdNovaOidnDenoiser::Shared();
    REQUIRE(oidn.Available()); // Houdini ships the library with a CPU device
    INFO("Open Image Denoise on " << oidn.DeviceName());
    CHECK((oidn.DeviceName() == "CPU" || oidn.DeviceName() == "CUDA" || oidn.DeviceName() == "GPU"));

    // A flat grey image with strong noise and flat guides, in two layers of different brightness.
    const int n = 64;
    const size_t pixels = size_t(n) * size_t(n);
    std::vector<float> a(pixels * 4), b(pixels * 4), albedo(pixels * 3, 0.5f), normal(pixels * 3, 0.0f);
    uint32_t h = 7u;
    for (size_t i = 0; i < pixels; ++i) {
        normal[i * 3 + 2] = 1.0f;
        for (int c = 0; c < 3; ++c) {
            h = h * 1664525u + 1013904223u;
            const float noise = float(h >> 8) / float(1u << 24) - 0.5f;
            a[i * 4 + size_t(c)] = 0.5f + 0.6f * noise;
            b[i * 4 + size_t(c)] = 4.0f + 3.0f * noise;
        }
        a[i * 4 + 3] = 0.25f;
        b[i * 4 + 3] = float(i % 7) / 7.0f;
    }
    const std::vector<float> a0 = a, b0 = b;
    const auto deviation = [&](std::vector<float> const &v, float mean) {
        double sum = 0.0;
        for (size_t i = 0; i < pixels; ++i)
            sum += std::fabs(v[i * 4] - mean);
        return sum / double(pixels);
    };
    const nova::Denoiser::Layer layers[2] = {{a.data(), a.data()}, {b.data(), b.data()}}; // in place
    REQUIRE(oidn.Denoise(n, n, layers, 2, albedo.data(), normal.data(), 2));
    CHECK(deviation(a, 0.5f) < deviation(a0, 0.5f) / 3.0);
    CHECK(deviation(b, 4.0f) < deviation(b0, 4.0f) / 3.0);
    bool alpha = true;
    for (size_t i = 0; i < pixels; ++i)
        alpha = alpha && a[i * 4 + 3] == 0.25f && b[i * 4 + 3] == b0[i * 4 + 3];
    CHECK(alpha);
    // Without guides, with the albedo alone, and at the viewport's quality.
    std::vector<float> c = a0;
    const nova::Denoiser::Layer one{c.data(), c.data()};
    REQUIRE(oidn.Denoise(n, n, &one, 1, nullptr, nullptr, 1));
    CHECK(deviation(c, 0.5f) < deviation(a0, 0.5f) / 2.0);
    c = a0;
    REQUIRE(oidn.Denoise(n, n, &one, 1, albedo.data(), nullptr, 0)); // the fast filter
    CHECK(deviation(c, 0.5f) < deviation(a0, 0.5f) / 3.0);
    c = a0;
    REQUIRE(oidn.Denoise(n, n, &one, 1, nullptr, normal.data(), 2)); // a normal without an albedo is left out
    CHECK(deviation(c, 0.5f) < deviation(a0, 0.5f) / 2.0);

    // Through the delegate's buffers, by name.
    nova::Film film;
    film.resize(32, 32);
    film.samples = 4;
    std::fill(film.count.begin(), film.count.end(), 4u);
    HdNovaRenderBuffer color(SdfPath("/color"), nullptr);
    REQUIRE(color.Allocate(GfVec3i(32, 32, 1), HdFormatFloat32Vec4, false));
    float *v = static_cast<float *>(color.Map());
    double before = 0.0;
    for (size_t i = 0; i < 32 * 32; ++i) {
        h = h * 1664525u + 1013904223u;
        const float noise = float(h >> 8) / float(1u << 24);
        v[i * 4 + 0] = v[i * 4 + 1] = v[i * 4 + 2] = 0.5f + (noise - 0.5f) * 0.6f;
        v[i * 4 + 3] = 0.25f;
        film.colorSum[i] = nova::Spectrum{v[i * 4], v[i * 4 + 1], v[i * 4 + 2]} * 4.0f;
        film.alphaSum[i] = 1.0f;
        film.albedo[i] = nova::Spectrum{0.5f, 0.5f, 0.5f};
        film.normal[i] = nova::Vec3f{0.0f, 0.0f, 1.0f};
        before += std::fabs(v[i * 4] - 0.5);
    }
    color.Unmap();
    REQUIRE(HdNovaDenoiseBuffers(film, {&color}, true, true, "oidn", 2));
    v = static_cast<float *>(color.Map());
    double after = 0.0;
    alpha = true;
    for (size_t i = 0; i < 32 * 32; ++i) {
        after += std::fabs(v[i * 4] - 0.5);
        alpha = alpha && v[i * 4 + 3] == 0.25f;
    }
    color.Unmap();
    CHECK(after < before / 3.0);
    CHECK(alpha);
}

// While stopped by the host, buffers may be destroyed and replaced; the pass
// must forget the old ones and render into the new ones after Restart().
TEST_CASE("Stopped render pass follows buffer replacement", "[hydra]") {
    Rig rig(160, 120, 1 << 20);
    rig.execute();
    rig.delegate->Stop();

    HdNovaRenderBuffer *replacement = rig.makeBuffer(80, 60);
    rig.bind(replacement);
    rig.destroy(rig.buffer);
    rig.buffer = replacement;

    rig.execute();
    CHECK(rig.pass->IsConverged()); // a stopped render does not change any more

    rig.delegate->SetRenderSetting(HdNovaSettingsTokens->samples, VtValue(8));
    rig.delegate->Restart();
    REQUIRE(rig.executeUntilConverged(10.0));
    CHECK(replacement->IsConverged());
    CHECK(rig.samples() == 8);
}

// Karma's ids into a float image (husk:format float): the int buffer turns
// float32 before the pass clears it, so the background keeps the clear value
// as a float (cleared as an int32, -1 read back as a NaN).
TEST_CASE("Float id images keep their clear value on the background", "[hydra][aov]") {
    Rig rig(16, 8, 2);
    auto *ids = static_cast<HdNovaRenderBuffer *>(rig.delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, SdfPath("/ids")));
    REQUIRE(ids->Allocate(GfVec3i(16, 8, 1), HdFormatInt32, false));
    HdRenderPassAovBinding color;
    color.aovName = HdAovTokens->color;
    color.renderBuffer = rig.buffer;
    color.clearValue = VtValue(GfVec4f(0.0f));
    HdRenderPassAovBinding id = karmaVar("objectid", "raw", "ray:objectid", "float");
    id.renderBuffer = ids;
    id.clearValue = VtValue(-1.0f);
    rig.state->SetAovBindings({color, id});
    REQUIRE(rig.executeUntilConverged(10.0));
    CHECK(ids->GetFormat() == HdFormatFloat32);
    const float *f = static_cast<const float *>(ids->Map());
    int background = 0; // the scene is empty
    for (int i = 0; i < 16 * 8; ++i)
        background += f[i] == -1.0f ? 1 : 0;
    ids->Unmap();
    CHECK(background == 16 * 8);
    rig.pass.reset(); // stops the render thread before its buffer goes
    rig.destroy(ids);
}

// nova:abortOnMissingTexture: the aborted render reports convergence, so
// hosts that wait for it (husk, nova_cli) finish instead of waiting forever.
TEST_CASE("A render aborted for a missing texture finishes", "[hydra]") {
    Rig rig(32, 24, 1 << 20);
    rig.delegate->SetRenderSetting(HdNovaSettingsTokens->abortOnMissingTexture, VtValue(true));
    HdNovaTextureSystem &textures = rig.delegate->GetNovaRenderParam().GetSurfaceTextures();
    HdNovaTextureRequest missing;
    missing.path = "nova_test_missing_texture_that_does_not_exist.exr";
    textures.Acquire(missing);
    REQUIRE(textures.MissingCount() == 1);
    REQUIRE(rig.executeUntilConverged(5.0));
    CHECK(rig.delegate->GetRenderStats()["rendererStage"].Get<std::string>() == "Aborted");
}

// A render paused before its first proxy pass (which is never interrupted)
// must still honour a stop request: StopRender() blocks until it does.
TEST_CASE("A paused render stops before its first proxy pass", "[hydra]") {
    Rig rig(64, 48, 1 << 20);
    rig.delegate->SetRenderSetting(HdNovaSettingsTokens->houdiniViewport, VtValue(true)); // proxy passes
    rig.delegate->Pause();
    rig.execute(); // the render thread starts paused
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // in its pause loop by now
    auto stop = std::async(std::launch::async, [&rig] { rig.delegate->GetRenderThread().StopRender(); });
    const bool stopped = stop.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    rig.delegate->Resume(); // lets a render that ignored the request end
    stop.wait();
    CHECK(stopped);
}

// A camera that gives no rays (a NaN parameter, a zero aperture or scale,
// equal clipping planes) ends the render with its buffers cleared.
// A NaN in the view never equals the view of the last Execute, so the pass
// used to restart the render on every Execute: husk and nova_cli waited
// forever, a viewport never came to rest.
TEST_CASE("A camera that gives no rays converges with cleared buffers", "[hydra][robust]") {
    Rig rig(64, 48, 4);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const GfVec4f clear(0.25f, 0.5f, 0.75f, 1.0f);
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = rig.buffer;
    binding.clearValue = VtValue(clear);
    rig.state->SetAovBindings({binding});

    SECTION("NaN focal length") { rig.camera.SetFocalLength(nan); }
    SECTION("zero aperture") { rig.camera.SetAperture(0.0f, 0.0f); }
    SECTION("infinite aperture") { rig.camera.SetAperture(inf, inf); } // one alone: the window policy conforms it
    SECTION("equal clipping planes") { rig.camera.SetClippingRange(0.0f, 0.0f); }
    SECTION("NaN clipping planes") { rig.camera.SetClippingRange(nan, 1000.0f); }
    SECTION("NaN transform") { rig.camera.SetTransform(GfMatrix4d(1.0).SetTranslate(GfVec3d(nan, 0.0, 10.0))); }
    SECTION("zero scale") { rig.camera.SetTransform(GfMatrix4d(1.0).SetScale(0.0)); }
    REQUIRE(rig.executeUntilConverged(5.0));
    CHECK(rig.buffer->IsConverged());
    const auto cleared = [&] {
        const GfVec4f *rgba = static_cast<const GfVec4f *>(rig.buffer->Map());
        const int n = int(std::count(rgba, rgba + 64 * 48, clear));
        rig.buffer->Unmap();
        return n;
    };
    CHECK(cleared() == 64 * 48); // nothing was rendered

    // A camera with rays again: the render runs.
    rig.camera.SetFocalLength(35.0f);
    rig.camera.SetAperture(36.0f, 24.0f);
    rig.camera.SetClippingRange(0.1f, 1000.0f);
    rig.camera.SetZ(10.0);
    rig.execute();
    CHECK_FALSE(rig.pass->IsConverged());
    REQUIRE(rig.executeUntilConverged(10.0));
    CHECK(rig.samples() == 4);
    CHECK(cleared() == 0); // the empty scene: transparent black
}

// A shutter that is not finite has no interval to sample motion over: the
// frame renders as with motion blur off. A NaN shutter never equalled the
// shutter the scene was sampled with, so every Execute resampled the scene
// and the render never started.
TEST_CASE("A shutter that is not finite renders the frame", "[hydra][robust]") {
    Rig rig(64, 48, 4);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    SECTION("NaN") { rig.camera.SetShutter(nan, nan); }
    SECTION("NaN close") { rig.camera.SetShutter(-0.25, nan); }
    SECTION("infinite") { rig.camera.SetShutter(-inf, inf); }
    SECTION("beyond float") { rig.camera.SetShutter(-1e300, 1e300); }
    REQUIRE(rig.executeUntilConverged(5.0));
    CHECK(rig.samples() == 4);
    CHECK(HdNovaResolveShutter(nan, 0.5, false, false) == HdNovaShutter{});
    CHECK(HdNovaResolveShutter(-0.25, inf, false, false) == HdNovaShutter{});
    CHECK(HdNovaResolveShutter(-0.25, 0.25, false, false).IsBlurred()); // a finite shutter is kept
}

// HdRenderPassAovBinding's equality calls a clear value or an AOV setting
// that holds a NaN different from itself, so bindings with one looked new on
// every Execute and restarted the render each time.
TEST_CASE("NaN clear values and AOV settings do not restart the render forever", "[hydra][robust]") {
    Rig rig(64, 48, 4);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = rig.buffer;
    binding.clearValue = VtValue(GfVec4f(0.0f));
    SECTION("clear value") { binding.clearValue = VtValue(GfVec4f(nan, 0.0f, 0.0f, 0.0f)); }
    SECTION("AOV setting") {
        binding.aovSettings[TfToken("driver:parameters:aov:clearValue")] = VtValue(GfVec4f(nan, 0.0f, 0.0f, 0.0f));
    }
    SECTION("float AOV setting") { binding.aovSettings[TfToken("driver:parameters:aov:weight")] = VtValue(nan); }
    rig.state->SetAovBindings({binding});
    REQUIRE(rig.executeUntilConverged(5.0));
    CHECK(rig.samples() == 4);
}

// A pixel aspect ratio that is no ratio (zero, negative, NaN) counts as
// square pixels instead of a camera window without rays.
TEST_CASE("A pixel aspect ratio that is not positive counts as square pixels", "[hydra][robust]") {
    Rig rig(64, 48, 4);
    const float par = GENERATE(0.0f, -2.0f, std::numeric_limits<float>::quiet_NaN());
    rig.delegate->SetRenderSetting(TfToken("resolution"), VtValue(GfVec2i(64, 48)));
    rig.delegate->SetRenderSetting(TfToken("pixelAspectRatio"), VtValue(par));
    REQUIRE(rig.executeUntilConverged(5.0));
    CHECK(rig.samples() == 4); // rendered, not the cleared buffers of a camera without rays
}

// A finite value beyond half's range stays finite in a half buffer (the
// largest half): converted plainly it became infinite, so a very bright
// emitter, or a bright light seen in a mirror, left inf pixels in half
// images. Values that are not finite pass as they are.
TEST_CASE("Half render buffers keep finite values finite", "[hydra][robust]") {
    HdNovaRenderBuffer buffer(SdfPath("/rb"), nullptr);
    REQUIRE(buffer.Allocate(GfVec3i(2, 1, 1), HdFormatFloat16Vec4, false));
    const float inf = std::numeric_limits<float>::infinity();
    const float rgba[8] = {1e30f, -1e30f, 65504.0f, 70000.0f, inf, -inf, 0.25f, -65505.0f};
    buffer.WriteRgba(rgba);
    const GfHalf *h = static_cast<const GfHalf *>(buffer.Map());
    CHECK(float(h[0]) == 65504.0f);
    CHECK(float(h[1]) == -65504.0f);
    CHECK(float(h[2]) == 65504.0f);
    CHECK(float(h[3]) == 65504.0f);
    CHECK(float(h[4]) == inf);
    CHECK(float(h[5]) == -inf);
    CHECK(float(h[6]) == 0.25f);
    CHECK(float(h[7]) == -65504.0f);
    buffer.Unmap();
}

// An image no memory holds (a resolution or data window off by orders of
// magnitude) fails to allocate instead of throwing through the host or
// wrapping to a smaller buffer.
TEST_CASE("A render buffer no memory holds fails to allocate", "[hydra][robust]") {
    HdNovaRenderBuffer buffer(SdfPath("/rb"), nullptr);
    const int huge = std::numeric_limits<int>::max();
    CHECK_FALSE(buffer.Allocate(GfVec3i(huge, huge, 1), HdFormatFloat32Vec4, false));
    CHECK(buffer.GetWidth() == 0);
    CHECK(buffer.GetHeight() == 0);
    CHECK_FALSE(buffer.Allocate(GfVec3i(1 << 30, 1 << 30, 1), HdFormatFloat32Vec4, false)); // 2^64 bytes: wraps to 0
    CHECK(buffer.GetWidth() == 0);
    REQUIRE(buffer.Allocate(GfVec3i(4, 2, 1), HdFormatFloat32Vec4, false));
    CHECK(buffer.GetWidth() == 4);
}
