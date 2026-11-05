// Throwaway probe of Houdini 22's OIIO 2.5.18 TextureSystem behaviour.
#include <OpenImageIO/texture.h>
#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/color.h>
#include <OpenImageIO/filesystem.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
using namespace OIIO;

static std::string D = "data/";
static float srgb2lin(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

static void writeConst(const std::string &name, int w, int h, std::vector<float> rgb, TypeDesc fmt, bool tiled = false) {
    ImageSpec spec(w, h, (int)rgb.size(), fmt);
    if (tiled) { spec.tile_width = 64; spec.tile_height = 64; }
    ImageBuf b(spec);
    ImageBufAlgo::fill(b, rgb);
    if (tiled) b.set_write_tiles(64, 64);
    if (!b.write(D + name)) std::printf("write %s failed: %s\n", name.c_str(), b.geterror().c_str());
}

// Tiled EXR whose MIP level L holds the constant value L (single channel).
static void writeMipLevels(const std::string &name, int w) {
    std::string mipimages;
    int level = 0;
    for (int res = w;; res /= 2, ++level) {
        const std::string lvl = "miplevel" + std::to_string(level) + ".exr";
        ImageSpec s(res, res, 1, TypeDesc::FLOAT);
        ImageBuf b(s);
        ImageBufAlgo::fill(b, {float(level)});
        b.write(D + lvl);
        if (level > 0)
            mipimages += (mipimages.empty() ? "" : ";") + D + lvl;
        if (res == 1) break;
    }
    ImageSpec cfg;
    cfg.tile_width = cfg.tile_height = 64;
    cfg.attribute("maketx:mipimages", mipimages);
    cfg.attribute("maketx:constant_color_detect", 0);
    cfg.attribute("maketx:fileformatname", "openexr");
    if (!ImageBufAlgo::make_texture(ImageBufAlgo::MakeTxTexture, D + "miplevel0.exr", D + name, cfg))
        std::printf("make_texture mips failed: %s\n", OIIO::geterror().c_str());
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    _putenv_s("OCIO", "C:/Program Files/Side Effects Software/Houdini 22.0.466/packages/ocio/houdini-config-v3.0.0_aces-v2.0_ocio-v2.5.ocio");
    Filesystem::create_directory(D);
    std::printf("OIIO %s\n", OIIO_VERSION_STRING);

    // ---- 1. colour transforms ---------------------------------------------
    const int codeDark = 10, codeMid = 128;
    for (int code : {codeDark, codeMid}) {
        std::string png = "c" + std::to_string(code) + ".png";
        writeConst(png, 256, 256, {code / 255.f, code / 255.f, code / 255.f}, TypeDesc::UINT8);
        ImageSpec cfg; cfg.tile_width = cfg.tile_height = 64; // keep uint8 (format unknown = input)
        if (!ImageBufAlgo::make_texture(ImageBufAlgo::MakeTxTexture, D + png, D + "c" + std::to_string(code) + "_u8.tx", cfg))
            std::printf("make_texture failed: %s\n", OIIO::geterror().c_str());
        writeConst("c" + std::to_string(code) + "_tiled.tif", 256, 256, {code / 255.f, code / 255.f, code / 255.f}, TypeDesc::UINT8, true);
    }
    for (int forcefloat : {0, 1}) {
        TextureSystem *ts = TextureSystem::create(false);
        ts->attribute("forcefloat", forcefloat);
        ts->attribute("automip", 1);
        int id = ts->get_colortransform_id(ustring("sRGB Encoded Rec.709 (sRGB)"), ustring("scene_linear"));
        int idSrgb = ts->get_colortransform_id(ustring("sRGB"), ustring());
        int idLin = ts->get_colortransform_id(ustring("Linear Rec.709 (sRGB)"), ustring("scene_linear"));
        int idBad = ts->get_colortransform_id(ustring("nonexistent"), ustring("scene_linear"));
        std::printf("\n[forcefloat=%d] colortransform ids: srgb_tx=%#x 'sRGB'=%#x linear=%d unknown=%d\n", forcefloat, id, idSrgb, idLin, idBad);
        for (int code : {codeDark, codeMid}) {
            for (std::string f : {"c" + std::to_string(code) + ".png", "c" + std::to_string(code) + "_tiled.tif", "c" + std::to_string(code) + "_u8.tx"}) {
                TextureOpt opt; opt.colortransformid = id;
                float r[3] = {-1, -1, -1};
                bool ok = ts->texture(ustring(D + f), opt, 0.5f, 0.5f, 0, 0, 0, 0, 3, r);
                float rb[3] = {-1, -1, -1};
                ok &= ts->texture(ustring(D + f), opt, 0.5f, 0.5f, 0.3f, 0, 0, 0.3f, 3, rb);
                std::printf("  %-16s ok=%d  fine=%.6f  coarse=%.6f   expected linear=%.6f (raw=%.6f)\n", f.c_str(), ok, r[0], rb[0],
                            srgb2lin(code / 255.f), code / 255.f);
            }
        }
        TextureSystem::destroy(ts);
    }

    // ---- 2. MIP level selection -------------------------------------------
    writeMipLevels("mips.exr", 1024);
    {
        TextureSystem *ts = TextureSystem::create(false);
        int nmip = 0;
        ts->get_texture_info(ustring(D + "mips.exr"), 0, ustring("miplevels"), TypeInt, &nmip);
        std::printf("\nMIP selection on a 1024^2 texture (%d levels) whose level L has value L (texel = 1/1024):\n", nmip);
        std::printf("  %-10s %-10s %-10s %-10s\n", "width(tx)", "Default", "Trilinear", "OneLevel");
        for (float texels : {0.5f, 1.f, 1.5f, 2.f, 3.f, 4.f, 8.f, 16.f, 64.f}) {
            float d = texels / 1024.f;
            float v[3];
            for (int m = 0; m < 3; ++m) {
                TextureOpt opt;
                opt.mipmode = m == 0 ? TextureOpt::MipModeDefault : m == 1 ? TextureOpt::MipModeTrilinear : TextureOpt::MipModeOneLevel;
                ts->texture(ustring(D + "mips.exr"), opt, 0.37f, 0.61f, d, 0, 0, d, 1, &v[m]);
            }
            std::printf("  %-10g %-10.4f %-10.4f %-10.4f\n", texels, v[0], v[1], v[2]);
        }
        std::printf("  anisotropic footprint (major 16 tx, minor 1 tx):\n");
        for (int aniso : {1, 4, 16, 32}) {
            TextureOpt opt; opt.anisotropic = aniso;
            float v;
            ts->texture(ustring(D + "mips.exr"), opt, 0.37f, 0.61f, 16.f / 1024, 0, 0, 1.f / 1024, 1, &v);
            std::printf("    anisotropic=%-3d -> level %.4f\n", aniso, v);
        }
        for (float blur : {0.f, 0.01f, 0.1f}) {
            TextureOpt opt; opt.sblur = opt.tblur = blur;
            float v;
            ts->texture(ustring(D + "mips.exr"), opt, 0.37f, 0.61f, 1.f / 1024, 0, 0, 1.f / 1024, 1, &v);
            std::printf("    blur=%g -> level %.4f\n", blur, v);
        }
        TextureSystem::destroy(ts);
    }

    // ---- 3. UDIM ------------------------------------------------------------
    writeConst("udim.1001.exr", 64, 64, {1, 0, 0}, TypeDesc::HALF, true);
    writeConst("udim.1002.exr", 64, 64, {0, 1, 0}, TypeDesc::HALF, true);
    writeConst("udim.1011.exr", 64, 64, {0, 0, 1}, TypeDesc::HALF, true);
    {
        ImageSpec spec(64, 64, 3, TypeDesc::HALF); spec.tile_width = spec.tile_height = 64;
        ImageBuf b(spec);
        ImageBufAlgo::fill(b, {1.f, 1.f, 1.f}, ROI(0, 64, 0, 32, 0, 1, 0, 3)); // rows 0..31 = top of the image
        b.write(D + "udimTB.1001.exr");
        b.write(D + "udimTB.1011.exr");
    }
    for (int flip : {0, 1}) {
        TextureSystem *ts = TextureSystem::create(false);
        ts->attribute("flip_t", flip);
        ustring pat(D + "udim.<UDIM>.exr");
        TextureSystem::Perthread *pt = ts->get_perthread_info();
        TextureSystem::TextureHandle *h = ts->get_texture_handle(pat, pt);
        std::vector<ustring> names; int nu = 0, nv = 0;
        ts->inventory_udim(h, pt, names, nu, nv);
        std::printf("\n[flip_t=%d] UDIM is_udim=%d good=%d inventory nu=%d nv=%d:", flip, ts->is_udim(h), ts->good(h), nu, nv);
        for (auto &n : names) std::printf(" '%s'", n.c_str());
        std::printf("\n");
        float miss[3] = {1, 0, 1};
        for (auto st : std::vector<std::pair<float, float>>{{0.5f, 0.5f}, {1.5f, 0.5f}, {0.5f, 1.5f}, {1.5f, 1.5f}, {12.5f, 0.5f}, {-0.5f, 0.5f}}) {
            TextureOpt opt; float r[3] = {-1, -1, -1};
            bool ok = ts->texture(h, pt, opt, st.first, st.second, 0, 0, 0, 0, 3, r);
            TextureOpt opt2; opt2.missingcolor = miss; float r2[3] = {-1, -1, -1};
            bool ok2 = ts->texture(h, pt, opt2, st.first, st.second, 0, 0, 0, 0, 3, r2);
            std::printf("  (s,t)=(%5.2f,%4.2f) ok=%d rgb=(%.2f %.2f %.2f) | missingcolor: ok=%d rgb=(%.2f %.2f %.2f)\n", st.first, st.second, ok, r[0], r[1], r[2], ok2, r2[0], r2[1], r2[2]);
        }
        std::printf("  error after UDIM lookups: '%s'\n", ts->geterror().c_str());
        ustring patTB(D + "udimTB.<UDIM>.exr");
        for (float t : {0.25f, 0.75f, 1.25f, 1.75f}) {
            TextureOpt opt; float r[3];
            ts->texture(patTB, opt, 0.5f, t, 0, 0, 0, 0, 3, r);
            std::printf("  udimTB t=%.2f -> %.2f (1 = image top half)\n", t, r[0]);
        }
        TextureSystem::destroy(ts);
    }

    // ---- 4. wrap modes ------------------------------------------------------
    {
        ImageSpec spec(4, 1, 1, TypeDesc::FLOAT);
        ImageBuf b(spec);
        for (int x = 0; x < 4; ++x) { float v = float(x + 1); b.setpixel(x, 0, &v); }
        b.write(D + "ramp4.exr");
        TextureSystem *ts = TextureSystem::create(false);
        std::printf("\nwrap modes on a 4x1 texture with texels 1,2,3,4 (closest interp), s=-0.1 / 1.1 / 1.6:\n");
        for (auto w : {TextureOpt::WrapDefault, TextureOpt::WrapBlack, TextureOpt::WrapClamp, TextureOpt::WrapPeriodic, TextureOpt::WrapMirror}) {
            TextureOpt opt; opt.swrap = opt.twrap = w; opt.interpmode = TextureOpt::InterpClosest; opt.mipmode = TextureOpt::MipModeNoMIP;
            float a, c, e;
            ts->texture(ustring(D + "ramp4.exr"), opt, -0.1f, 0.5f, 0, 0, 0, 0, 1, &a);
            ts->texture(ustring(D + "ramp4.exr"), opt, 1.1f, 0.5f, 0, 0, 0, 0, 1, &c);
            ts->texture(ustring(D + "ramp4.exr"), opt, 1.6f, 0.5f, 0, 0, 0, 0, 1, &e);
            std::printf("  wrap=%d: %.2f %.2f %.2f\n", (int)w, a, c, e);
        }
        TextureSystem::destroy(ts);
    }

    // ---- 5. missing files ---------------------------------------------------
    {
        TextureSystem *ts = TextureSystem::create(false);
        float r[3] = {-1, -1, -1};
        TextureOpt opt;
        bool ok = ts->texture(ustring(D + "does_not_exist.exr"), opt, 0.5f, 0.5f, 0, 0, 0, 0, 3, r);
        std::printf("\nmissing file: ok=%d rgb=(%.2f %.2f %.2f) has_error=%d err='%s'\n", ok, r[0], r[1], r[2], ts->has_error(), ts->geterror().c_str());
        float mc[3] = {1, 0, 1};
        opt.missingcolor = mc;
        ok = ts->texture(ustring(D + "does_not_exist.exr"), opt, 0.5f, 0.5f, 0, 0, 0, 0, 3, r);
        std::printf("missing file + missingcolor: ok=%d rgb=(%.2f %.2f %.2f) err='%s'\n", ok, r[0], r[1], r[2], ts->geterror().c_str());
        TextureSystem::TextureHandle *h = ts->get_texture_handle(ustring(D + "does_not_exist.exr"));
        std::printf("handle of missing file: %p good=%d\n", (void *)h, h ? (int)ts->good(h) : -1);
        int exists = -1;
        ts->get_texture_info(ustring(D + "does_not_exist.exr"), 0, ustring("exists"), TypeInt, &exists);
        std::printf("exists query = %d\n", exists);
        TextureSystem::destroy(ts);
    }

    // ---- 6. timing ----------------------------------------------------------
    {
        ImageSpec spec(4096, 4096, 3, TypeDesc::HALF);
        ImageBuf b(spec);
        ImageBufAlgo::noise(b, "uniform", 0.f, 1.f, false, 1);
        b.write(D + "big.exr");
        ImageSpec cfg; cfg.tile_width = cfg.tile_height = 64;
        ImageBufAlgo::make_texture(ImageBufAlgo::MakeTxTexture, D + "big.exr", D + "big.tx", cfg);
        TextureSystem *ts = TextureSystem::create(false);
        ts->attribute("max_memory_MB", 4096.f);
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> U(0.f, 1.f);
        const int N = 2000000;
        std::vector<float> s(N), t(N);
        for (int i = 0; i < N; ++i) { s[i] = U(rng); t[i] = U(rng); }
        ustring name(D + "big.tx");
        TextureSystem::Perthread *pt = ts->get_perthread_info();
        TextureSystem::TextureHandle *h = ts->get_texture_handle(name, pt);
        float r[3], acc = 0;
        TextureOpt opt;
        for (int i = 0; i < N; ++i) { ts->texture(h, pt, opt, s[i], t[i], 1.f / 4096, 0, 0, 1.f / 4096, 3, r); acc += r[0]; }
        for (float fp : {1.f, 64.f}) {
            float d = fp / 4096;
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < N; ++i) { ts->texture(h, pt, opt, s[i], t[i], d, 0, 0, d, 3, r); acc += r[0]; }
            auto t1 = std::chrono::steady_clock::now();
            for (int i = 0; i < N; ++i) { ts->texture(name, opt, s[i], t[i], d, 0, 0, d, 3, r); acc += r[0]; }
            auto t2 = std::chrono::steady_clock::now();
            TextureOpt optB; optB.interpmode = TextureOpt::InterpBilinear; optB.mipmode = TextureOpt::MipModeTrilinear;
            for (int i = 0; i < N; ++i) { ts->texture(h, pt, optB, s[i], t[i], d, 0, 0, d, 3, r); acc += r[0]; }
            auto t3 = std::chrono::steady_clock::now();
            TextureOpt optC; optC.interpmode = TextureOpt::InterpClosest; optC.mipmode = TextureOpt::MipModeNoMIP;
            for (int i = 0; i < N; ++i) { ts->texture(h, pt, optC, s[i], t[i], d, 0, 0, d, 3, r); acc += r[0]; }
            auto t4 = std::chrono::steady_clock::now();
            // sorted coordinates (coherent access, like a sorted shading queue)
            std::vector<int> idx(N);
            auto ns = [&](auto a, auto b) { return std::chrono::duration<double, std::nano>(b - a).count() / N; };
            std::printf("\nfootprint %g texels, random coords, warm cache, 1 thread: handle+perthread %.0f ns, by name %.0f ns, trilinear+bilinear %.0f ns, closest/nomip %.0f ns\n",
                        fp, ns(t0, t1), ns(t1, t2), ns(t2, t3), ns(t3, t4));
        }
        std::printf("(acc %g)\n", acc);
        std::string st = ts->getstats(1, false);
        std::printf("%s\n", st.substr(0, 1500).c_str());
        TextureSystem::destroy(ts);
    }
    return 0;
}
