#pragma once
// Throwaway sketch of the Nova core/bsdf building blocks (float only, local
// shading frame with +Z = shading normal, wo = view, wi = light, both pointing
// away from the surface; f() excludes the cosine). Mirrors nova::Vec3f.
#include <algorithm>
#include <cmath>
#include <cstdint>

#define NOVA_INLINE inline
namespace sk {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInvPi = 1.0f / kPi;

struct V2 { float x, y; };
struct V3 { float x, y, z; };
NOVA_INLINE V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
NOVA_INLINE V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
NOVA_INLINE V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
NOVA_INLINE V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
NOVA_INLINE V3 operator*(float s, V3 a) { return a * s; }
NOVA_INLINE V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
NOVA_INLINE float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
NOVA_INLINE V3 normalize(V3 a) {
    const float l2 = dot(a, a);
    return l2 > 0.0f ? a * (1.0f / std::sqrt(l2)) : V3{0.0f, 0.0f, 1.0f};
}
NOVA_INLINE float sqr(float x) { return x * x; }
NOVA_INLINE float clamp01(float x) { return std::fmin(std::fmax(x, 0.0f), 1.0f); }
NOVA_INLINE float lerp(float a, float b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------- GGX ----
struct Ggx {
    float ax, ay; // alpha_x (tangent), alpha_y (bitangent), clamped >= 1e-4

    // D(m) = 1 / (pi ax ay (mx^2/ax^2 + my^2/ay^2 + mz^2)^2)
    NOVA_INLINE float D(V3 m) const {
        if (m.z <= 0.0f) return 0.0f;
        const float x = m.x / ax, y = m.y / ay;
        const float d = x * x + y * y + m.z * m.z;
        return 1.0f / (kPi * ax * ay * d * d);
    }
    // t(w) = |(ax wx, ay wy, wz)|. Lambda(w) = (t - |wz|) / (2|wz|),
    // G1 = 2|wz| / (t + |wz|): no tan(), no 1 - cos^2 cancellation.
    NOVA_INLINE float t(V3 w) const { return std::sqrt(sqr(ax * w.x) + sqr(ay * w.y) + sqr(w.z)); }
    NOVA_INLINE float G1(V3 w) const {
        const float z = std::fabs(w.z);
        return 2.0f * z / (t(w) + z);
    }
    // Height-correlated G2 / (4 |wo.z| |wi.z|) = 1 / (2 (t_o |wi.z| + t_i |wo.z|)).
    NOVA_INLINE float vis(V3 wo, V3 wi) const {
        return 0.5f / (t(wo) * std::fabs(wi.z) + t(wi) * std::fabs(wo.z));
    }
    NOVA_INLINE float G2(V3 wo, V3 wi) const {
        return 4.0f * std::fabs(wo.z) * std::fabs(wi.z) * vis(wo, wi);
    }
    // Spherical-cap VNDF sampling (Dupuy & Benyoub 2023). wo.z > 0.
    // bounded = Eto & Tokuyoshi 2023 (reflection-only lobes).
    NOVA_INLINE float capK(V3 wo, bool bounded) const {
        if (!bounded || wo.z <= 0.0f) return 1.0f;
        const float a = clamp01(std::fmin(ax, ay));
        const float s = 1.0f + std::sqrt(sqr(wo.x) + sqr(wo.y));
        const float a2 = a * a, s2 = s * s;
        return (1.0f - a2) * s2 / (s2 + a2 * wo.z * wo.z);
    }
    NOVA_INLINE V3 sampleVisible(V3 wo, V2 u, bool bounded = false) const {
        const V3 wh = normalize({ax * wo.x, ay * wo.y, wo.z});
        const float b = capK(wo, bounded) * wh.z;
        const float phi = 2.0f * kPi * u.x;
        const float z = (1.0f - u.y) * (1.0f + b) - b; // uniform in (-b, 1]
        const float s = std::sqrt(clamp01(1.0f - z * z));
        const V3 h = V3{s * std::cos(phi), s * std::sin(phi), z} + wh;
        return normalize({ax * h.x, ay * h.y, std::fmax(h.z, 0.0f)});
    }
    // Density of m: D_wo(m) = G1(wo) max(0, wo.m) D(m) / wo.z
    NOVA_INLINE float pdfVisible(V3 wo, V3 m) const {
        const float om = dot(wo, m);
        return om > 0.0f ? G1(wo) * om * D(m) / wo.z : 0.0f;
    }
    // Solid-angle pdf of the reflected direction wi = reflect(wo, m):
    // D(m) / (2 (k wo.z + t(wo))); k = 1 for the full cap.
    NOVA_INLINE float pdfReflect(V3 wo, V3 wi, bool bounded = false) const {
        const V3 m = normalize(wo + wi);
        return D(m) / (2.0f * (capK(wo, bounded) * wo.z + t(wo)));
    }
};

NOVA_INLINE V3 reflect(V3 w, V3 m) { return 2.0f * dot(w, m) * m - w; }

// ------------------------------------------------------------ Fresnel ----
// Exact unpolarized dielectric Fresnel. cosI >= 0 on the side of wo;
// eta = n_transmitted / n_incident. Returns 1 on TIR.
NOVA_INLINE float fresnelDielectric(float cosI, float eta, float &cosT) {
    const float s2t = (1.0f - cosI * cosI) / (eta * eta);
    if (s2t >= 1.0f) { cosT = 0.0f; return 1.0f; }
    cosT = std::sqrt(1.0f - s2t);
    const float rs = (cosI - eta * cosT) / (cosI + eta * cosT);
    const float rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5f * (rs * rs + rp * rp);
}
// Exact conductor Fresnel, one channel (n + ik relative to the outside).
NOVA_INLINE float fresnelConductor(float cosI, float n, float k) {
    const float c2 = cosI * cosI, s2 = 1.0f - c2;
    const float n2 = n * n, k2 = k * k;
    const float t0 = n2 - k2 - s2;
    const float a2b2 = std::sqrt(t0 * t0 + 4.0f * n2 * k2);
    const float t1 = a2b2 + c2;
    const float a = std::sqrt(std::fmax(0.5f * (a2b2 + t0), 0.0f));
    const float t2 = 2.0f * a * cosI;
    const float Rs = (t1 - t2) / (t1 + t2);
    const float t3 = c2 * a2b2 + s2 * s2;
    const float t4 = t2 * s2;
    const float Rp = Rs * (t3 - t4) / (t3 + t4);
    return 0.5f * (Rp + Rs);
}
// Generalized Schlick with F82 tint (Hoffman 2023; MaterialX
// mx_fresnel_hoffman_schlick; OpenPBR metal for p = 5, F90 = 1). One channel.
NOVA_INLINE float fresnelF82(float mu, float F0, float F82tint, float F90, float p) {
    constexpr float kMuBar = 1.0f / 7.0f;
    const float kDen = kMuBar * std::pow(1.0f - kMuBar, 6.0f); // 0.0566...
    const float x = clamp01(mu);
    const float a = lerp(F0, F90, std::pow(1.0f - kMuBar, p)) * (1.0f - F82tint) / kDen;
    return std::fmax(lerp(F0, F90, std::pow(1.0f - x, p)) - a * x * std::pow(1.0f - x, 6.0f), 0.0f);
}
// Hemispherical average 2 int F mu dmu of the above (without the clamp).
NOVA_INLINE float fresnelF82Avg(float F0, float F82tint, float F90, float p) {
    constexpr float kMuBar = 1.0f / 7.0f;
    const float kDen = kMuBar * std::pow(1.0f - kMuBar, 6.0f);
    const float a = lerp(F0, F90, std::pow(1.0f - kMuBar, p)) * (1.0f - F82tint) / kDen;
    return lerp(F0, F90, 2.0f / ((p + 1.0f) * (p + 2.0f))) - a / 126.0f;
}
// Kulla & Conty 2017 fit of the average dielectric Fresnel.
NOVA_INLINE float fresnelDielectricAvg(float eta) {
    if (eta < 1.0f) return 0.997118f + eta * (0.1014f - eta * (0.965241f + eta * 0.130607f));
    return (eta - 1.0f) / (4.08567f + 1.00071f * eta);
}

// ------------------------------------------- rough dielectric (Walter) ----
// eta = n_inside / n_outside of the macro surface. wo may be on either side;
// the lobe is evaluated in a frame flipped so that wo.z > 0 (eta inverted).
struct DielectricSample { V3 wi; float f; float pdf; bool reflect; };

struct RoughDielectric {
    Ggx g;
    float eta;

    // Radiance transport (camera paths): BTDF divided by etap^2.
    NOVA_INLINE float eval(V3 wo, V3 wi, float *pdfOut) const {
        float e = eta;
        if (wo.z < 0.0f) { wo.z = -wo.z; wi.z = -wi.z; e = 1.0f / e; }
        *pdfOut = 0.0f;
        if (wo.z == 0.0f || wi.z == 0.0f) return 0.0f;
        const bool refl = wi.z > 0.0f;
        const float etap = refl ? 1.0f : e;
        V3 m = wo + wi * etap;
        if (dot(m, m) == 0.0f) return 0.0f;
        m = normalize(m);
        if (m.z < 0.0f) m = -m;
        const float om = dot(wo, m), im = dot(wi, m);
        if (om <= 0.0f || im * wi.z <= 0.0f) return 0.0f; // backfacing microfacets
        float cosT;
        const float F = fresnelDielectric(om, e, cosT);
        const float Dv = g.pdfVisible(wo, m); // G1 om D / wo.z
        if (refl) {
            *pdfOut = Dv / (4.0f * om) * F;
            return F * g.D(m) * g.vis(wo, wi);
        }
        const float denom = sqr(im + om / etap);
        const float dwm = std::fabs(im) / denom;
        *pdfOut = Dv * dwm * (1.0f - F);
        const float ft = (1.0f - F) * g.D(m) * g.G2(wo, wi) * std::fabs(im * om / (wi.z * wo.z * denom));
        return ft / (etap * etap);
    }
    NOVA_INLINE bool sample(V3 wo, V2 u, float uc, DielectricSample &s) const {
        float e = eta;
        const bool flip = wo.z < 0.0f;
        if (flip) { wo.z = -wo.z; e = 1.0f / e; }
        const V3 m = g.sampleVisible(wo, u);
        const float om = dot(wo, m);
        if (om <= 0.0f) return false;
        float cosT;
        const float F = fresnelDielectric(om, e, cosT);
        V3 wi;
        if (uc < F) {
            wi = reflect(wo, m);
            if (wi.z <= 0.0f) return false;
            s.reflect = true;
        } else {
            // refract wo about m: wi = -wo/e + (om/e - cosT) m
            wi = (-wo) * (1.0f / e) + m * (om / e - cosT);
            if (wi.z >= 0.0f) return false;
            s.reflect = false;
        }
        V3 wiF = wi, woF = wo;
        if (flip) { wiF.z = -wiF.z; woF.z = -woF.z; }
        s.wi = wiF;
        s.f = eval(woF, wiF, &s.pdf);
        return s.pdf > 0.0f;
    }
};

// ----------------------------------------------------- EON diffuse ----
// Portsmouth, Kutz, Hill 2024 (Listing 1/2/3/4). f includes rho/pi.
constexpr float kFon1 = 0.5f - 2.0f / (3.0f * kPi);
constexpr float kFon2 = 2.0f / 3.0f - 28.0f / (15.0f * kPi);
NOVA_INLINE float eFonExact(float mu, float r) {
    const float AF = 1.0f / (1.0f + kFon1 * r), BF = r * AF;
    const float Si = std::sqrt(std::fmax(0.0f, 1.0f - mu * mu));
    const float G = Si * (std::acos(clamp01(mu)) - Si * mu) +
                    (2.0f / 3.0f) * ((Si / mu) * (1.0f - Si * Si * Si) - Si);
    return AF + (BF * kInvPi) * G;
}
NOVA_INLINE float eFonApprox(float mu, float r) {
    const float mc = 1.0f - mu;
    const float GoverPi = mc * (0.0571085289f + mc * (0.491881867f + mc * (-0.332181442f + mc * 0.0714429953f)));
    return (1.0f + r * GoverPi) / (1.0f + kFon1 * r);
}
NOVA_INLINE float fEon(float rho, float r, V3 wi, V3 wo, bool exact) {
    if (wi.z <= 0.0f || wo.z <= 0.0f) return 0.0f;
    const float s = dot(wi, wo) - wi.z * wo.z;
    const float sovert = s > 0.0f ? s / std::fmax(wi.z, wo.z) : s;
    const float AF = 1.0f / (1.0f + kFon1 * r);
    const float fss = rho * kInvPi * AF * (1.0f + r * sovert);
    const float EFo = exact ? eFonExact(wo.z, r) : eFonApprox(wo.z, r);
    const float EFi = exact ? eFonExact(wi.z, r) : eFonApprox(wi.z, r);
    const float avgEF = AF * (1.0f + kFon2 * r);
    const float rhoMs = rho * rho * avgEF / (1.0f - rho * (1.0f - avgEF));
    const float eps = 1e-7f;
    const float fms = rhoMs * kInvPi * std::fmax(eps, 1.0f - EFo) * std::fmax(eps, 1.0f - EFi) /
                      std::fmax(eps, 1.0f - avgEF);
    return fss + fms;
}
NOVA_INLINE void ltcCoeffs(float mu, float r, float &a, float &b, float &c, float &d) {
    a = 1.0f + r * (0.303392f + (-0.518982f + 0.111709f * mu) * mu + (-0.276266f + 0.335918f * mu) * r);
    b = r * (-1.16407f + 1.15859f * mu + (0.150815f - 0.150105f * mu) * r) / (mu * mu * mu - 1.43545f);
    c = 1.0f + r * (0.20013f + (-0.506373f + 0.261777f * mu) * mu);
    d = r * (0.540852f + (-1.01625f + 0.475392f * mu) * mu) / (-1.0743f + (0.0725628f + mu) * mu);
}
// Basis with X along the projection of wo (wo = (sin, 0, cos) in it).
NOVA_INLINE void ltcBasis(V3 wo, V3 &X, V3 &Y) {
    const float l2 = wo.x * wo.x + wo.y * wo.y;
    X = l2 > 0.0f ? V3{wo.x, wo.y, 0.0f} * (1.0f / std::sqrt(l2)) : V3{1, 0, 0};
    Y = {-X.y, X.x, 0.0f};
}
NOVA_INLINE float cltcPdf(V3 wo, V3 wiL, float r) {
    V3 X, Y; ltcBasis(wo, X, Y);
    const V3 wi{dot(wiL, X), dot(wiL, Y), wiL.z};
    float a, b, c, d; ltcCoeffs(wo.z, r, a, b, c, d);
    const float detM = c * (a - b * d);
    const V3 wh{c * (wi.x - b * wi.z), (a - b * d) * wi.y, -c * (d * wi.x - a * wi.z)};
    const float lenSqr = dot(wh, wh);
    const float vz = 1.0f / std::sqrt(d * d + 1.0f);
    const float s = 0.5f * (1.0f + vz);
    return detM * detM / (lenSqr * lenSqr) * std::fmax(wh.z, 0.0f) / (kPi * s);
}
NOVA_INLINE V3 cltcSample(V3 wo, float r, float u1, float u2) {
    float a, b, c, d; ltcCoeffs(wo.z, r, a, b, c, d);
    const float R = std::sqrt(u1), phi = 2.0f * kPi * u2;
    float x = R * std::cos(phi);
    const float y = R * std::sin(phi);
    const float vz = 1.0f / std::sqrt(d * d + 1.0f);
    const float s = 0.5f * (1.0f + vz);
    x = -lerp(std::sqrt(std::fmax(0.0f, 1.0f - y * y)), x, s);
    const V3 wh{x, y, std::sqrt(std::fmax(1.0f - (x * x + y * y), 0.0f))};
    const V3 w{a * wh.x + b * wh.z, c * wh.y, d * wh.x + wh.z};
    V3 X, Y; ltcBasis(wo, X, Y);
    return normalize(X * w.x + Y * w.y + V3{0, 0, w.z});
}
NOVA_INLINE float eonUniformProb(float mu, float r) {
    return std::pow(r, 0.1f) * (0.162925f + (-0.372058f + (0.538233f - 0.290822f * mu) * mu) * mu);
}
NOVA_INLINE float eonPdf(V3 wo, V3 wi, float r) {
    if (wi.z <= 0.0f) return 0.0f;
    const float Pu = eonUniformProb(wo.z, r);
    return Pu / (2.0f * kPi) + (1.0f - Pu) * cltcPdf(wo, wi, r);
}
// u1 reused for the lobe choice as in the paper; Nova should pass a third
// dimension instead (see report).
NOVA_INLINE V3 eonSample(V3 wo, float r, float u1, float u2) {
    const float Pu = eonUniformProb(wo.z, r);
    if (u1 <= Pu) {
        u1 /= Pu;
        const float st = std::sqrt(std::fmax(0.0f, 1.0f - u1 * u1)), phi = 2.0f * kPi * u2;
        return {st * std::cos(phi), st * std::sin(phi), u1};
    }
    u1 = (u1 - Pu) / (1.0f - Pu);
    return cltcSample(wo, r, u1, u2);
}

// ---------------------------------------------------- sheen ----
// Imageworks "Charlie" (Estevez & Kulla 2017)
NOVA_INLINE float charlieD(float cosH, float r) {
    const float inv = 1.0f / std::fmax(r, 0.005f);
    const float sin2 = std::fmax(0.0f, 1.0f - cosH * cosH);
    return (2.0f + inv) * std::pow(sin2, 0.5f * inv) / (2.0f * kPi);
}
NOVA_INLINE float charlieL(float x, float r) {
    const float t = sqr(1.0f - r);
    const float a = lerp(21.5473f, 25.3245f, t), b = lerp(3.82987f, 3.32435f, t), c = lerp(0.19823f, 0.16801f, t),
                d = lerp(-1.97760f, -1.27393f, t), e = lerp(-4.32054f, -4.85967f, t);
    return a / (1.0f + b * std::pow(x, c)) + d * x + e;
}
NOVA_INLINE float charlieLambda(float mu, float r) {
    return mu < 0.5f ? std::exp(charlieL(mu, r)) : std::exp(2.0f * charlieL(0.5f, r) - charlieL(1.0f - mu, r));
}
// Original form with correlated shadowing + terminator softening (eq. 4).
NOVA_INLINE float charlieBrdf(V3 wo, V3 wi, float r, bool soften) {
    if (wo.z <= 0.0f || wi.z <= 0.0f) return 0.0f;
    const V3 h = normalize(wo + wi);
    float li = charlieLambda(wi.z, r);
    if (soften) li = std::pow(li, 1.0f + 2.0f * std::pow(1.0f - wi.z, 8.0f));
    const float G = 1.0f / (1.0f + charlieLambda(wo.z, r) + li);
    return charlieD(h.z, r) * G / (4.0f * wo.z * wi.z);
}
// MaterialX form (no G, Neubelt-Pettineo denominator).
NOVA_INLINE float charlieBrdfMx(V3 wo, V3 wi, float r) {
    if (wo.z <= 0.0f || wi.z <= 0.0f) return 0.0f;
    const V3 h = normalize(wo + wi);
    return charlieD(h.z, r) / (4.0f * (wi.z + wo.z - wi.z * wo.z));
}
NOVA_INLINE float charlieAlbedoMx(float mu, float r) { // mx_imageworks_sheen_dir_albedo_analytic
    const float x = 13.67300f - 68.78018f * mu + 799.08825f * r - 905.00061f * mu * r + 60.28956f * mu * mu +
                    1086.96473f * r * r;
    const float y = 1.0f + 61.57746f * mu + 442.78211f * r + 2597.49308f * mu * r + 121.81241f * mu * mu +
                    3045.55075f * r * r;
    return clamp01(x / y);
}
// Zeltner et al. 2022 LTC sheen, MaterialX rational fits.
NOVA_INLINE float zeltnerAlbedo(float x, float y) {
    const float s = y * (0.0206607f + 1.58491f * y) / (0.0379424f + y * (1.32227f + y));
    const float m = y * (-0.193854f + y * (-1.14885f + y * (1.7932f - 0.95943f * y * y))) / (0.046391f + y);
    const float o = y * (0.000654023f + (-0.0207818f + 0.119681f * y) * y) / (1.26264f + y * (-1.92021f + y));
    return std::exp(-0.5f * sqr((x - m) / s)) / (s * std::sqrt(2.0f * kPi)) + o;
}
NOVA_INLINE float zeltnerAInv(float x, float y) { return (2.58126f * x + 0.813703f * y) * y / (1.0f + 0.310327f * x * x + 2.60994f * x * y); }
NOVA_INLINE float zeltnerBInv(float x, float y) {
    return std::sqrt(1.0f - x) * (y - 1.0f) * y * y * y / (0.0000254053f + 1.71228f * x - 1.71506f * x * y + 1.34174f * y * y);
}
// pdf == normalized LTC lobe D(wi); f*cos = albedo * D(wi)
NOVA_INLINE float zeltnerPdf(V3 wo, V3 wi, float r) {
    r = std::fmin(std::fmax(r, 0.01f), 1.0f);
    V3 X, Y; ltcBasis(wo, X, Y);
    const V3 w{dot(wi, X), dot(wi, Y), wi.z};
    const float aI = zeltnerAInv(wo.z, r), bI = zeltnerBInv(wo.z, r);
    const V3 o{aI * w.x + bI * w.z, aI * w.y, w.z};
    const float l2 = dot(o, o);
    return std::fmax(o.z, 0.0f) * kInvPi * sqr(aI / l2);
}
NOVA_INLINE V3 zeltnerSample(V3 wo, float r, V2 u) {
    r = std::fmin(std::fmax(r, 0.01f), 1.0f);
    const float rr = std::sqrt(u.x), phi = 2.0f * kPi * u.y;
    const V3 c{rr * std::cos(phi), rr * std::sin(phi), std::sqrt(std::fmax(0.0f, 1.0f - u.x))};
    const float aI = zeltnerAInv(wo.z, r), bI = zeltnerBInv(wo.z, r);
    const V3 w = normalize({c.x / aI - c.z * bI / aI, c.y / aI, c.z});
    V3 X, Y; ltcBasis(wo, X, Y);
    return X * w.x + Y * w.y + V3{0, 0, w.z};
}

// ------------------------------------------------ MaterialX GGX albedo fit ----
NOVA_INLINE V2 mxGgxDirAlbedoAB(float NdotV, float alpha) {
    const float x = NdotV, y = alpha, x2 = x * x, y2 = y * y;
    float r[4];
    const float c0[4] = {0.1003f, 0.9345f, 1.0f, 1.0f}, cx[4] = {-0.6303f, -2.323f, -1.765f, 0.2281f},
                cy[4] = {9.748f, 2.229f, 8.263f, 15.94f}, cxy[4] = {-2.038f, -3.748f, 11.53f, -55.83f},
                cx2[4] = {29.34f, 1.424f, 28.96f, 13.08f}, cy2[4] = {-8.245f, -0.7684f, -7.507f, 41.26f},
                cx2y[4] = {-26.44f, 1.436f, -36.11f, 54.9f}, cxy2[4] = {19.99f, 0.2913f, 15.86f, 300.2f},
                cx2y2[4] = {-5.448f, 0.6286f, 33.37f, -285.1f};
    for (int i = 0; i < 4; ++i)
        r[i] = c0[i] + cx[i] * x + cy[i] * y + cxy[i] * x * y + cx2[i] * x2 + cy2[i] * y2 + cx2y[i] * x2 * y +
               cxy2[i] * x * y2 + cx2y2[i] * x2 * y2;
    return {clamp01(r[0] / r[2]), clamp01(r[1] / r[3])};
}

} // namespace sk
