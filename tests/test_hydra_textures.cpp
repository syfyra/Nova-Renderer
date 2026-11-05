// Surface textures through OIIO (docs/design/M3.md M3.8; expected values
// measured in docs/research/m3/textures.md 1.7, 1.8, 4, 8): address modes,
// UDIM like Karma, sRGB decoding before filtering (also of 8-bit .tx files),
// a linear gamut as a matrix after the lookup, grey to RGB, premultiplied
// PNG alpha, missing files, .rat through imaketx, the conversion cache,
// textures changed on disk, files in .usdz packages and the translation of
// image nodes.

#include "test_helpers.h"

#include "hydra/material/translator.h"
#include "hydra/textureCache.h"
#include "hydra/textureSystem.h"

#include "core/shading/compiler.h"
#include "core/texture/texel_stream.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/imageio.h>

#include <pxr/usd/sdf/zipFile.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <process.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

// Houdini's OCIO config, as husk sets it (before any texture is read).
const bool kOcioSet = [] {
    if (!std::getenv("OCIO"))
        _putenv_s("OCIO", NOVA_HFS "/packages/ocio/houdini-config-v3.0.0_aces-v2.0_ocio-v2.5.ocio");
    return true;
}();

std::string testDir() {
    static const std::string dir = [] {
        // Per process: ctest runs the test cases in parallel processes.
        const fs::path d = fs::temp_directory_path() / ("nova_test_textures_" + std::to_string(_getpid()));
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d / "cache");
        return d.generic_string();
    }();
    return dir;
}

HdNovaTextureOptions testOptions() {
    HdNovaTextureOptions o;
    o.cacheDir = testDir() + "/cache";
    return o;
}

// A constant image of `channels` floats per texel (values written as
// `type`: UINT8 stores codes v * 255).
std::string writeImage(std::string const &name, int w, int h, std::vector<float> const &texel,
                       OIIO::TypeDesc type, bool tiledMipped = false, bool unassociated = false) {
    const std::string path = testDir() + "/" + name;
    const int nc = int(texel.size());
    OIIO::ImageBuf buf(OIIO::ImageSpec(w, h, nc, OIIO::TypeDesc::FLOAT));
    OIIO::ImageBufAlgo::fill(buf, texel);
    if (tiledMipped) {
        OIIO::ImageSpec config;
        config.tile_width = config.tile_height = 16;
        config.set_format(type);
        REQUIRE(OIIO::ImageBufAlgo::make_texture(OIIO::ImageBufAlgo::MakeTxTexture, buf, path, config));
        return path;
    }
    OIIO::ImageSpec spec(w, h, nc, type);
    if (unassociated)
        spec.attribute("oiio:UnassociatedAlpha", 1);
    auto out = OIIO::ImageOutput::create(path);
    REQUIRE(out);
    REQUIRE(out->open(path, spec));
    std::vector<float> pixels;
    for (int i = 0; i < w * h; ++i)
        pixels.insert(pixels.end(), texel.begin(), texel.end());
    REQUIRE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()));
    out->close();
    return path;
}

struct Lookup {
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8_t status = 0;
};

Lookup sample(HdNovaTextureSystem const &ts, nova::TextureId id, nova::TexSlot slot, float s, float t) {
    nova::TexSampler smp;
    smp.texture = id;
    smp.slot = slot;
    nova::TexQuery q;
    q.count = 1;
    q.s = &s;
    q.t = &t;
    Lookup r;
    ts.sample2D(smp, q, r.v, &r.status);
    return r;
}

nova::TexSlot rgbSlot(nova::TexFilter filter = nova::TexFilter::Linear) {
    nova::TexSlot s;
    s.wrapS = s.wrapT = nova::TexWrap::Clamp;
    s.filter = filter;
    s.channels = 3;
    return s;
}

} // namespace

TEST_CASE("OIIO address modes on a 4x1 ramp", "[hydra][textures]") {
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    // An untiled float EXR: converted (tiled, MIP-mapped), values unchanged.
    const std::string path = testDir() + "/ramp.exr";
    {
        OIIO::ImageSpec spec(4, 1, 1, OIIO::TypeDesc::FLOAT);
        auto out = OIIO::ImageOutput::create(path);
        REQUIRE(out->open(path, spec));
        const float texels[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        REQUIRE(out->write_image(OIIO::TypeDesc::FLOAT, texels));
        out->close();
    }
    const nova::TextureId id = ts.Acquire({path, "raw", ""});
    REQUIRE(id != nova::kNoTexture);
    struct Row {
        nova::TexWrap wrap;
        float expect[3];
    };
    const Row rows[] = {{nova::TexWrap::Clamp, {1, 4, 4}},    {nova::TexWrap::Periodic, {4, 1, 3}},
                        {nova::TexWrap::Mirror, {1, 4, 2}},   {nova::TexWrap::Black, {0, 0, 0}},
                        {nova::TexWrap::Constant, {-1, -1, -1}}};
    const float s[3] = {-0.1f, 1.1f, 1.6f};
    for (Row const &r : rows) {
        nova::TexSlot slot;
        slot.wrapS = r.wrap;
        slot.wrapT = nova::TexWrap::Clamp;
        slot.filter = nova::TexFilter::Closest;
        slot.channels = 1;
        for (int i = 0; i < 3; ++i) {
            INFO("wrap " << int(r.wrap) << " s " << s[i]);
            const Lookup l = sample(ts, id, slot, s[i], 0.5f);
            CHECK((l.status ? -1.0f : l.v[0]) == r.expect[i]);
        }
    }
}

TEST_CASE("UDIM tiles as in Karma", "[hydra][textures]") {
    // 1001 red, 1002 green, 1011 white above v = 1.5 and blue below, 1012 missing.
    writeImage("udim.1001.exr", 8, 8, {1, 0, 0}, OIIO::TypeDesc::HALF);
    writeImage("udim.1002.exr", 8, 8, {0, 1, 0}, OIIO::TypeDesc::HALF);
    {
        const std::string path = testDir() + "/udim.1011.exr";
        OIIO::ImageBuf buf(OIIO::ImageSpec(8, 8, 3, OIIO::TypeDesc::FLOAT));
        const float white[3] = {1, 1, 1}, blue[3] = {0, 0, 1};
        OIIO::ImageBufAlgo::fill(buf, white, OIIO::ROI(0, 8, 0, 4, 0, 1, 0, 3)); // file rows from the top
        OIIO::ImageBufAlgo::fill(buf, blue, OIIO::ROI(0, 8, 4, 8, 0, 1, 0, 3));
        REQUIRE(buf.write(path, OIIO::TypeDesc::HALF));
    }
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    const nova::TextureId id = ts.Acquire({testDir() + "/udim.<UDIM>.exr", "", ""});
    REQUIRE(id != nova::kNoTexture);
    CHECK(ts.MissingCount() == 0);
    const nova::TexSlot slot = rgbSlot(nova::TexFilter::Closest);
    auto rgb = [&](float u, float v) { return sample(ts, id, slot, u, v); };
    Lookup l = rgb(0.5f, 0.5f);
    CHECK((l.status == 0 && l.v[0] == 1.0f && l.v[1] == 0.0f));
    l = rgb(1.5f, 0.5f);
    CHECK((l.status == 0 && l.v[1] == 1.0f && l.v[0] == 0.0f));
    l = rgb(0.5f, 1.75f);
    CHECK((l.status == 0 && l.v[0] == 1.0f && l.v[2] == 1.0f));
    l = rgb(0.5f, 1.25f);
    CHECK((l.status == 0 && l.v[0] == 0.0f && l.v[2] == 1.0f));
    CHECK(rgb(1.5f, 1.5f).status == 1);  // missing tile
    CHECK(rgb(-0.5f, 0.5f).status == 1); // u < 0 (OIIO alone would read 1001)
    CHECK(rgb(0.5f, -0.5f).status == 1);
    CHECK(rgb(10.5f, 0.5f).status == 1);
}

TEST_CASE("sRGB textures are decoded before filtering", "[hydra][textures]") {
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    // Constant 8-bit codes 10 and 128: PNG (an OCIO file rule) and a tiled,
    // MIP-mapped uint8 .tx without metadata (Karma's 8-bit rule).
    const std::string c10 = writeImage("c10.png", 8, 8, {10 / 255.0f, 10 / 255.0f, 10 / 255.0f}, OIIO::TypeDesc::UINT8);
    const std::string c128 =
        writeImage("c128.png", 8, 8, {128 / 255.0f, 128 / 255.0f, 128 / 255.0f}, OIIO::TypeDesc::UINT8);
    const std::string c10tx =
        writeImage("c10_u8.tx", 64, 64, {10 / 255.0f, 10 / 255.0f, 10 / 255.0f}, OIIO::TypeDesc::UINT8, true);
    const std::string c128tx =
        writeImage("c128_u8.tx", 64, 64, {128 / 255.0f, 128 / 255.0f, 128 / 255.0f}, OIIO::TypeDesc::UINT8, true);
    const nova::TexSlot slot = rgbSlot();
    for (std::string const &p : {c10, c10tx})
        CHECK_THAT(sample(ts, ts.Acquire({p, "", ""}), slot, 0.5f, 0.5f).v[1], WithinAbs(0.003035, 1e-5));
    for (std::string const &p : {c128, c128tx})
        CHECK_THAT(sample(ts, ts.Acquire({p, "", ""}), slot, 0.5f, 0.5f).v[1], WithinAbs(0.215861, 1e-4));
    // An explicit raw color space keeps the codes.
    CHECK_THAT(sample(ts, ts.Acquire({c128, "raw", ""}), slot, 0.5f, 0.5f).v[0], WithinAbs(128 / 255.0, 1e-3));

    // A 50 % black/white sRGB checker averaged over the whole texture: 0.5
    // (filtered in linear), not srgbToLinear(0.5) = 0.214.
    const std::string checker = testDir() + "/checker.png";
    {
        OIIO::ImageBuf buf(OIIO::ImageSpec(64, 64, 3, OIIO::TypeDesc::FLOAT));
        const float white[3] = {1, 1, 1}, black[3] = {0, 0, 0};
        OIIO::ImageBufAlgo::checker(buf, 1, 1, 1, white, black);
        REQUIRE(buf.write(checker, OIIO::TypeDesc::UINT8));
    }
    const nova::TextureId cid = ts.Acquire({checker, "", ""});
    nova::TexSampler smp;
    smp.texture = cid;
    smp.slot = rgbSlot();
    float s = 0.5f, t = 0.5f, d = 1.0f, zero = 0.0f, out[3];
    uint8_t status;
    nova::TexQuery q;
    q.count = 1;
    q.s = &s;
    q.t = &t;
    q.dsdx = &d;
    q.dtdx = &zero;
    q.dsdy = &zero;
    q.dtdy = &d;
    ts.sample2D(smp, q, out, &status);
    CHECK(status == 0);
    CHECK_THAT(out[0], WithinAbs(0.5, 0.01));
}

TEST_CASE("Linear gamuts, grey files and PNG alpha", "[hydra][textures]") {
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    const nova::TexSlot slot = rgbSlot();
    // ACEScg (file rule "*ACEScg*"): tiled and MIP-mapped, so read directly
    // with the matrix after the lookup; untiled, converted with it baked in.
    const float c[3] = {0.2f, 0.5f, 0.7f};
    const float M[3][3] = {{1.7051f, -0.6218f, -0.0833f}, {-0.1303f, 1.1408f, -0.0105f}, {-0.0240f, -0.1290f, 1.1530f}};
    const std::string tiled = writeImage("tex_ACEScg.exr", 64, 64, {c[0], c[1], c[2]}, OIIO::TypeDesc::FLOAT, true);
    const std::string plain = writeImage("plain_ACEScg.exr", 8, 8, {c[0], c[1], c[2]}, OIIO::TypeDesc::FLOAT);
    for (std::string const &p : {tiled, plain}) {
        const Lookup l = sample(ts, ts.Acquire({p, "", ""}), slot, 0.5f, 0.5f);
        REQUIRE(l.status == 0);
        for (int r = 0; r < 3; ++r)
            CHECK_THAT(l.v[r], WithinAbs(M[r][0] * c[0] + M[r][1] * c[1] + M[r][2] * c[2], 2e-3));
    }
    // A grey PNG is linear (1-channel rule) and replicated to RGB.
    const std::string grey = writeImage("grey.png", 8, 8, {64 / 255.0f}, OIIO::TypeDesc::UINT8);
    const Lookup g = sample(ts, ts.Acquire({grey, "", ""}), slot, 0.5f, 0.5f);
    for (int k = 0; k < 3; ++k)
        CHECK_THAT(g.v[k], WithinAbs(64 / 255.0, 1e-3));
    // RGBA (255, 255, 255, 128): premultiplied in linear space, as Karma.
    const std::string rgba = writeImage("rgba.png", 8, 8, {1.0f, 1.0f, 1.0f, 128 / 255.0f}, OIIO::TypeDesc::UINT8,
                                        false, true);
    nova::TexSlot slot4 = slot;
    slot4.channels = 4;
    const Lookup a = sample(ts, ts.Acquire({rgba, "", ""}), slot4, 0.5f, 0.5f);
    CHECK_THAT(a.v[0], WithinAbs(0.502, 0.003));
    CHECK_THAT(a.v[3], WithinAbs(128 / 255.0, 2e-3));
}

TEST_CASE("Missing files, .rat files and the conversion cache", "[hydra][textures]") {
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    const nova::TextureId missing = ts.Acquire({testDir() + "/does_not_exist.exr", "", ""});
    CHECK(ts.MissingCount() == 1);
    CHECK(sample(ts, missing, rgbSlot(), 0.5f, 0.5f).status == 1);
    CHECK(ts.Acquire({testDir() + "/does_not_exist.exr", "", ""}) == missing); // reported once
    CHECK(ts.MissingCount() == 1);
    CHECK(ts.Acquire({"", "", ""}) == nova::kNoTexture);

    // .rat (no OIIO reader): Houdini's imaketx, sRGB code 128 -> 0.2158.
    const std::string png = writeImage("rat_src.png", 8, 8, {128 / 255.0f, 128 / 255.0f, 128 / 255.0f},
                                       OIIO::TypeDesc::UINT8);
    const std::string rat = testDir() + "/c128.rat";
    const std::string cmd = "\"\"" NOVA_HFS "/bin/imaketx.exe\" \"" + png + "\" \"" + rat + "\"\"";
    REQUIRE(std::system(cmd.c_str()) == 0);
    const Lookup r = sample(ts, ts.Acquire({rat, "", ""}), rgbSlot(), 0.5f, 0.5f);
    REQUIRE(r.status == 0);
    CHECK_THAT(r.v[0], WithinAbs(0.2158, 1e-3));

    // Converting the same source again reuses the cached file.
    const HdNovaPreparedTexture first = HdNovaPrepareTexture(png, "", "", testOptions());
    REQUIRE((first.found && first.converted));
    const auto time = fs::last_write_time(fs::u8path(first.lookupPath));
    const HdNovaPreparedTexture second = HdNovaPrepareTexture(png, "", "", testOptions());
    CHECK(second.lookupPath == first.lookupPath);
    CHECK(fs::last_write_time(fs::u8path(second.lookupPath)) == time);
    // A different color space is another file.
    CHECK(HdNovaPrepareTexture(png, "raw", "", testOptions()).lookupPath != first.lookupPath);
    // The name also holds the transform to the rendering space (the OCIO
    // config), so a file converted under another config is not reused.
    CHECK(HdNovaSceneLinearTransformId("srgb_tx") != HdNovaSceneLinearTransformId(""));
    CHECK(HdNovaSceneLinearTransformId("srgb_tx") != HdNovaSceneLinearTransformId("ACEScg"));
}

TEST_CASE("Grey files with an explicit color space are decoded", "[hydra][textures]") {
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    // Code 128 in one channel, sRGB by the hint (UsdUVTexture sourceColorSpace
    // "sRGB"): decoded in every channel, not the codes (0.502).
    const std::string grey = writeImage("grey128.png", 8, 8, {128 / 255.0f}, OIIO::TypeDesc::UINT8);
    const Lookup g = sample(ts, ts.Acquire({grey, "srgb_texture", ""}), rgbSlot(), 0.5f, 0.5f);
    REQUIRE(g.status == 0);
    for (int k = 0; k < 3; ++k)
        CHECK_THAT(g.v[k], WithinAbs(0.215861, 1e-4));
    // Grey and alpha: the grey decoded, the alpha kept.
    const std::string greyAlpha = writeImage("grey128a.png", 8, 8, {128 / 255.0f, 1.0f}, OIIO::TypeDesc::UINT8);
    nova::TexSlot slot4 = rgbSlot();
    slot4.channels = 4;
    const Lookup a = sample(ts, ts.Acquire({greyAlpha, "srgb_texture", ""}), slot4, 0.5f, 0.5f);
    REQUIRE(a.status == 0);
    CHECK_THAT(a.v[1], WithinAbs(0.215861, 1e-4));
    CHECK_THAT(a.v[3], WithinAbs(1.0, 1e-3));
}

TEST_CASE("Direct UDIM sets see changed and added tiles", "[hydra][textures]") {
    // Tiled, MIP-mapped linear EXRs: read directly, not converted. Distinct
    // colors everywhere, since OIIO shares files with equal pixels.
    const auto writeTile = [](std::string const &set, int udim, std::vector<float> const &rgb) {
        const std::string name = set + "." + std::to_string(udim) + ".exr";
        const fs::path p = fs::u8path(testDir() + "/" + name);
        std::error_code ec;
        const auto before = fs::last_write_time(p, ec);
        OIIO::ImageCache::create(true)->close_all(); // Windows cannot replace a file OIIO holds open
        writeImage(name, 16, 16, rgb, OIIO::TypeDesc::HALF, true);
        if (!ec) // a later time, as an artist's re-export (OIIO compares whole seconds)
            fs::last_write_time(p, before + std::chrono::seconds(2));
    };
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    const nova::TexSlot slot = rgbSlot(nova::TexFilter::Closest);

    // A tile rewritten in place.
    writeTile("inplace", 1001, {0.5f, 0.5f, 0.0f});
    const HdNovaTextureRequest inplace{testDir() + "/inplace.<UDIM>.exr", "raw", ""};
    const nova::TextureId a = ts.Acquire(inplace);
    REQUIRE(HdNovaPrepareTexture(inplace.path, "raw", "", testOptions()).converted == false);
    CHECK_THAT(sample(ts, a, slot, 0.5f, 0.5f).v[0], WithinAbs(0.5, 1e-3));
    writeTile("inplace", 1001, {0.0f, 0.5f, 0.5f});
    CHECK(ts.Acquire(inplace) == a);
    CHECK_THAT(sample(ts, a, slot, 0.5f, 0.5f).v[0], WithinAbs(0.0, 1e-3));

    // A tile rewritten and one added.
    writeTile("grown", 1001, {0.25f, 0.0f, 0.0f});
    const HdNovaTextureRequest grown{testDir() + "/grown.<UDIM>.exr", "raw", ""};
    const nova::TextureId b = ts.Acquire(grown);
    CHECK_THAT(sample(ts, b, slot, 0.5f, 0.5f).v[0], WithinAbs(0.25, 1e-3));
    CHECK(sample(ts, b, slot, 1.5f, 0.5f).status == 1);
    writeTile("grown", 1001, {0.0f, 0.75f, 0.0f});
    writeTile("grown", 1002, {0.0f, 0.0f, 0.375f});
    CHECK(ts.Acquire(grown) == b);
    const Lookup l1001 = sample(ts, b, slot, 0.5f, 0.5f), l1002 = sample(ts, b, slot, 1.5f, 0.5f);
    REQUIRE((l1001.status == 0 && l1002.status == 0));
    CHECK_THAT(l1001.v[1], WithinAbs(0.75, 1e-3));
    CHECK_THAT(l1002.v[2], WithinAbs(0.375, 1e-3));
}

TEST_CASE("Textures inside .usdz packages", "[hydra][textures]") {
    // An 8-bit PNG (code 128) packaged as tex/c128.png.
    const std::string png =
        writeImage("usdz_c128.png", 8, 8, {128 / 255.0f, 128 / 255.0f, 128 / 255.0f}, OIIO::TypeDesc::UINT8);
    const std::string package = testDir() + "/asset.usdz";
    {
        SdfZipFileWriter zip = SdfZipFileWriter::CreateNew(package);
        REQUIRE(!zip.AddFile(png, "tex/c128.png").empty());
        REQUIRE(zip.Save());
    }
    const std::string packaged = package + "[tex/c128.png]";
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    const Lookup l = sample(ts, ts.Acquire({packaged, "", ""}), rgbSlot(), 0.5f, 0.5f);
    CHECK(ts.MissingCount() == 0);
    REQUIRE(l.status == 0);
    CHECK_THAT(l.v[0], WithinAbs(0.215861, 1e-4)); // sRGB as a .png
    CHECK(!HdNovaTextureSignature(packaged).empty());

    // Light textures read it as well.
    HdNovaTextureCache lights;
    const std::shared_ptr<const nova::Image> image = lights.Load(packaged);
    REQUIRE(image);
    REQUIRE(image->width == 8);
    CHECK_THAT(image->pixels[0].x, WithinAbs(0.215861, 1e-4));
}

TEST_CASE("Image nodes translate to texture slots", "[hydra][textures]") {
    auto node = [](char const *id) {
        HdMaterialNode2 n;
        n.nodeTypeId = TfToken(id);
        return n;
    };
    HdNovaNetworkSource src;
    src.context = TfToken("mtlx");
    HdMaterialNode2 surface = node("ND_standard_surface_surfaceshader");
    surface.inputConnections[TfToken("base_color")] = {{SdfPath("albedo"), TfToken("out")}};
    surface.inputConnections[TfToken("specular_roughness")] = {{SdfPath("rough"), TfToken("out")}};
    surface.inputConnections[TfToken("emission_color")] = {{SdfPath("albedo2"), TfToken("out")}};
    src.net.nodes[SdfPath("surface")] = surface;
    src.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("surface"), TfToken()};
    HdMaterialNode2 albedo = node("ND_image_color3");
    albedo.parameters[TfToken("file")] = VtValue(SdfAssetPath("tex/albedo.png", "C:/tex/albedo.png"));
    albedo.parameters[TfToken("uaddressmode")] = VtValue(std::string("clamp"));
    src.net.nodes[SdfPath("albedo")] = albedo;
    src.net.nodes[SdfPath("albedo2")] = albedo; // the same file in another color space
    HdMaterialNode2 rough = node("ND_image_float");
    rough.parameters[TfToken("file")] = VtValue(SdfAssetPath("tex/rough.png", "C:/tex/rough.png"));
    src.net.nodes[SdfPath("rough")] = rough;
    src.meta[SdfPath("albedo2")][TfToken("file")].colorSpace = TfToken("srgb_texture");

    const HdNovaTranslatedMaterial tr = HdNovaTranslateMaterial(src);
    REQUIRE(tr.ok);
    CHECK(tr.warnings.empty());
    REQUIRE(tr.textures.size() == 3); // albedo, albedo2 (another color space), rough
    REQUIRE(tr.graph.textures.size() == 3);
    CHECK(tr.textures[0].path == "C:/tex/albedo.png");
    CHECK(tr.textures[0].colorSpace.empty()); // color: automatic
    CHECK(tr.graph.textures[0].wrapS == nova::TexWrap::Clamp);
    CHECK(tr.graph.textures[0].wrapT == nova::TexWrap::Periodic);
    CHECK(tr.graph.textures[0].channels == 3);
    bool sawRaw = false, sawSrgb = false;
    for (size_t i = 1; i < 3; ++i) {
        sawRaw |= tr.textures[i].colorSpace == "raw" && tr.graph.textures[i].channels == 1;
        sawSrgb |= tr.textures[i].colorSpace == "srgb_texture";
    }
    CHECK(sawRaw);
    CHECK(sawSrgb);

    // UsdUVTexture: sourceColorSpace and scale/bias; an empty file is the fallback.
    HdNovaNetworkSource usd;
    usd.context = TfToken();
    HdMaterialNode2 unlit = node("ND_surface_unlit");
    unlit.inputConnections[TfToken("emission_color")] = {{SdfPath("tex"), TfToken("rgb")}};
    usd.net.nodes[SdfPath("surface")] = unlit;
    usd.net.terminals[HdMaterialTerminalTokens->surface] = {SdfPath("surface"), TfToken()};
    HdMaterialNode2 uv = node("UsdUVTexture");
    uv.parameters[TfToken("file")] = VtValue(SdfAssetPath("n.png", "C:/tex/n.png"));
    uv.parameters[TfToken("sourceColorSpace")] = VtValue(TfToken("raw"));
    uv.parameters[TfToken("wrapS")] = VtValue(TfToken("repeat"));
    usd.net.nodes[SdfPath("tex")] = uv;
    const HdNovaTranslatedMaterial tu = HdNovaTranslateMaterial(usd);
    REQUIRE(tu.ok);
    REQUIRE(tu.textures.size() == 1);
    CHECK(tu.textures[0].colorSpace == "raw");
    CHECK(tu.graph.textures[0].channels == 4);
    CHECK(tu.graph.textures[0].wrapS == nova::TexWrap::Periodic);
}

namespace {

// An unlit quad over [-1, 1]^2 (st 0..1) showing the texture, rendered
// orthographically at `pixels`^2 with `spp` samples: the image mean.
float renderTextured(HdNovaTextureSystem const &ts, nova::TextureId texture, int pixels, int spp) {
    nova::Scene scene;
    nova::MeshDesc quad;
    quad.points = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    quad.indices = {0, 1, 2, 0, 2, 3};
    quad.setAttribute({"st", nova::AttributeRate::Vertex, 2, {0, 0, 1, 0, 1, 1, 0, 1}});
    const nova::MeshId id = scene.createMesh();
    scene.updateMesh(id, quad);
    nova::shading::IrGraph g;
    g.model = nova::SurfaceModel::Unlit;
    nova::shading::IrBuilder b(g);
    nova::TexSlot slot;
    slot.channels = 1;
    const uint32_t s = b.textureSlot(slot);
    b.output(nova::surface_field::emissionColor, 3,
             b.tex2D(s, b.primvar("st", 2, b.constant(0.0f)), b.constant(-1.0f)));
    nova::MaterialDesc material;
    material.inputs = nova::unlitDefaults();
    material.program = nova::shading::compile(g).program;
    REQUIRE(material.program);
    material.textures = {texture};
    const nova::MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(id, {m});
    scene.setTextureSampler(&ts);
    scene.commit();

    nova::Renderer renderer;
    nova::RenderSettings settings;
    settings.maxSamples = uint32_t(spp);
    settings.pixelFilterScale = 0.0f; // pixel centers: the footprint is exactly one pixel
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(nova::test::makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, pixels, pixels));
    renderer.resize(pixels, pixels);
    nova::test::renderToConvergence(renderer);
    double sum = 0.0;
    for (int y = 0; y < pixels; ++y)
        for (int x = 0; x < pixels; ++x)
            sum += nova::test::pixelMean(renderer.film(), x, y).x;
    return float(sum / double(pixels * pixels));
}

} // namespace

TEST_CASE("Ray cones pick Karma's MIP levels", "[hydra][textures][cones]") {
    // 1024^2, MIP level L holds the constant L (linear, already a texture).
    std::string list;
    for (int l = 1, size = 512; size >= 1; ++l, size /= 2) {
        const std::string name = "level" + std::to_string(l) + ".exr";
        writeImage(name, size, size, {float(l)}, OIIO::TypeDesc::FLOAT);
        list += (list.empty() ? "" : ";") + testDir() + "/" + name;
    }
    const std::string levels = testDir() + "/levels.exr";
    {
        OIIO::ImageBuf top(OIIO::ImageSpec(1024, 1024, 1, OIIO::TypeDesc::FLOAT));
        const float zero = 0.0f;
        OIIO::ImageBufAlgo::fill(top, &zero);
        OIIO::ImageSpec config;
        config.tile_width = config.tile_height = 64;
        config.set_format(OIIO::TypeDesc::FLOAT);
        config.attribute("maketx:mipimages", list);
        REQUIRE(OIIO::ImageBufAlgo::make_texture(OIIO::ImageBufAlgo::MakeTxTexture, top, levels, config));
    }
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    const nova::TextureId id = ts.Acquire({levels, "raw", ""});
    REQUIRE(id != nova::kNoTexture);
    // nova:textureFilterWidth 0.25 (the default): level log2(texels per pixel) - 2.
    CHECK_THAT(renderTextured(ts, id, 64, 16), WithinAbs(2.0, 0.02));  // 16 texels per pixel
    CHECK_THAT(renderTextured(ts, id, 16, 16), WithinAbs(4.0, 0.05));  // 64
    CHECK(renderTextured(ts, id, 256, 4) < 0.05f);                     // 4

    // A 1-texel black/white checker minified 64 times averages to 0.5.
    const std::string checker = testDir() + "/fine_checker.exr";
    {
        OIIO::ImageBuf buf(OIIO::ImageSpec(1024, 1024, 1, OIIO::TypeDesc::FLOAT));
        const float white = 1.0f, black = 0.0f;
        OIIO::ImageBufAlgo::checker(buf, 1, 1, 1, &white, &black);
        REQUIRE(buf.write(checker, OIIO::TypeDesc::HALF));
    }
    CHECK_THAT(renderTextured(ts, ts.Acquire({checker, "raw", ""}), 16, 16), WithinAbs(0.5, 0.005));
}

namespace {

// Keeps what textureLevels hands over, per level (texels as 4 x uint16_t
// halfs or 4 floats), and the largest band.
struct DeviceLevels final : nova::TexRowSink {
    std::map<uint32_t, std::vector<uint8_t>> levels;
    std::map<uint32_t, int> widths;
    int largestBand = 0;
    size_t texel = 8;
    bool put(nova::TexRows const &r) override {
        std::vector<uint8_t> &l = levels[r.level];
        const size_t row = size_t(r.width) * texel;
        l.resize(row * size_t(r.height));
        std::memcpy(&l[size_t(r.y) * row], r.texels, row * size_t(r.rows));
        widths[r.level] = r.width;
        largestBand = std::max(largestBand, r.rows);
        return true;
    }
    template <class T> T at(uint32_t level, size_t i) const {
        T v;
        std::memcpy(&v, &levels.at(level)[i * sizeof(T)], sizeof(T));
        return v;
    }
};

} // namespace

// R1-48: the GPU's texture data. textureInfo reads only the headers;
// textureLevels reads only the requested levels of one tile, a band of rows
// at a time, and makes a level the file does not store from its last stored
// one while reading that once.
TEST_CASE("GPU texture data reads only the requested levels", "[hydra][textures][budget]") {
    // 256^2 half, tiled and MIP-mapped (read directly), level l constant l + 1.
    std::string list;
    for (int l = 1, size = 128; size >= 1; ++l, size /= 2) {
        const std::string name = "dev_level" + std::to_string(l) + ".exr";
        writeImage(name, size, size, {float(l + 1)}, OIIO::TypeDesc::FLOAT);
        list += (list.empty() ? "" : ";") + testDir() + "/" + name;
    }
    const std::string mipped = testDir() + "/dev_levels.exr";
    {
        OIIO::ImageBuf top(OIIO::ImageSpec(256, 256, 1, OIIO::TypeDesc::FLOAT));
        const float one = 1.0f;
        OIIO::ImageBufAlgo::fill(top, &one);
        OIIO::ImageSpec config;
        config.tile_width = config.tile_height = 32;
        config.set_format(OIIO::TypeDesc::HALF);
        config.attribute("maketx:mipimages", list);
        REQUIRE(OIIO::ImageBufAlgo::make_texture(OIIO::ImageBufAlgo::MakeTxTexture, top, mipped, config));
    }
    HdNovaTextureSystem ts;
    ts.SetOptions(testOptions());
    const nova::TextureId id = ts.Acquire({mipped, "raw", ""});
    nova::TexInfo info;
    REQUIRE(ts.textureInfo(id, info));
    REQUIRE(info.tiles.size() == 1);
    CHECK(!info.udim);
    CHECK(info.half);
    CHECK((info.tiles[0].width == 256 && info.tiles[0].height == 256));
    CHECK(info.tiles[0].stored == 9);
    CHECK(ts.DeviceTexelsRead() == 0); // headers only

    // Levels 3 to 8 (32^2 down to 1): only their texels are read.
    DeviceLevels got;
    nova::TexLevelRequest request;
    request.first = 3;
    request.count = 6;
    REQUIRE(ts.textureLevels(id, request, got));
    CHECK(ts.DeviceTexelsRead() == 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1);
    REQUIRE(got.levels.size() == 6);
    CHECK(got.levels.count(2) == 0);
    CHECK(got.widths[3] == 32);
    CHECK(got.largestBand <= nova::LevelWriter::kBandRows);
    bool constant = true;
    for (uint32_t l = 3; l <= 8; ++l) {
        const size_t n = size_t(got.widths[l]) * size_t(got.widths[l]);
        for (size_t i = 0; i < n; ++i)
            constant = constant && got.at<uint16_t>(l, i * 4) == nova::floatToHalf(float(l + 1)) &&
                       got.at<uint16_t>(l, i * 4 + 2) == nova::floatToHalf(float(l + 1)) && // grey to RGB
                       got.at<uint16_t>(l, i * 4 + 3) == nova::floatToHalf(1.0f);
    }
    CHECK(constant);

    // A plain scanline file read directly (nova:textureAutoTx off): level 0
    // only; its level 2 is made from it, a 4 x 4 box per texel.
    const std::string ramp = testDir() + "/dev_ramp.exr";
    {
        std::vector<float> px(300 * 200);
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 300; ++x)
                px[size_t(y) * 300 + size_t(x)] = float(x);
        auto out = OIIO::ImageOutput::create(ramp);
        REQUIRE(out);
        REQUIRE(out->open(ramp, OIIO::ImageSpec(300, 200, 1, OIIO::TypeDesc::FLOAT)));
        REQUIRE(out->write_image(OIIO::TypeDesc::FLOAT, px.data()));
        out->close();
    }
    HdNovaTextureOptions direct = testOptions();
    direct.autoTx = false;
    HdNovaTextureSystem plain;
    plain.SetOptions(direct);
    const nova::TextureId rid = plain.Acquire({ramp, "raw", ""});
    nova::TexInfo rinfo;
    REQUIRE(plain.textureInfo(rid, rinfo));
    CHECK(!rinfo.half);
    CHECK((rinfo.tiles[0].width == 300 && rinfo.tiles[0].stored == 1));
    DeviceLevels made;
    made.texel = 16;
    request = {};
    request.first = 2;
    request.half = false;
    REQUIRE(plain.textureLevels(rid, request, made));
    CHECK(plain.DeviceTexelsRead() == 300 * 200);
    REQUIRE(made.widths[2] == 75);
    REQUIRE(made.levels[2].size() == size_t(75 * 50) * 16);
    bool means = true;
    for (size_t y = 0; y < 50; ++y)
        for (size_t x = 0; x < 75; ++x) {
            const size_t i = (y * 75 + x) * 4;
            const float m = 4.0f * float(x) + 1.5f;
            means = means && made.at<float>(2, i) == m && made.at<float>(2, i + 1) == m && made.at<float>(2, i + 3) == 1.0f;
        }
    CHECK(means);

    // A UDIM set: each tile's shape, and one tile's coarsest level.
    writeImage("devudim.1001.exr", 16, 16, {0.25f, 0.25f, 0.25f}, OIIO::TypeDesc::HALF, true);
    writeImage("devudim.1002.exr", 32, 32, {0.5f, 0.75f, 1.0f}, OIIO::TypeDesc::HALF, true);
    const nova::TextureId uid = ts.Acquire({testDir() + "/devudim.<UDIM>.exr", "raw", ""});
    nova::TexInfo uinfo;
    REQUIRE(ts.textureInfo(uid, uinfo));
    CHECK(uinfo.udim);
    REQUIRE(uinfo.tiles.size() == 2);
    CHECK((uinfo.tiles[0].udim == 1001 && uinfo.tiles[0].width == 16 && uinfo.tiles[0].stored == 5));
    CHECK((uinfo.tiles[1].udim == 1002 && uinfo.tiles[1].width == 32 && uinfo.tiles[1].stored == 6));
    DeviceLevels tile;
    request = {};
    request.udim = 1002;
    request.first = 5;
    const uint64_t before = ts.DeviceTexelsRead();
    REQUIRE(ts.textureLevels(uid, request, tile));
    CHECK(ts.DeviceTexelsRead() == before + 1);
    CHECK(tile.at<uint16_t>(5, 1) == nova::floatToHalf(0.75f));
    request.udim = 1011; // no such tile
    CHECK(!ts.textureLevels(uid, request, tile));
}
