// Fourth probe: cost of deterministic vs stochastic (rnd) filtering, and the
// mean of stochastic lookups vs the deterministic value on the MIP-level texture.
#include <OpenImageIO/texture.h>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
using namespace OIIO;
static std::string D = "data/";
using clk = std::chrono::steady_clock;

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    const int N = 2000000;
    std::vector<float> s(N), t(N), rnd(N);
    for (int i = 0; i < N; ++i) { s[i] = U(rng); t[i] = U(rng); rnd[i] = U(rng); }
    for (int stoch : {0, 1, 3}) {
        TextureSystem *ts = TextureSystem::create(false);
        ts->attribute("max_memory_MB", 4096.f);
        ts->attribute("stochastic", stoch);
        ustring name(D + "big.tx");
        TextureSystem::Perthread *pt = ts->get_perthread_info();
        TextureSystem::TextureHandle *h = ts->get_texture_handle(name, pt);
        float r[3], acc = 0;
        for (float fp : {1.5f, 6.f}) {
            const float d = fp / 4096;
            for (int pass = 0; pass < 2; ++pass) { // pass 0 warms the cache
                auto t0 = clk::now();
                for (int i = 0; i < N; ++i) {
                    TextureOpt opt;
                    opt.rnd = stoch ? rnd[i] : -1.0f;
                    // anisotropic footprint 4:1
                    ts->texture(h, pt, opt, s[i], t[i], d * 4, 0, 0, d, 3, r);
                    acc += r[0];
                }
                auto t1 = clk::now();
                if (pass == 1)
                    std::printf("stochastic=%d footprint %.1fx%.1f texels: %.0f ns/lookup\n", stoch, fp * 4, fp,
                                std::chrono::duration<double, std::nano>(t1 - t0).count() / N);
            }
        }
        // unbiasedness on the level texture: deterministic blend vs mean of stochastic
        float det = 0, mean = 0;
        TextureOpt o0;
        ts->texture(ustring(D + "mips.exr"), o0, 0.37f, 0.61f, 3.f / 1024, 0, 0, 3.f / 1024, 1, &det);
        for (int i = 0; i < 100000; ++i) {
            TextureOpt o; o.rnd = (i + 0.5f) / 100000; float v;
            ts->texture(ustring(D + "mips.exr"), o, 0.37f, 0.61f, 3.f / 1024, 0, 0, 3.f / 1024, 1, &v);
            mean += v / 100000;
        }
        std::printf("stochastic=%d level texture, 3-texel footprint: deterministic %.4f, mean of rnd lookups %.4f (acc %g)\n", stoch, det, mean, acc);
        TextureSystem::destroy(ts);
    }
    return 0;
}
