// Throwaway numerical verification of bsdf_sketch.h
#include "bsdf_sketch.h"

#include <cstdio>
#include <functional>
#include <random>
#include <vector>

using namespace sk;

static V3 dirFrom(float mu, float phi) {
    const float s = std::sqrt(std::fmax(0.0f, 1.0f - mu * mu));
    return {s * std::cos(phi), s * std::sin(phi), mu};
}
// midpoint quadrature over the sphere (or upper hemisphere) in (mu, phi)
static double integrate(const std::function<double(V3)> &fn, int nmu, int nphi, bool upperOnly) {
    const double mu0 = upperOnly ? 0.0 : -1.0;
    const double dmu = (1.0 - mu0) / nmu, dphi = 2.0 * kPi / nphi;
    double sum = 0.0;
    for (int i = 0; i < nmu; ++i) {
        const float mu = float(mu0 + (i + 0.5) * dmu);
        for (int j = 0; j < nphi; ++j) sum += fn(dirFrom(mu, float((j + 0.5) * dphi)));
    }
    return sum * dmu * dphi;
}

struct Rng {
    std::mt19937_64 g{12345};
    std::uniform_real_distribution<float> d{0.0f, 1.0f};
    float operator()() { float x = d(g); return x >= 1.0f ? 0.99999994f : x; }
};

// chi^2 of sampled directions against pdf (pbrt style, pooled cells)
static double chi2(const std::function<bool(Rng &, V3 &)> &sampler, const std::function<double(V3)> &pdf,
                   int N = 1000000, int nz = 20, int nphi = 40) {
    Rng rng;
    std::vector<double> obs(nz * nphi, 0.0), expv(nz * nphi, 0.0);
    for (int i = 0; i < N; ++i) {
        V3 w;
        if (!sampler(rng, w)) continue;
        float phi = std::atan2(w.y, w.x);
        if (phi < 0) phi += 2 * kPi;
        int iz = std::min(nz - 1, std::max(0, int((w.z + 1.0f) * 0.5f * nz)));
        int ip = std::min(nphi - 1, std::max(0, int(phi / (2 * kPi) * nphi)));
        obs[iz * nphi + ip] += 1;
    }
    const int sub = 8;
    for (int iz = 0; iz < nz; ++iz)
        for (int ip = 0; ip < nphi; ++ip) {
            double s = 0;
            for (int a = 0; a < sub; ++a)
                for (int b = 0; b < sub; ++b) {
                    float z = -1.0f + 2.0f * (iz + (a + 0.5f) / sub) / nz;
                    float phi = 2 * kPi * (ip + (b + 0.5f) / sub) / nphi;
                    s += pdf(dirFrom(z, phi));
                }
            expv[iz * nphi + ip] = s / (sub * sub) * (2.0 / nz) * (2 * kPi / nphi) * N;
        }
    // pool
    std::vector<int> idx(nz * nphi);
    for (int i = 0; i < nz * nphi; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return expv[a] < expv[b]; });
    double pO = 0, pE = 0, c2 = 0;
    int dof = 0;
    for (int i : idx) {
        if (expv[i] == 0) {
            if (obs[i] > 0) return 1e9; // samples where pdf == 0
            continue;
        }
        if (expv[i] < 5) { pO += obs[i]; pE += expv[i]; }
        else { c2 += sqr(float(obs[i] - expv[i])) / expv[i]; ++dof; }
    }
    if (pE > 0) { c2 += (pO - pE) * (pO - pE) / pE; ++dof; }
    dof -= 1;
    // Wilson-Hilferty p-value
    const double k = dof;
    const double zz = (std::pow(c2 / k, 1.0 / 3.0) - (1 - 2 / (9 * k))) / std::sqrt(2 / (9 * k));
    return 0.5 * std::erfc(zz / std::sqrt(2.0)); // p-value
}

int main() {
    std::printf("== GGX normalization: int D(m) m.z dm (expect 1)\n");
    for (auto [ax, ay] : {std::pair{0.1f, 0.1f}, {0.5f, 0.5f}, {1.0f, 1.0f}, {0.1f, 0.6f}}) {
        Ggx g{ax, ay};
        double v = integrate([&](V3 m) { return g.D(m) * m.z; }, 4000, 400, true);
        std::printf("  ax=%.2f ay=%.2f  %.5f\n", ax, ay, v);
    }

    std::printf("== VNDF normalization int D_wo(m) dm (expect 1), full and bounded reflect pdf mass over H+\n");
    for (auto [ax, ay] : {std::pair{0.2f, 0.2f}, {0.8f, 0.8f}, {0.3f, 0.9f}}) {
        Ggx g{ax, ay};
        for (float mu : {0.1f, 0.5f, 0.95f}) {
            V3 wo = dirFrom(mu, 0.7f);
            double v = integrate([&](V3 m) { return g.pdfVisible(wo, m); }, 3000, 400, true);
            double rf = integrate([&](V3 wi) { return g.pdfReflect(wo, wi, false); }, 3000, 400, true);
            double rb = integrate([&](V3 wi) { return g.pdfReflect(wo, wi, true); }, 3000, 400, true);
            // MC: fraction of samples reflecting above the horizon
            Rng rng;
            int okF = 0, okB = 0, N = 200000;
            for (int i = 0; i < N; ++i) {
                V2 u{rng(), rng()};
                okF += reflect(wo, g.sampleVisible(wo, u, false)).z > 0;
                okB += reflect(wo, g.sampleVisible(wo, u, true)).z > 0;
            }
            std::printf("  a=(%.1f,%.1f) mu=%.2f  intVNDF=%.4f  massH+ full=%.4f (MC %.4f) bounded=%.4f (MC %.4f)\n",
                        ax, ay, mu, v, rf, okF / double(N), rb, okB / double(N));
        }
    }

    std::printf("== chi2 p-values (>0.01 = pass)\n");
    {
        Ggx g{0.3f, 0.6f};
        V3 wo = dirFrom(0.4f, 1.0f);
        for (bool bounded : {false, true}) {
            double p = chi2(
                [&](Rng &r, V3 &w) { w = reflect(wo, g.sampleVisible(wo, {r(), r()}, bounded)); return w.z > 0; },
                [&](V3 wi) { return wi.z > 0 ? g.pdfReflect(wo, wi, bounded) : 0.0; });
            std::printf("  GGX reflection (aniso) bounded=%d: p=%.3f\n", bounded, p);
        }
    }
    for (float eta : {1.5f, 1.0f / 1.5f}) {
        for (float alpha : {0.1f, 0.5f}) {
            RoughDielectric d{{alpha, alpha}, eta};
            V3 wo = dirFrom(0.6f, 0.3f);
            double p = chi2(
                [&](Rng &r, V3 &w) {
                    DielectricSample s;
                    if (!d.sample(wo, {r(), r()}, r(), s)) return false;
                    w = s.wi; return true;
                },
                [&](V3 wi) { float pdf; d.eval(wo, wi, &pdf); return pdf; }, 1000000, 40, 40);
            std::printf("  rough dielectric eta=%.3f alpha=%.1f: p=%.3f\n", eta, alpha, p);
        }
    }
    for (float r : {0.5f, 1.0f}) {
        V3 wo = dirFrom(0.15f, 0.0f);
        double p = chi2([&](Rng &rg, V3 &w) { w = eonSample(wo, r, rg(), rg()); return true; },
                        [&](V3 wi) { return eonPdf(wo, wi, r); });
        std::printf("  EON CLTC+uniform r=%.1f mu_o=0.15: p=%.3f\n", r, p);
        double pz = chi2([&](Rng &rg, V3 &w) { w = zeltnerSample(wo, r, {rg(), rg()}); return true; },
                         [&](V3 wi) { return zeltnerPdf(wo, wi, r); });
        std::printf("  Zeltner LTC sheen r=%.1f: p=%.3f\n", r, pz);
    }

    std::printf("== rough dielectric single-scatter albedo E_R+E_T (white furnace of 1 interface), MC vs quadrature\n");
    for (float eta : {1.5f, 1.0f / 1.5f, 1.33f}) {
        for (float alpha : {0.05f, 0.3f, 0.7f, 1.0f}) {
            for (float mu : {0.2f, 0.7f, 1.0f}) {
                RoughDielectric d{{alpha, alpha}, eta};
                V3 wo = dirFrom(mu, 0.0f);
                Rng rng;
                double sumR = 0, sumT = 0;
                int N = 400000;
                for (int i = 0; i < N; ++i) {
                    DielectricSample s;
                    if (!d.sample(wo, {rng(), rng()}, rng(), s)) continue;
                    double w = s.f * std::fabs(s.wi.z) / s.pdf;
                    // radiance mode: undo 1/eta^2 to report energy (importance) albedo
                    if (!s.reflect) w *= (wo.z > 0 ? sqr(eta) : 1.0 / sqr(eta));
                    (s.reflect ? sumR : sumT) += w;
                }
                std::printf("  eta=%.3f a=%.2f mu=%.1f  E_R=%.4f E_T=%.4f  sum=%.4f\n", eta, alpha, mu, sumR / N,
                            sumT / N, (sumR + sumT) / N);
            }
        }
    }

    std::printf("== BTDF reciprocity (radiance mode): f(wo,wi) * eta_o^2 vs f(wi,wo) * eta_i^2\n");
    {
        RoughDielectric d{{0.4f, 0.4f}, 1.5f};
        V3 wo = dirFrom(0.7f, 0.2f), wi = dirFrom(-0.5f, 3.0f);
        float p1, p2;
        float a = d.eval(wo, wi, &p1), b = d.eval(wi, wo, &p2);
        // wo outside (n=1), wi inside (n=1.5)
        std::printf("  f(wo,wi)=%.6f f(wi,wo)=%.6f  ratio=%.4f (expect (n_i/n_o)^2 relation)\n", a, b, a / b);
    }

    std::printf("== conductor E(mu, alpha) F=1 vs MaterialX analytic fit (F0=1,F90=1) and with F0=0.04\n");
    {
        double maxErr = 0, maxErr04 = 0;
        for (float alpha : {0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f})
            for (float mu : {0.05f, 0.2f, 0.5f, 0.8f, 1.0f}) {
                Ggx g{alpha, alpha};
                V3 wo = dirFrom(mu, 0.0f);
                Rng rng;
                double e1 = 0, e04 = 0;
                int N = 400000;
                for (int i = 0; i < N; ++i) {
                    V3 m = g.sampleVisible(wo, {rng(), rng()});
                    V3 wi = reflect(wo, m);
                    if (wi.z <= 0) continue;
                    double w = g.G2(wo, wi) / g.G1(wo); // f cos / pdf for F = 1
                    e1 += w;
                    float x = 1 - dot(wo, m), x5 = x * x * x * x * x;
                    e04 += w * (0.04f + 0.96f * x5);
                }
                e1 /= N; e04 /= N;
                V2 AB = mxGgxDirAlbedoAB(mu, alpha);
                double fit1 = AB.x + AB.y, fit04 = 0.04 * AB.x + AB.y;
                maxErr = std::max(maxErr, std::fabs(fit1 - e1));
                maxErr04 = std::max(maxErr04, std::fabs(fit04 - e04));
                if (alpha == 0.5f || alpha == 1.0f)
                    std::printf("  a=%.2f mu=%.2f E=%.4f fit=%.4f | E(F0=.04)=%.4f fit=%.4f\n", alpha, mu, e1, fit1, e04, fit04);
            }
        std::printf("  max |fit-E| F=1: %.4f   F0=0.04 Schlick: %.4f\n", maxErr, maxErr04);
    }

    std::printf("== EON: albedo for rho=1 (expect 1), CLTC pdf & mixture pdf integrals (expect 1)\n");
    for (float r : {0.25f, 0.5f, 1.0f})
        for (float mu : {0.05f, 0.5f, 1.0f}) {
            V3 wo = dirFrom(mu, 0.0f);
            double E = integrate([&](V3 wi) { return fEon(1.0f, r, wi, wo, true) * wi.z; }, 2000, 400, true);
            double Ea = integrate([&](V3 wi) { return fEon(1.0f, r, wi, wo, false) * wi.z; }, 2000, 400, true);
            double Ic = integrate([&](V3 wi) { return cltcPdf(wo, wi, r); }, 2000, 400, true);
            double Im = integrate([&](V3 wi) { return eonPdf(wo, wi, r); }, 2000, 400, true);
            // variance of throughput weight: cosine vs EON sampling
            Rng rng;
            double m1 = 0, m2 = 0, c1 = 0, c2 = 0;
            int N = 200000;
            for (int i = 0; i < N; ++i) {
                float u1 = rng(), u2 = rng();
                V3 wi = eonSample(wo, r, u1, u2);
                double w = wi.z > 0 ? fEon(1, r, wi, wo, false) * wi.z / eonPdf(wo, wi, r) : 0;
                m1 += w; m2 += w * w;
                float rr = std::sqrt(u1), ph = 2 * kPi * u2;
                V3 wc{rr * std::cos(ph), rr * std::sin(ph), std::sqrt(std::fmax(0.f, 1 - u1))};
                double wcw = wc.z > 0 ? fEon(1, r, wc, wo, false) * kPi : 0;
                c1 += wcw; c2 += wcw * wcw;
            }
            std::printf("  r=%.2f mu=%.2f E_exact=%.4f E_approx=%.4f intCLTC=%.4f intMix=%.4f | var EON-samp=%.4f cos-samp=%.4f\n",
                        r, mu, E, Ea, Ic, Im, m2 / N - sqr(float(m1 / N)), c2 / N - sqr(float(c1 / N)));
        }

    std::printf("== Zeltner LTC pdf integral and albedo fit range\n");
    for (float r : {0.1f, 0.5f, 1.0f})
        for (float mu : {0.1f, 0.5f, 1.0f}) {
            V3 wo = dirFrom(mu, 0.0f);
            double I = integrate([&](V3 wi) { return zeltnerPdf(wo, wi, r); }, 2000, 400, true);
            std::printf("  r=%.1f mu=%.1f intPdf=%.4f R(mu,r)=%.4f\n", r, mu, I, zeltnerAlbedo(mu, r));
        }

    std::printf("== Charlie sheen albedo: MaterialX form (quadrature) vs MaterialX fit; original form w/ G\n");
    for (float r : {0.1f, 0.3f, 0.6f, 1.0f})
        for (float mu : {0.1f, 0.5f, 1.0f}) {
            V3 wo = dirFrom(mu, 0.0f);
            double Emx = integrate([&](V3 wi) { return charlieBrdfMx(wo, wi, r) * wi.z; }, 2000, 400, true);
            double Eog = integrate([&](V3 wi) { return charlieBrdf(wo, wi, r, false) * wi.z; }, 2000, 400, true);
            double Eos = integrate([&](V3 wi) { return charlieBrdf(wo, wi, r, true) * wi.z; }, 2000, 400, true);
            std::printf("  r=%.1f mu=%.1f  E_mx=%.4f fit=%.4f | E_orig=%.4f E_orig_soft=%.4f\n", r, mu, Emx,
                        charlieAlbedoMx(mu, r), Eog, Eos);
        }

    std::printf("== F82 tint checks\n");
    for (float F0 : {0.0f, 0.5f, 0.95f})
        for (float tint : {0.0f, 0.5f, 1.0f}) {
            float mb = 1.0f / 7.0f;
            float schlick = F0 + (1 - F0) * std::pow(1 - mb, 5.0f);
            double avgQ = 0, minF = 1;
            int n = 20000;
            for (int i = 0; i < n; ++i) {
                float mu = (i + 0.5f) / n;
                float raw = lerp(F0, 1, std::pow(1 - mu, 5.0f)) -
                            lerp(F0, 1, std::pow(1 - mb, 5.0f)) * (1 - tint) / (mb * std::pow(1 - mb, 6.0f)) * mu *
                                std::pow(1 - mu, 6.0f);
                minF = std::min(minF, double(raw));
                avgQ += 2 * mu * raw / n;
            }
            std::printf("  F0=%.2f tint=%.1f  F(1/7)=%.4f tint*Schlick=%.4f  Favg quad=%.4f formula=%.4f  min raw F=%.4f\n",
                        F0, tint, fresnelF82(mb, F0, tint, 1, 5), tint * schlick, avgQ, fresnelF82Avg(F0, tint, 1, 5), minF);
        }

    std::printf("== average dielectric Fresnel: quadrature vs Kulla-Conty fit\n");
    for (float eta : {1.1f, 1.33f, 1.5f, 2.0f, 3.0f, 1 / 1.33f, 1 / 1.5f, 1 / 2.0f}) {
        double avg = 0;
        int n = 100000;
        for (int i = 0; i < n; ++i) {
            float mu = (i + 0.5f) / n, ct;
            avg += 2 * mu * fresnelDielectric(mu, eta, ct) / n;
        }
        std::printf("  eta=%.3f Favg=%.5f fit=%.5f\n", eta, avg, fresnelDielectricAvg(eta));
    }

    std::printf("== Belcour sensitivity at OPD=0 (XYZ) and its sRGB image\n");
    {
        double X = 5.4856e-13 * std::sqrt(2 * kPi * 4.3278e9) + 9.7470e-14 * std::sqrt(2 * kPi * 4.5282e9);
        double Y = 4.4201e-13 * std::sqrt(2 * kPi * 9.3046e9), Z = 5.2481e-13 * std::sqrt(2 * kPi * 6.6121e9);
        X /= 1.0685e-7; Y /= 1.0685e-7; Z /= 1.0685e-7;
        double r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z, g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z,
               b = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
        double cr = 2.3706743 * X - 0.9000405 * Y - 0.4706338 * Z, cg = -0.5138850 * X + 1.4253036 * Y + 0.0885814 * Z,
               cb = 0.0052982 * X - 0.0146949 * Y + 1.0093968 * Z;
        std::printf("  XYZ=(%.4f %.4f %.4f) -> sRGB(D65 matrix)=(%.4f %.4f %.4f), CIE-RGB(E)=(%.4f %.4f %.4f)\n", X, Y, Z,
                    r, g, b, cr, cg, cb);
    }
    return 0;
}
