// Karma probe cross-check: dielectric specular albedo at mu = 1
#include "bsdf_sketch.h"
#include <cstdio>
#include <random>
using namespace sk;
int main() {
    std::mt19937_64 g(7);
    std::uniform_real_distribution<float> U(0.0f, 0.99999f);
    for (float alpha : {0.09f, 0.25f, 0.64f}) {
        Ggx gg{alpha, alpha};
        V3 wo{0, 0, 1};
        double eF = 0, e1 = 0;
        const int N = 4000000;
        for (int i = 0; i < N; ++i) {
            V3 m = gg.sampleVisible(wo, {U(g), U(g)});
            V3 wi = reflect(wo, m);
            if (wi.z <= 0) continue;
            double w = gg.G2(wo, wi) / gg.G1(wo);
            float ct;
            eF += w * fresnelDielectric(dot(wo, m), 1.5f, ct);
            e1 += w;
        }
        eF /= N; e1 /= N;
        V2 ab = mxGgxDirAlbedoAB(1.0f, alpha);
        std::printf("alpha=%.2f  E_ss(F exact)=%.5f  E_ss(F=1)=%.5f  ratio=%.5f  | mx fit E11=%.5f -> E_ss/fitE11=%.5f\n",
                    alpha, eF, e1, eF / e1, ab.x + ab.y, eF / (ab.x + ab.y));
    }
}
