// M9 research prototype (docs/research/m9/curves-hair.md, section 3): a
// stand-alone implementation of the Chiang et al. 2016 hair BSDF (the model of
// pbrt-v4's HairBxDF and of Karma Hair), written from the papers' equations:
//   - longitudinal Mp: d'Eon et al. 2011 (energy-normalised, with the
//     log-Bessel form for small variances), lobe variances v_TT = v/4,
//     v_TRT = v_TRRT+ = 4v (Chiang 2016 sec. 4.1, Marschner 2003);
//   - azimuthal Np: trimmed logistic around Phi(p) = 2p gamma_t - 2 gamma_o + p pi;
//   - attenuation Ap from the dielectric Fresnel and the absorption through
//     the cortex, TRRT+ as a geometric series;
//   - the roughness maps beta_m -> v and beta_n -> s and the colour inversion
//     (Chiang 2016, eqs. 7, 8, 9);
//   - importance sampling: lobe by Ap, theta by the d'Eon sampler, phi by the
//     trimmed logistic; pdf = sum_p (A_p / sum A) Mp Np.
// Convention: local frame x = curve tangent (root -> tip), y/z span the normal
// plane, sin(theta) = w.x, phi = atan2(w.z, w.y); h in [-1, 1] across the
// width. "fcos" is f |cos| in solid angle measure: sum_p Mp Ap Np (it
// integrates to sum_p Ap = 1 for sigma_a = 0).
//
// Build (x64 Native Tools prompt of VS 2022 Build Tools):
//   cl /nologo /O2 /std:c++20 /EHsc docs\research\m9\probes\ch_hair_proto.cpp /Fe:<scratch>\ch_hair_proto.exe
// Run:
//   ch_hair_proto tests                 furnace, sampling consistency, chi-square
//   ch_hair_proto predict               Karma calibration scenes (curves-hair.md 4)
//   ch_hair_proto invert <thetaO_deg> <beta_m> <beta_n> <albedo> ...   sigma_a per albedo (furnace inversion)
//   ch_hair_proto whatif <v_R> <s> <alpha_deg> <eta> [sigma_a]          reflection-side furnace energy
//   ch_hair_proto profile <light 0-5> <beta_m> <beta_n> <alpha_deg> <sa_r> <sa_g> <sa_b>  f|cos| across the fibre
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
using RGB = std::array<double, 3>;

struct V3 {
    double x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(double s, V3 a) { return {s * a.x, s * a.y, s * a.z}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 normalize(V3 a) { return (1.0 / std::sqrt(dot(a, a))) * a; }
double sq(double x) { return x * x; }
double safeSqrt(double x) { return std::sqrt(std::fmax(x, 0.0)); }
double safeAsin(double x) { return std::asin(std::fmin(std::fmax(x, -1.0), 1.0)); }

// Modified Bessel function I0 (power series) and its logarithm (asymptotic
// for large x). pbrt's 10-term series under-estimates I0 by about 2 % near
// x = 12, where the asymptotic form takes over, which makes the furnace lose
// up to 0.4 %; CH_I0_TERMS=10 reproduces that, the default 24 terms do not.
int g_i0Terms = 24;
bool g_dropR = false;  // CH_DROP_R=1: A_R = 0 (what-if for Karma's MaterialX hair)
bool g_tiltWi = false; // CH_TILT_WI=1: tilt theta_i instead of theta_o in eval()
double besselI0(double x) {
    double val = 0, term = 1;
    const double q = x * x / 4;
    for (int i = 0; i < g_i0Terms; ++i) {
        if (i > 0)
            term *= q / (double(i) * i);
        val += term;
    }
    return val;
}
double logI0(double x) {
    if (x > 12)
        return x + 0.5 * (-std::log(2 * kPi) + std::log(1 / x) + 1 / (8 * x));
    return std::log(besselI0(x));
}

// Unpolarised dielectric Fresnel reflectance (cosI >= 0, eta = n_t / n_i).
double fresnel(double cosI, double eta) {
    const double sin2T = (1 - cosI * cosI) / (eta * eta);
    if (sin2T >= 1)
        return 1;
    const double cosT = safeSqrt(1 - sin2T);
    const double rs = (cosI - eta * cosT) / (cosI + eta * cosT);
    const double rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5 * (rs * rs + rp * rp);
}

// d'Eon 2011 longitudinal scattering (normalised over the sphere with the cos theta measure).
double Mp(double cosI, double cosO, double sinI, double sinO, double v) {
    const double a = cosI * cosO / v, b = sinI * sinO / v;
    if (v <= 0.1)
        return std::exp(logI0(a) - b - 1 / v + 0.6931 + std::log(1 / (2 * v)));
    return std::exp(-b) * besselI0(a) / (std::sinh(1 / v) * 2 * v);
}

double logistic(double x, double s) {
    x = std::fabs(x);
    return std::exp(-x / s) / (s * sq(1 + std::exp(-x / s)));
}
double logisticCDF(double x, double s) { return 1 / (1 + std::exp(-x / s)); }
double trimmedLogistic(double x, double s, double a, double b) {
    return logistic(x, s) / (logisticCDF(b, s) - logisticCDF(a, s));
}
double sampleTrimmedLogistic(double u, double s, double a, double b) {
    const double k = logisticCDF(b, s) - logisticCDF(a, s);
    const double x = -s * std::log(1 / (u * k + logisticCDF(a, s)) - 1);
    return std::fmin(std::fmax(x, a), b);
}

double Phi(int p, double gammaO, double gammaT) { return 2 * p * gammaT - 2 * gammaO + p * kPi; }
double Np(double phi, int p, double s, double gammaO, double gammaT) {
    double dphi = phi - Phi(p, gammaO, gammaT);
    while (dphi > kPi)
        dphi -= 2 * kPi;
    while (dphi < -kPi)
        dphi += 2 * kPi;
    return trimmedLogistic(dphi, s, -kPi, kPi);
}

struct Hair {
    double eta = 1.55;
    RGB sigmaA{0, 0, 0};
    double v[4]{};          // longitudinal variances R, TT, TRT, TRRT+
    double s = 0;           // azimuthal logistic scale
    double sin2kAlpha[3]{}, cos2kAlpha[3]{};
    double tiltAlpha = 0;   // the cuticle tilt (radians)

    // Chiang 2016 roughness map; alpha = cuticle tilt in radians.
    static Hair make(double eta, RGB sigmaA, double betaM, double betaN, double alpha) {
        Hair h;
        h.eta = eta;
        h.sigmaA = sigmaA;
        h.v[0] = sq(0.726 * betaM + 0.812 * sq(betaM) + 3.7 * std::pow(betaM, 20));
        h.v[1] = 0.25 * h.v[0];
        h.v[2] = 4 * h.v[0];
        h.v[3] = h.v[2];
        h.s = std::sqrt(kPi / 8) * (0.265 * betaN + 1.194 * sq(betaN) + 5.372 * std::pow(betaN, 22));
        h.tiltAlpha = alpha;
        h.sin2kAlpha[0] = std::sin(alpha);
        h.cos2kAlpha[0] = safeSqrt(1 - sq(h.sin2kAlpha[0]));
        for (int i = 1; i < 3; ++i) {
            h.sin2kAlpha[i] = 2 * h.cos2kAlpha[i - 1] * h.sin2kAlpha[i - 1];
            h.cos2kAlpha[i] = sq(h.cos2kAlpha[i - 1]) - sq(h.sin2kAlpha[i - 1]);
        }
        return h;
    }
    // Chiang 2016 eq. 9: absorption from a desired colour.
    static RGB sigmaFromColor(RGB c, double betaN) {
        RGB s;
        const double d = 5.969 - 0.215 * betaN + 2.532 * sq(betaN) - 10.73 * std::pow(betaN, 3) +
                         5.574 * std::pow(betaN, 4) + 0.245 * std::pow(betaN, 5);
        for (int i = 0; i < 3; ++i)
            s[i] = sq(std::log(std::fmax(c[i], 1e-4)) / d);
        return s;
    }
    // d'Eon 2011 / pbrt: absorption from eumelanin and pheomelanin concentrations.
    static RGB sigmaFromMelanin(double eu, double pheo) {
        const RGB e{0.419, 0.697, 1.37}, p{0.187, 0.4, 1.05};
        return {eu * e[0] + pheo * p[0], eu * e[1] + pheo * p[1], eu * e[2] + pheo * p[2]};
    }

    // Attenuations of the four lobes per channel.
    void Ap(double cosThetaO, double h, RGB ap[4]) const {
        const double sinThetaO = safeSqrt(1 - cosThetaO * cosThetaO);
        const double sinThetaT = sinThetaO / eta, cosThetaT = safeSqrt(1 - sq(sinThetaT));
        const double etap = std::sqrt(eta * eta - sq(sinThetaO)) / cosThetaO;
        const double sinGammaT = h / etap, cosGammaT = safeSqrt(1 - sq(sinGammaT));
        const double cosGammaO = safeSqrt(1 - h * h);
        const double f = fresnel(cosThetaO * cosGammaO, eta);
        for (int c = 0; c < 3; ++c) {
            const double T = std::exp(-sigmaA[c] * (2 * cosGammaT / cosThetaT));
            ap[0][c] = g_dropR ? 0.0 : f; // CH_DROP_R=1: without the R lobe (what-if)
            ap[1][c] = sq(1 - f) * T;
            ap[2][c] = ap[1][c] * T * f;
            ap[3][c] = ap[2][c] * f * T / (1 - T * f);
        }
    }

    // The tilted longitudinal angles of lobe p (cuticle scales, Marschner shifts).
    void tilt(int p, double sinThetaO, double cosThetaO, double &sinOp, double &cosOp) const {
        if (p == 0) {
            sinOp = sinThetaO * cos2kAlpha[1] - cosThetaO * sin2kAlpha[1];
            cosOp = cosThetaO * cos2kAlpha[1] + sinThetaO * sin2kAlpha[1];
        } else if (p == 1) {
            sinOp = sinThetaO * cos2kAlpha[0] + cosThetaO * sin2kAlpha[0];
            cosOp = cosThetaO * cos2kAlpha[0] - sinThetaO * sin2kAlpha[0];
        } else if (p == 2) {
            sinOp = sinThetaO * cos2kAlpha[2] + cosThetaO * sin2kAlpha[2];
            cosOp = cosThetaO * cos2kAlpha[2] - sinThetaO * sin2kAlpha[2];
        } else {
            sinOp = sinThetaO;
            cosOp = cosThetaO;
        }
        cosOp = std::fabs(cosOp);
    }

    // f |cos| per lobe (lobe[p], p = 0..3) and its sum; returns the sum.
    RGB eval(V3 wo, V3 wi, double h, RGB lobe[4] = nullptr) const {
        const double sinThetaO = wo.x, cosThetaO = safeSqrt(1 - sq(sinThetaO)), phiO = std::atan2(wo.z, wo.y);
        const double sinThetaI = wi.x, cosThetaI = safeSqrt(1 - sq(sinThetaI)), phiI = std::atan2(wi.z, wi.y);
        const double etap = std::sqrt(eta * eta - sq(sinThetaO)) / cosThetaO;
        const double gammaO = safeAsin(h), gammaT = safeAsin(h / etap);
        RGB ap[4];
        Ap(cosThetaO, h, ap);
        const double phi = phiI - phiO;
        RGB sum{0, 0, 0};
        for (int p = 0; p < 4; ++p) {
            double sinOp, cosOp, m;
            if (g_tiltWi && p < 3) {
                // Variant (CH_TILT_WI=1): the cuticle tilt rotates theta_i
                // instead of theta_o (as MaterialX's GLSL does), with the same
                // first-order lobe shift: theta_i - 2a (R), + a (TT), + 4a (TRT).
                const double k = p == 0 ? -2 : (p == 1 ? 1 : 4);
                const double sa = std::sin(k * tiltAlpha), ca = std::cos(k * tiltAlpha);
                const double sinIp = sinThetaI * ca + cosThetaI * sa;
                const double cosIp = std::fabs(cosThetaI * ca - sinThetaI * sa);
                m = Mp(cosIp, cosThetaO, sinIp, sinThetaO, v[p]);
            } else {
                tilt(p, sinThetaO, cosThetaO, sinOp, cosOp);
                m = Mp(cosThetaI, cosOp, sinThetaI, sinOp, v[p]);
            }
            const double n = p < 3 ? Np(phi, p, s, gammaO, gammaT) : 1 / (2 * kPi);
            for (int c = 0; c < 3; ++c) {
                const double val = m * ap[p][c] * n;
                sum[c] += val;
                if (lobe)
                    lobe[p][c] = val;
            }
        }
        return sum;
    }

    // Lobe selection probabilities (luminance-like average of the channels).
    void lobePdf(double cosThetaO, double h, double pdf[4]) const {
        RGB ap[4];
        Ap(cosThetaO, h, ap);
        double total = 0;
        for (int p = 0; p < 4; ++p) {
            pdf[p] = (ap[p][0] + ap[p][1] + ap[p][2]) / 3;
            total += pdf[p];
        }
        for (int p = 0; p < 4; ++p)
            pdf[p] /= total;
    }

    double pdf(V3 wo, V3 wi, double h) const {
        const double sinThetaO = wo.x, cosThetaO = safeSqrt(1 - sq(sinThetaO)), phiO = std::atan2(wo.z, wo.y);
        const double sinThetaI = wi.x, cosThetaI = safeSqrt(1 - sq(sinThetaI)), phiI = std::atan2(wi.z, wi.y);
        const double etap = std::sqrt(eta * eta - sq(sinThetaO)) / cosThetaO;
        const double gammaO = safeAsin(h), gammaT = safeAsin(h / etap);
        double ap[4];
        lobePdf(cosThetaO, h, ap);
        const double phi = phiI - phiO;
        double pdf = 0;
        for (int p = 0; p < 4; ++p) {
            double sinOp, cosOp;
            tilt(p, sinThetaO, cosThetaO, sinOp, cosOp);
            pdf += Mp(cosThetaI, cosOp, sinThetaI, sinOp, v[p]) * ap[p] *
                   (p < 3 ? Np(phi, p, s, gammaO, gammaT) : 1 / (2 * kPi));
        }
        return pdf;
    }

    // Samples wi; returns false if no direction. u: 4 uniform numbers.
    bool sample(V3 wo, double h, const double u[4], V3 &wi, double &pdfOut, RGB &weight) const {
        const double sinThetaO = wo.x, cosThetaO = safeSqrt(1 - sq(sinThetaO)), phiO = std::atan2(wo.z, wo.y);
        const double etap = std::sqrt(eta * eta - sq(sinThetaO)) / cosThetaO;
        const double gammaO = safeAsin(h), gammaT = safeAsin(h / etap);
        double ap[4];
        lobePdf(cosThetaO, h, ap);
        int p = 0;
        double uc = u[0];
        for (; p < 3; ++p) {
            if (uc < ap[p])
                break;
            uc -= ap[p];
        }
        double sinOp, cosOp;
        tilt(p, sinThetaO, cosThetaO, sinOp, cosOp);
        // d'Eon's sampler of the longitudinal lobe (around the mirrored, tilted theta_o).
        const double u1 = std::fmax(u[1], 1e-5);
        const double cosTheta = 1 + v[p] * std::log(u1 + (1 - u1) * std::exp(-2 / v[p]));
        const double sinTheta = safeSqrt(1 - sq(cosTheta));
        const double cosPhi = std::cos(2 * kPi * u[2]);
        const double sinThetaI = std::fmin(std::fmax(-cosTheta * sinOp + sinTheta * cosPhi * cosOp, -1.0), 1.0);
        const double cosThetaI = safeSqrt(1 - sq(sinThetaI));
        const double dphi = p < 3 ? Phi(p, gammaO, gammaT) + sampleTrimmedLogistic(u[3], s, -kPi, kPi)
                                  : 2 * kPi * u[3];
        const double phiI = phiO + dphi;
        wi = {sinThetaI, cosThetaI * std::cos(phiI), cosThetaI * std::sin(phiI)};
        pdfOut = pdf(wo, wi, h);
        if (!(pdfOut > 0))
            return false;
        const RGB f = eval(wo, wi, h);
        for (int c = 0; c < 3; ++c)
            weight[c] = f[c] / pdfOut;
        return true;
    }
};

// Regularised upper incomplete gamma Q(a, x) for chi-square p-values.
double gammaQ(double a, double x) {
    if (x <= 0)
        return 1;
    if (x < a + 1) { // series for P
        double sum = 1 / a, del = sum, ap = a;
        for (int n = 0; n < 1000; ++n) {
            ap += 1;
            del *= x / ap;
            sum += del;
            if (std::fabs(del) < std::fabs(sum) * 1e-14)
                break;
        }
        return 1 - sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
    }
    double b = x + 1 - a, c = 1 / 1e-300, d = 1 / b, hh = d; // continued fraction for Q
    for (int i = 1; i < 1000; ++i) {
        const double an = -i * (i - a);
        b += 2;
        d = an * d + b;
        if (std::fabs(d) < 1e-300)
            d = 1e-300;
        c = b + an / c;
        if (std::fabs(c) < 1e-300)
            c = 1e-300;
        d = 1 / d;
        const double del = d * c;
        hh *= del;
        if (std::fabs(del - 1) < 1e-14)
            break;
    }
    return std::exp(-x + a * std::log(x) - std::lgamma(a)) * hh;
}

V3 dirFromAngles(double thetaDeg, double phiDeg) { // local frame: sin(theta) = x, phi in the y-z plane
    const double t = thetaDeg * kPi / 180, p = phiDeg * kPi / 180;
    return {std::sin(t), std::cos(t) * std::cos(p), std::cos(t) * std::sin(p)};
}

double lum(const RGB &c) { return (c[0] + c[1] + c[2]) / 3; }

// Directional albedo averaged over h (orthographic view of a fibre), by BSDF sampling.
RGB albedo(const Hair &hr, double thetaODeg, int nh = 64, int ns = 4096, uint32_t seed = 1) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0, 1);
    const V3 wo = dirFromAngles(thetaODeg, 90);
    RGB acc{0, 0, 0};
    for (int ih = 0; ih < nh; ++ih) {
        const double h = -1 + (ih + 0.5) * 2.0 / nh;
        for (int k = 0; k < ns; ++k) {
            const double u[4] = {U(rng), U(rng), U(rng), U(rng)};
            V3 wi;
            double pdf;
            RGB w;
            if (hr.sample(wo, h, u, wi, pdf, w))
                for (int c = 0; c < 3; ++c)
                    acc[c] += w[c];
        }
    }
    for (int c = 0; c < 3; ++c)
        acc[c] /= double(nh) * ns;
    return acc;
}

// Directional albedo by the A_p sum alone (exact when Mp and Np are normalised).
RGB albedoAp(const Hair &hr, double thetaODeg, int nh = 4000) {
    RGB acc{0, 0, 0};
    const double cosO = std::cos(thetaODeg * kPi / 180);
    for (int ih = 0; ih < nh; ++ih) {
        const double h = -1 + (ih + 0.5) * 2.0 / nh;
        RGB ap[4];
        hr.Ap(cosO, h, ap);
        for (int p = 0; p < 4; ++p)
            for (int c = 0; c < 3; ++c)
                acc[c] += ap[p][c];
    }
    for (int c = 0; c < 3; ++c)
        acc[c] /= nh;
    return acc;
}

// Directional albedo by quadrature, averaged over h: for fixed wo and h the
// lobes separate into A_p x (integral of Mp over sin(theta_i)) x (integral of
// Np over phi), so the 2D integral over the sphere is a sum of 1D integrals.
// This is the energy test that does not depend on the sampler.
RGB albedoQuad(const Hair &hr, double thetaODeg, int nh = 64, int nz = 20000, int nphi = 20000,
               double *mintOut = nullptr, double *nintOut = nullptr) {
    const double sinO = std::sin(thetaODeg * kPi / 180), cosO = std::cos(thetaODeg * kPi / 180);
    double mint[4];
    for (int p = 0; p < 4; ++p) {
        double sinOp, cosOp;
        hr.tilt(p, sinO, cosO, sinOp, cosOp);
        double acc = 0;
        for (int i = 0; i < nz; ++i) {
            const double z = -1 + (i + 0.5) * 2.0 / nz;
            acc += Mp(safeSqrt(1 - z * z), cosOp, z, sinOp, hr.v[p]) * 2.0 / nz;
        }
        mint[p] = acc;
        if (mintOut)
            mintOut[p] = acc;
    }
    RGB out{0, 0, 0};
    const double etap = std::sqrt(hr.eta * hr.eta - sq(sinO)) / cosO;
    double nintAvg[3] = {0, 0, 0};
    for (int ih = 0; ih < nh; ++ih) {
        const double h = -1 + (ih + 0.5) * 2.0 / nh;
        const double gO = safeAsin(h), gT = safeAsin(h / etap);
        RGB ap[4];
        hr.Ap(cosO, h, ap);
        for (int p = 0; p < 4; ++p) {
            double nint = 1;
            if (p < 3) {
                nint = 0;
                for (int k = 0; k < nphi; ++k) {
                    const double phi = -kPi + (k + 0.5) * 2 * kPi / nphi;
                    nint += Np(phi, p, hr.s, gO, gT) * 2 * kPi / nphi;
                }
                nintAvg[p] += nint / nh;
            }
            for (int c = 0; c < 3; ++c)
                out[c] += ap[p][c] * mint[p] * nint / nh;
        }
    }
    if (nintOut)
        for (int p = 0; p < 3; ++p)
            nintOut[p] = nintAvg[p];
    return out;
}

int runTests() {
    std::printf("== 1. White furnace (sigma_a = 0, eta 1.55): albedo averaged over h by quadrature ==\n");
    std::printf("   (integral of Mp over the sphere per lobe R TT TRT TRRT at theta_o 60; Np integrates to 1)\n");
    std::printf("   beta_m beta_n alpha  | theta_o: 0 30 60 80 deg      | Mp integrals at 60 deg\n");
    double worst = 0;
    for (double bm : {0.1, 0.3, 0.6, 1.0})
        for (double bn : {0.1, 0.3, 1.0})
            for (double a : {0.0, 2.0}) {
                const Hair hr = Hair::make(1.55, {0, 0, 0}, bm, bn, a * kPi / 180);
                std::printf("   %.1f    %.1f    %.0f deg |", bm, bn, a);
                double mint[4] = {}, nint[3] = {};
                for (double t : {0.0, 30.0, 60.0, 80.0}) {
                    double mi[4], ni[3];
                    const RGB r = albedoQuad(hr, t, 16, 8000, 8000, mi, ni);
                    if (t == 60.0)
                        for (int p = 0; p < 4; ++p)
                            mint[p] = mi[p];
                    std::printf(" %.4f", r[0]);
                    worst = std::fmax(worst, std::fabs(r[0] - 1));
                }
                std::printf("      | %.4f %.4f %.4f %.4f\n", mint[0], mint[1], mint[2], mint[3]);
            }
    std::printf("   largest |albedo - 1|: %.4f\n", worst);

    std::printf("== 2. Sampling consistency: E[f/pdf] vs uniform-sphere integral of f, and the pdf's integral ==\n");
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> U(0, 1);
    for (double bm : {0.2, 0.5})
        for (double bn : {0.2, 0.7}) {
            const Hair hr = Hair::make(1.55, {0.2, 0.5, 1.0}, bm, bn, 2 * kPi / 180);
            for (double t : {10.0, 50.0}) {
                const double h = 0.37;
                const V3 wo = dirFromAngles(t, 90);
                const int n = 2000000;
                RGB isAcc{0, 0, 0}, uniAcc{0, 0, 0};
                double pdfInt = 0, maxRel = 0;
                for (int k = 0; k < n; ++k) {
                    const double u[4] = {U(rng), U(rng), U(rng), U(rng)};
                    V3 wi;
                    double pdf;
                    RGB w;
                    if (hr.sample(wo, h, u, wi, pdf, w)) {
                        for (int c = 0; c < 3; ++c)
                            isAcc[c] += w[c];
                        // the sampled direction's pdf must match pdf()
                        maxRel = std::fmax(maxRel, std::fabs(hr.pdf(wo, wi, h) / pdf - 1));
                    }
                    const double z = 1 - 2 * U(rng), r = safeSqrt(1 - z * z), ph = 2 * kPi * U(rng);
                    const V3 wu{z, r * std::cos(ph), r * std::sin(ph)};
                    const RGB f = hr.eval(wo, wu, h);
                    for (int c = 0; c < 3; ++c)
                        uniAcc[c] += f[c] * 4 * kPi;
                    pdfInt += hr.pdf(wo, wu, h) * 4 * kPi;
                }
                std::printf("   bm %.1f bn %.1f theta_o %2.0f h %.2f: IS (%.4f %.4f %.4f) uniform (%.4f %.4f %.4f) "
                            "pdf integral %.4f\n",
                            bm, bn, t, h, isAcc[0] / n, isAcc[1] / n, isAcc[2] / n, uniAcc[0] / n, uniAcc[1] / n,
                            uniAcc[2] / n, pdfInt / n);
            }
        }

    std::printf("== 3. Chi-square test of the sampler (equal-area bins in sin(theta_i) x phi_i) ==\n");
    const int NT = 24, NP = 48, N = 2000000;
    for (double bm : {0.15, 0.4})
        for (double bn : {0.2, 0.8})
            for (double t : {0.0, 45.0}) {
                const Hair hr = Hair::make(1.55, {0.3, 0.6, 1.2}, bm, bn, 2 * kPi / 180);
                const double h = -0.55;
                const V3 wo = dirFromAngles(t, 90);
                std::vector<double> obs(NT * NP, 0), expv(NT * NP, 0);
                for (int k = 0; k < N; ++k) {
                    const double u[4] = {U(rng), U(rng), U(rng), U(rng)};
                    V3 wi;
                    double pdf;
                    RGB w;
                    if (!hr.sample(wo, h, u, wi, pdf, w))
                        continue;
                    int it = int((wi.x + 1) / 2 * NT);
                    double ph = std::atan2(wi.z, wi.y);
                    int ip = int((ph + kPi) / (2 * kPi) * NP);
                    it = std::min(std::max(it, 0), NT - 1);
                    ip = std::min(std::max(ip, 0), NP - 1);
                    obs[it * NP + ip] += 1;
                }
                const int SS = 8;
                for (int it = 0; it < NT; ++it)
                    for (int ip = 0; ip < NP; ++ip) {
                        double acc = 0;
                        for (int a = 0; a < SS; ++a)
                            for (int b = 0; b < SS; ++b) {
                                const double z = -1 + 2.0 * (it + (a + 0.5) / SS) / NT;
                                const double ph = -kPi + 2 * kPi * (ip + (b + 0.5) / SS) / NP;
                                const double r = safeSqrt(1 - z * z);
                                acc += hr.pdf(wo, {z, r * std::cos(ph), r * std::sin(ph)}, h);
                            }
                        expv[it * NP + ip] = acc / (SS * SS) * (2.0 / NT) * (2 * kPi / NP) * N;
                    }
                // merge low-expectation bins into one
                double chi = 0, poolO = 0, poolE = 0;
                int dof = 0;
                for (size_t i = 0; i < obs.size(); ++i) {
                    if (expv[i] < 5) {
                        poolO += obs[i];
                        poolE += expv[i];
                    } else {
                        chi += sq(obs[i] - expv[i]) / expv[i];
                        ++dof;
                    }
                }
                if (poolE > 5) {
                    chi += sq(poolO - poolE) / poolE;
                    ++dof;
                }
                const double pval = gammaQ((dof - 1) / 2.0, chi / 2);
                std::printf("   bm %.2f bn %.1f theta_o %2.0f h %.2f: chi2 %.1f dof %d p %.3f %s\n", bm, bn, t, h, chi,
                            dof - 1, pval, pval > 0.01 ? "ok" : "FAIL");
            }
    return 0;
}

// The Karma calibration scenes (ch_karma_scenes.py): a fibre along
// T = (0, cos a, sin a) seen orthographically along -z (wo = +z); lights
// arrive from world directions. Local frame: x = T, y = (-1, 0, 0),
// z = (0, -sin a, cos a).
V3 toLocal(V3 w, double aDeg) {
    const double a = aDeg * kPi / 180;
    const V3 X{0, std::cos(a), std::sin(a)}, Y{-1, 0, 0}, Z{0, -std::sin(a), std::cos(a)};
    return {dot(w, X), dot(w, Y), dot(w, Z)};
}

struct Light {
    const char *tag;
    V3 dir;
};

const Light kLights[] = {
    {"l0", {0, 0, 1}},
    {"l1", {1, 0, 0}},
    {"l2", {0, 0, -1}},
    {"l3", {0, std::sin(10 * kPi / 180), std::cos(10 * kPi / 180)}},
    {"l4", {0, -std::sin(10 * kPi / 180), std::cos(10 * kPi / 180)}},
    {"l5", {0.5, 0.5, std::sqrt(0.5)}},
};

// Mean over h of f|cos| for one light (irradiance 1), per lobe.
void distantLight(const Hair &hr, double aDeg, V3 wiWorld, RGB lobes[4], RGB &sum, int nh = 4000) {
    const V3 wo = toLocal({0, 0, 1}, aDeg), wi = toLocal(normalize(wiWorld), aDeg);
    for (int p = 0; p < 4; ++p)
        lobes[p] = {0, 0, 0};
    sum = {0, 0, 0};
    for (int ih = 0; ih < nh; ++ih) {
        const double h = -1 + (ih + 0.5) * 2.0 / nh;
        RGB l[4];
        const RGB f = hr.eval(wo, wi, h, l);
        for (int c = 0; c < 3; ++c) {
            sum[c] += f[c] / nh;
            for (int p = 0; p < 4; ++p)
                lobes[p][c] += l[p][c] / nh;
        }
    }
}

// The furnace albedo split by hypotheses for Karma's R/T classification.
void furnaceSplit(const Hair &hr, double aDeg, RGB &lobeR, RGB &lobeT, RGB &sideN, RGB &sideZ, int nh = 64,
                  int ns = 8192) {
    std::mt19937_64 rng(11);
    std::uniform_real_distribution<double> U(0, 1);
    const V3 wo = toLocal({0, 0, 1}, aDeg);
    lobeR = lobeT = sideN = sideZ = {0, 0, 0};
    for (int ih = 0; ih < nh; ++ih) {
        const double h = -1 + (ih + 0.5) * 2.0 / nh;
        // tube normal at the hit in the local frame: in the normal plane at angle gamma_o from wo's projection
        const double phiO = std::atan2(wo.z, wo.y), g = safeAsin(h);
        const V3 n{0, std::cos(phiO - g), std::sin(phiO - g)};
        const V3 n2{0, std::cos(phiO + g), std::sin(phiO + g)};
        (void)n2;
        const V3 wop = normalize({0, wo.y, wo.z});
        for (int k = 0; k < ns; ++k) {
            const double u[4] = {U(rng), U(rng), U(rng), U(rng)};
            V3 wi;
            double pdf;
            RGB w;
            if (!hr.sample(wo, h, u, wi, pdf, w))
                continue;
            RGB l[4];
            hr.eval(wo, wi, h, l);
            for (int c = 0; c < 3; ++c) {
                const double scale = 1.0 / (pdf * nh * ns);
                lobeR[c] += (l[0][c] + l[2][c]) * scale;
                lobeT[c] += (l[1][c] + l[3][c]) * scale;
                if (dot(wi, n) > 0)
                    sideN[c] += w[c] / (nh * ns);
                if (dot(wi, wop) > 0)
                    sideZ[c] += w[c] / (nh * ns);
            }
        }
    }
}

int runPredict() {
    struct Mat {
        const char *name;
        Hair hair;
    };
    const RGB sa{0.2, 0.5, 1.0};
    const Mat mats[] = {
        {"chiang s0 (bm .3 bn .3)", Hair::make(1.55, {0, 0, 0}, 0.3, 0.3, 0)},
        {"chiang sa (bm .3 bn .3)", Hair::make(1.55, sa, 0.3, 0.3, 0)},
        {"chiang sa tilt +3deg", Hair::make(1.55, sa, 0.3, 0.3, 3 * kPi / 180)},
        {"chiang sa tilt -3deg", Hair::make(1.55, sa, 0.3, 0.3, -3 * kPi / 180)},
        {"chiang sa bm .15 bn .5", Hair::make(1.55, sa, 0.15, 0.5, 0)},
        {"chiang sa eta 1.3", Hair::make(1.3, sa, 0.3, 0.3, 0)},
    };
    std::printf("== Furnace of one fibre (orthographic, mean over h): albedo by quadrature | by sum A_p ==\n");
    for (const Mat &m : mats)
        for (double a : {0.0, 30.0, 60.0}) {
            const RGB r = albedoQuad(m.hair, a, 64, 8000, 8000);
            const RGB q = albedoAp(m.hair, a);
            std::printf("   %-26s a %2.0f: (%.4f %.4f %.4f) | (%.4f %.4f %.4f)\n", m.name, a, r[0], r[1], r[2], q[0],
                        q[1], q[2]);
        }
    std::printf("== R/T split of the furnace albedo (sigma_a = 0 and sa), hypotheses: lobes (R+TRT | TT+TRRT), "
                "side of the hit normal, side of the view plane ==\n");
    for (int mi : {0, 1})
        for (double a : {0.0, 30.0, 60.0}) {
            RGB lr, lt, sn, sz;
            furnaceSplit(mats[mi].hair, a, lr, lt, sn, sz);
            std::printf("   %-26s a %2.0f: lobes R %.4f T %.4f | wi.N>0 %.4f | wi.wo_perp>0 %.4f   (green: %.4f %.4f "
                        "%.4f %.4f)\n",
                        mats[mi].name, a, lr[0], lt[0], sn[0], sz[0], lr[1], lt[1], sn[1], sz[1]);
        }
    std::printf("== Distant lights (irradiance 1), mean over h of f|cos| per light, lobes R TT TRT TRRT+ ==\n");
    for (const Mat &m : mats)
        for (double a : {0.0, 30.0}) {
            std::printf("   %s, a %.0f\n", m.name, a);
            RGB tot{0, 0, 0}, totR{0, 0, 0}, totT{0, 0, 0};
            for (const Light &l : kLights) {
                RGB lobes[4], s;
                distantLight(m.hair, a, l.dir, lobes, s);
                std::printf("      %s (%.4f %.4f %.4f)  R %.4f TT %.4f TRT %.4f TRRT %.4f (red)\n", l.tag, s[0], s[1], s[2],
                            lobes[0][0], lobes[1][0], lobes[2][0], lobes[3][0]);
                for (int c = 0; c < 3; ++c) {
                    tot[c] += s[c];
                    totR[c] += lobes[0][c] + lobes[2][c];
                    totT[c] += lobes[1][c] + lobes[3][c];
                }
            }
            std::printf("      sum (%.4f %.4f %.4f)  R+TRT (%.4f %.4f %.4f)  TT+TRRT (%.4f %.4f %.4f)\n", tot[0], tot[1],
                        tot[2], totR[0], totR[1], totR[2], totT[0], totT[1], totT[2]);
        }
    std::printf("== Absorption candidates for Karma Hair's colour controls ==\n");
    {
        const RGB c{0.8, 0.5, 0.2};
        const RGB s = Hair::sigmaFromColor(c, 0.3);
        const RGB r = albedoAp(Hair::make(1.55, s, 0.3, 0.3, 0), 0);
        std::printf("   colour (0.8 0.5 0.2), beta_n 0.3 -> sigma_a (%.4f %.4f %.4f), furnace a0 (%.4f %.4f %.4f)\n", s[0],
                    s[1], s[2], r[0], r[1], r[2]);
    }
    for (double m : {0.4, 0.8})
        for (double red : {0.0, 0.1}) {
            const double mel = -std::log(std::fmax(1 - m, 1e-4));
            const RGB s = Hair::sigmaFromMelanin(mel * (1 - red), mel * red);
            const RGB r = albedoAp(Hair::make(1.55, s, 0.3, 0.3, 0), 0);
            const RGB s2 = Hair::sigmaFromMelanin(m * (1 - red), m * red);
            const RGB r2 = albedoAp(Hair::make(1.55, s2, 0.3, 0.3, 0), 0);
            std::printf("   melanin %.1f redness %.1f: MaterialX deon map (-ln(1-m)) sigma (%.4f %.4f %.4f) furnace a0 "
                        "(%.4f %.4f %.4f) | linear map sigma (%.4f %.4f %.4f) furnace (%.4f %.4f %.4f)\n",
                        m, red, s[0], s[1], s[2], r[0], r[1], r[2], s2[0], s2[1], s2[2], r2[0], r2[1], r2[2]);
        }
    return 0;
}

// What-if: the furnace energy on the reflection side (wi.N > 0) for raw lobe
// parameters (v_R, s in the paper's units, tilt alpha in degrees, eta, sigma_a),
// i.e. what a renderer whose own fibre occludes the transmission side shows.
int runWhatIf(int argc, char **argv) {
    const double vR = std::atof(argv[2]), s = std::atof(argv[3]), alpha = std::atof(argv[4]), eta = std::atof(argv[5]);
    const double sa = argc > 6 ? std::atof(argv[6]) : 0.0;
    Hair hr = Hair::make(eta, {sa, sa, sa}, 0.3, 0.3, alpha * kPi / 180);
    hr.v[0] = vR;
    hr.v[1] = 0.25 * vR;
    hr.v[2] = hr.v[3] = 4 * vR;
    hr.s = s;
    for (double a : {0.0, 30.0, 60.0}) {
        RGB lr, lt, sn, sz;
        furnaceSplit(hr, a, lr, lt, sn, sz, 64, 4096);
        std::printf("   v_R %.4f s %.4f alpha %+.1f eta %.2f sigma %.2f, a %2.0f: total %.4f, R side (wi.N>0) %.4f\n", vR,
                    s, alpha, eta, sa, a, lr[0] + lt[0], sn[0]);
    }
    return 0;
}

// f|cos| across the fibre for one of the calibration lights (fibre a = 0),
// h = N . y with y = z x T, z = wo projected into the normal plane; in the
// calibration image (T = +y, wo = +z) y is world -x, so h = -X / r.
int runProfile(int argc, char **argv) {
    const int li = std::atoi(argv[2]);
    const double bm = std::atof(argv[3]), bn = std::atof(argv[4]), alpha = std::atof(argv[5]);
    const RGB sa{std::atof(argv[6]), std::atof(argv[7]), std::atof(argv[8])};
    const Hair hr = Hair::make(1.55, sa, bm, bn, alpha * kPi / 180);
    const V3 wo = toLocal({0, 0, 1}, 0), wi = toLocal(normalize(kLights[li].dir), 0);
    for (double X : {-0.9, -0.5, 0.0, 0.5, 0.9}) {
        const RGB f = hr.eval(wo, wi, -X);
        std::printf("   light %s, world x/r %+.1f (h %+.1f): (%.4f %.4f %.4f)\n", kLights[li].tag, X, -X, f[0], f[1], f[2]);
    }
    return 0;
}

int runInvert(int argc, char **argv) {
    const double theta = std::atof(argv[2]), bm = std::atof(argv[3]), bn = std::atof(argv[4]);
    for (int i = 5; i < argc; ++i) {
        const double target = std::atof(argv[i]);
        double lo = 0, hi = 50;
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo + hi);
            const RGB r = albedoAp(Hair::make(1.55, {mid, mid, mid}, bm, bn, 0), theta, 2000);
            (r[0] > target ? lo : hi) = mid;
        }
        std::printf("   albedo %.4f at theta_o %.0f -> sigma_a %.4f\n", target, theta, 0.5 * (lo + hi));
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (const char *t = std::getenv("CH_I0_TERMS"))
        g_i0Terms = std::max(1, std::atoi(t));
    if (const char *t = std::getenv("CH_DROP_R"))
        g_dropR = std::atoi(t) != 0;
    if (const char *t = std::getenv("CH_TILT_WI"))
        g_tiltWi = std::atoi(t) != 0;
    if (argc >= 2 && !std::strcmp(argv[1], "tests"))
        return runTests();
    if (argc >= 2 && !std::strcmp(argv[1], "predict"))
        return runPredict();
    if (argc >= 9 && !std::strcmp(argv[1], "profile"))
        return runProfile(argc, argv);
    if (argc >= 6 && !std::strcmp(argv[1], "whatif"))
        return runWhatIf(argc, argv);
    if (argc >= 6 && !std::strcmp(argv[1], "invert"))
        return runInvert(argc, argv);
    std::printf("usage: ch_hair_proto tests | predict | invert <thetaO_deg> <beta_m> <beta_n> <albedo>...\n");
    return 1;
}
