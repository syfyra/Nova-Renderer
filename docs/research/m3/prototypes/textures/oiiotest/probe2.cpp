// Second throwaway probe: constant-colour metadata, grey->rgb, private TS on a
// shared ImageCache, multi-thread scaling.
#include <OpenImageIO/texture.h>
#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
using namespace OIIO;
static std::string D = "data/";

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // (a) constant-colour shortcut with imaketx's .tx (8-bit sRGB pixels, linear AverageColor metadata)
    {
        TextureSystem *ts = TextureSystem::create(false);
        for (const char *f : {"c128_imaketx.tx", "c128_u8.tx", "c128_imaketx.exr"}) {
            TextureOpt opt;
            float r[3] = {-1, -1, -1};
            ts->texture(ustring(D + f), opt, 0.5f, 0.5f, 0, 0, 0, 0, 3, r);
            float raw[3] = {-1, -1, -1};
            ts->get_texels(ustring(D + f), opt, 0, 10, 11, 10, 11, 0, 1, 0, 3, TypeFloat, raw);
            float cc[4] = {-1, -1, -1, -1};
            bool isConst = ts->get_texture_info(ustring(D + f), 0, ustring("constantcolor"), TypeDesc(TypeDesc::FLOAT, 3), cc);
            std::printf("(a) %-18s texture()=%.4f  raw texel=%.4f  constantcolor(ok=%d)=%.4f\n", f, r[0], raw[0], isConst, cc[0]);
        }
        TextureSystem::destroy(ts);
    }
    // (b) grey texture, 3-channel lookup, gray_to_rgb 0/1; 4-channel lookup of RGB (alpha fill)
    {
        ImageSpec spec(64, 64, 1, TypeDesc::HALF);
        ImageBuf b(spec);
        ImageBufAlgo::fill(b, {0.25f});
        b.write(D + "grey.exr");
        for (int g : {0, 1}) {
            TextureSystem *ts = TextureSystem::create(false);
            ts->attribute("gray_to_rgb", g);
            TextureOpt opt; opt.fill = 0.75f;
            float r[4] = {-1, -1, -1, -1};
            ts->texture(ustring(D + "grey.exr"), opt, 0.5f, 0.5f, 0, 0, 0, 0, 3, r);
            float r4[4] = {-1, -1, -1, -1};
            ts->texture(ustring(D + "c128_imaketx.exr"), opt, 0.5f, 0.5f, 0, 0, 0, 0, 4, r4);
            std::printf("(b) gray_to_rgb=%d grey->rgb = (%.2f %.2f %.2f) | rgb file 4ch (fill .75) alpha=%.2f\n", g, r[0], r[1], r[2], r4[3]);
            TextureSystem::destroy(ts);
        }
    }
    // (c) private TS sharing the process-wide ImageCache
    {
        ImageCache *shared = ImageCache::create(true);
        shared->attribute("max_memory_MB", 2048.0f);
        TextureSystem *ts = TextureSystem::create(false, shared);
        ts->attribute("flip_t", 1);
        TextureSystem *tsShared = TextureSystem::create(true);
        int flipPriv = -1, flipShared = -1; float mem = 0;
        ts->getattribute("flip_t", flipPriv);
        tsShared->getattribute("flip_t", flipShared);
        ts->getattribute("max_memory_MB", mem);
        std::printf("(c) private TS on shared IC: imagecache()==shared %d, flip_t private=%d shared=%d, max_memory_MB via TS=%.0f\n",
                    ts->imagecache() == shared, flipPriv, flipShared, mem);
        TextureSystem::destroy(ts);
        TextureSystem::destroy(tsShared);
        ImageCache::destroy(shared);
    }
    // (d) thread scaling on big.tx (made by test.exe)
    {
        TextureSystem *ts = TextureSystem::create(false);
        ts->attribute("max_memory_MB", 4096.f);
        ustring name(D + "big.tx");
        const int N = 1000000;
        std::vector<float> s(N), t(N);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> U(0.f, 1.f);
        for (int i = 0; i < N; ++i) { s[i] = U(rng); t[i] = U(rng); }
        TextureSystem::TextureHandle *h = ts->get_texture_handle(name);
        auto run = [&](int nthreads) {
            std::atomic<double> sink{0};
            auto t0 = std::chrono::steady_clock::now();
            std::vector<std::thread> th;
            for (int k = 0; k < nthreads; ++k)
                th.emplace_back([&, k] {
                    TextureSystem::Perthread *pt = ts->get_perthread_info();
                    TextureOpt opt;
                    float r[3], acc = 0;
                    const float d = 1.f / 4096;
                    for (int i = 0; i < N; ++i) {
                        int j = (i + k * 7919) % N;
                        ts->texture(h, pt, opt, s[j], t[j], d, 0, 0, d, 3, r);
                        acc += r[0];
                    }
                    double cur = sink.load();
                    while (!sink.compare_exchange_weak(cur, cur + acc)) {}
                });
            for (auto &x : th) x.join();
            auto t1 = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(t1 - t0).count();
            return double(N) * nthreads / sec / 1e6;
        };
        run(1); // warm
        unsigned hw = std::thread::hardware_concurrency();
        for (int n : {1, 2, 4, 8, 16, 32}) {
            if (unsigned(n) > hw) break;
            std::printf("(d) %2d threads: %.1f M lookups/s\n", n, run(n));
        }
        TextureSystem::destroy(ts);
    }
    return 0;
}
