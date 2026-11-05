// Validates bsdf_stats.h: correct samplers must pass, a sampler with a
// deliberately wrong pdf must fail. GGX: visible-normal sampling (Heitz 2018),
// height-correlated Smith (MaterialX mx_ggx_smith_G2), F = 1.
#include "bsdf_stats.h"
#include <chrono>
#include <cstdio>

using namespace nova::test;
static const double kPi = 3.14159265358979323846;

static double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 norm(V3 a) { double l = std::sqrt(dot(a, a)); return {a.x / l, a.y / l, a.z / l}; }

LobeUnderTest lambert(bool wrongPdf = false) {
    LobeUnderTest l;
    l.sample = [wrongPdf](V3 wo, V3 u, V3 &wi, double &pdf, double &w, bool &delta) {
        delta = false;
        if (wo.z <= 0) return false;
        // wrong: uniform hemisphere sampling while pdf() claims cosine
        if (wrongPdf) {
            const double z = u.x, r = std::sqrt(std::max(0.0, 1 - z * z));
            wi = {r * std::cos(2 * kPi * u.y), r * std::sin(2 * kPi * u.y), z};
        } else {
            const double r = std::sqrt(u.x);
            wi = {r * std::cos(2 * kPi * u.y), r * std::sin(2 * kPi * u.y), std::sqrt(std::max(0.0, 1 - u.x))};
        }
        pdf = wi.z / kPi;
        w = 1.0;
        return pdf > 0;
    };
    l.pdf = [](V3 wo, V3 wi) { return (wo.z > 0 && wi.z > 0) ? wi.z / kPi : 0.0; };
    l.evalCos = [](V3 wo, V3 wi) { return (wo.z > 0 && wi.z > 0) ? wi.z / kPi : 0.0; };
    return l;
}

struct Ggx {
    double ax, ay;
    double D(V3 h) const {
        if (h.z <= 0) return 0;
        const double e = (h.x * h.x) / (ax * ax) + (h.y * h.y) / (ay * ay) + h.z * h.z;
        return 1.0 / (kPi * ax * ay * e * e);
    }
    double Lambda(V3 w) const {
        const double a2 = (ax * ax * w.x * w.x + ay * ay * w.y * w.y) / (w.z * w.z);
        return 0.5 * (std::sqrt(1 + a2) - 1);
    }
    double G1(V3 w) const { return 1 / (1 + Lambda(w)); }
    double G2(V3 o, V3 i) const { return 1 / (1 + Lambda(o) + Lambda(i)); }
    double Dv(V3 wo, V3 h) const { return G1(wo) * std::max(0.0, dot(wo, h)) * D(h) / wo.z; }
    V3 sampleVndf(V3 wo, double u1, double u2) const {
        V3 vh = norm({ax * wo.x, ay * wo.y, wo.z});
        const double lensq = vh.x * vh.x + vh.y * vh.y;
        V3 T1 = lensq > 0 ? V3{-vh.y / std::sqrt(lensq), vh.x / std::sqrt(lensq), 0} : V3{1, 0, 0};
        V3 T2 = {vh.y * T1.z - vh.z * T1.y, vh.z * T1.x - vh.x * T1.z, vh.x * T1.y - vh.y * T1.x};
        const double r = std::sqrt(u1), phi = 2 * kPi * u2;
        double t1 = r * std::cos(phi), t2 = r * std::sin(phi);
        const double s = 0.5 * (1 + vh.z);
        t2 = (1 - s) * std::sqrt(std::max(0.0, 1 - t1 * t1)) + s * t2;
        const double t3 = std::sqrt(std::max(0.0, 1 - t1 * t1 - t2 * t2));
        V3 nh = {t1 * T1.x + t2 * T2.x + t3 * vh.x, t1 * T1.y + t2 * T2.y + t3 * vh.y, t1 * T1.z + t2 * T2.z + t3 * vh.z};
        return norm({ax * nh.x, ay * nh.y, std::max(1e-9, nh.z)});
    }
};

LobeUnderTest ggx(double ax, double ay, double pdfScaleBug = 1.0) {
    LobeUnderTest l;
    Ggx g{ax, ay};
    l.sample = [g, pdfScaleBug](V3 wo, V3 u, V3 &wi, double &pdf, double &w, bool &delta) {
        delta = false;
        if (wo.z <= 0) return false;
        V3 h = g.sampleVndf(wo, u.x, u.y);
        const double oh = dot(wo, h);
        wi = {2 * oh * h.x - wo.x, 2 * oh * h.y - wo.y, 2 * oh * h.z - wo.z};
        if (wi.z <= 0) return false;
        pdf = g.Dv(wo, h) / (4 * oh);
        w = g.G2(wo, wi) / g.G1(wo); // F = 1
        (void)pdfScaleBug;
        return pdf > 0;
    };
    l.pdf = [g, pdfScaleBug](V3 wo, V3 wi) {
        if (wo.z <= 0 || wi.z <= 0) return 0.0;
        V3 h = norm({wo.x + wi.x, wo.y + wi.y, wo.z + wi.z});
        // bug variant: Jacobian with dot(wi, n) instead of dot(wo, h)
        const double jac = pdfScaleBug != 1.0 ? 4 * std::max(1e-9, wi.z) : 4 * dot(wo, h);
        return g.Dv(wo, h) / jac;
    };
    l.evalCos = [g](V3 wo, V3 wi) {
        if (wo.z <= 0 || wi.z <= 0) return 0.0;
        V3 h = norm({wo.x + wi.x, wo.y + wi.y, wo.z + wi.z});
        return g.D(h) * g.G2(wo, wi) / (4 * wo.z);
    };
    return l;
}

int main() {
    Pcg32 rng(42);
    struct Case { const char *name; LobeUnderTest lobe; bool expectPass; };
    std::vector<Case> cases = {
        {"lambert", lambert(), true},
        {"lambert, wrong pdf", lambert(true), false},
        {"ggx a=0.5", ggx(0.5, 0.5), true},
        {"ggx a=0.1", ggx(0.1, 0.1), true},
        {"ggx aniso 0.6/0.15", ggx(0.6, 0.15), true},
        {"ggx a=0.3, wrong jacobian", ggx(0.3, 0.3, 2.0), false},
    };
    int failures = 0;
    for (auto &c : cases) {
        for (int k = 0; k < 3; ++k) {
            const double ct = 0.15 + 0.8 * rng.uniform(), st = std::sqrt(1 - ct * ct), ph = 2 * kPi * rng.uniform();
            V3 wo{st * std::cos(ph), st * std::sin(ph), ct};
            auto t0 = std::chrono::steady_clock::now();
            Chi2Result r = chi2Test(c.lobe, wo, rng, 1 << 20, 48, 5.0, 0.01, 3);
            AlbedoPair a = albedoEstimates(c.lobe, wo, rng, 1 << 18);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const bool ok = r.pass == c.expectPass;
            failures += !ok;
            std::printf("%-28s cos %.3f  chi2 %s p=%.3g dof=%d mass=%.5f | E imp %.4f+-%.4f uni %.4f+-%.4f | %.0f ms %s\n",
                        c.name, ct, r.pass ? "pass" : "FAIL", r.pValue, r.dof, r.pdfMass, a.importance,
                        a.importanceErr, a.uniform, a.uniformErr, ms, ok ? "" : "<-- unexpected");
        }
    }
    std::printf("%d unexpected results\n", failures);
    return failures ? 1 : 0;
}
