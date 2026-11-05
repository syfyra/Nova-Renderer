// Third throwaway probe: cost of automip on an untiled 8-bit image versus a
// one-time make_texture conversion (sRGB -> scene_linear, half, tiled, mipped).
#include <OpenImageIO/texture.h>
#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace OIIO;
static std::string D = "data/";
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    _putenv_s("OCIO", "C:/Program Files/Side Effects Software/Houdini 22.0.466/packages/ocio/houdini-config-v3.0.0_aces-v2.0_ocio-v2.5.ocio");
    const int R = 4096;
    {
        ImageSpec spec(R, R, 3, TypeDesc::UINT8);
        ImageBuf b(spec);
        ImageBufAlgo::noise(b, "uniform", 0.f, 1.f, false, 3);
        b.write(D + "big8.png");
    }
    // automip on the untiled PNG
    for (int automip : {1}) {
        TextureSystem *ts = TextureSystem::create(false);
        ts->attribute("automip", automip);
        ts->attribute("max_memory_MB", 4096.f);
        ustring name(D + "big8.png");
        TextureOpt opt;
        float r[3];
        auto t0 = clk::now();
        ts->texture(name, opt, 0.3f, 0.3f, 1.f / R, 0, 0, 1.f / R, 3, r); // level 0: reads the whole image
        auto t1 = clk::now();
        ts->texture(name, opt, 0.3f, 0.3f, 16.f / R, 0, 0, 16.f / R, 3, r); // level 4: generates 1..4 locally
        auto t2 = clk::now();
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> U(0, 1);
        for (int i = 0; i < 200000; ++i) ts->texture(name, opt, U(rng), U(rng), 16.f / R, 0, 0, 16.f / R, 3, r); // whole level 4
        auto t3 = clk::now();
        float mem = 0; ts->getattribute("max_memory_MB", mem);
        long long used = 0; ts->imagecache()->getattribute("stat:cache_memory_used", TypeInt64, &used);
        std::printf("automip=%d untiled 4k uint8 PNG: first level-0 lookup %.0f ms, first level-4 lookup %.0f ms, 200k random level-4 lookups %.0f ms; cache used %.0f MB (max_memory_MB now %.0f)\n",
                    automip, ms(t0, t1), ms(t1, t2), ms(t2, t3), used / 1048576.0, mem);
        TextureSystem::destroy(ts);
    }
    // one-time conversion
    {
        ImageSpec cfg;
        cfg.tile_width = cfg.tile_height = 64;
        cfg.set_format(TypeDesc::HALF);
        cfg.attribute("maketx:incolorspace", "sRGB Encoded Rec.709 (sRGB)");
        cfg.attribute("maketx:outcolorspace", "scene_linear");
        cfg.attribute("maketx:fixnan", "box3");
        cfg.attribute("maketx:constant_color_detect", 1);
        cfg.attribute("maketx:opaque_detect", 1);
        cfg.attribute("maketx:fileformatname", "openexr");
        cfg.attribute("compression", "zip");
        auto t0 = clk::now();
        bool ok = ImageBufAlgo::make_texture(ImageBufAlgo::MakeTxTexture, D + "big8.png", D + "big8_lin.exr", cfg);
        auto t1 = clk::now();
        std::printf("make_texture 4k uint8 PNG -> half linear tiled mipped EXR: ok=%d %.0f ms\n", ok, ms(t0, t1));
        if (!ok) std::printf("  %s\n", OIIO::geterror().c_str());
        // precision check on the dark constant (code 10)
        ok = ImageBufAlgo::make_texture(ImageBufAlgo::MakeTxTexture, D + "c10.png", D + "c10_lin.exr", cfg);
        TextureSystem *ts = TextureSystem::create(false);
        TextureOpt opt; float r[3];
        ts->texture(ustring(D + "c10_lin.exr"), opt, 0.5f, 0.5f, 0, 0, 0, 0, 3, r);
        std::printf("c10.png converted to half linear: %.6f (expected 0.003035)\n", r[0]);
        TextureSystem::destroy(ts);
    }
    return 0;
}
