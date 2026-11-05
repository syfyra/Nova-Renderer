// Numeric directional albedos for the Nova M3 report (verification only).
#include <cmath>
#include <cstdio>
#include <algorithm>
struct V3 { double x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
static double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 norm(V3 a) { return a * (1.0 / std::sqrt(dot(a, a))); }
const double PI = 3.14159265358979323846;

// GGX, isotropic alpha. Smith Lambda.
static double lambda(double mu, double a) { double t2 = (1 - mu * mu) / (mu * mu); return 0.5 * (-1 + std::sqrt(1 + a * a * t2)); }
static double G1(double mu, double a) { return 1 / (1 + lambda(mu, a)); }
static double G2(double mi, double mo, double a) { return 1 / (1 + lambda(mi, a) + lambda(mo, a)); }
// Dupuy-Benyoub 2023 spherical-cap VNDF sampling.
static V3 sampleVNDF(V3 V, double a, double u1, double u2) {
    V3 Vh = norm({V.x * a, V.y * a, V.z});
    double phi = 2 * PI * u1, z = (1 - u2) * (1 + Vh.z) - Vh.z;
    double s = std::sqrt(std::max(0.0, 1 - z * z));
    V3 c{s * std::cos(phi), s * std::sin(phi), z};
    V3 H = c + Vh;
    return norm({H.x * a, H.y * a, std::max(H.z, 0.0)});
}
static double fresnelDielectric(double c, double eta) { // c = cos to microfacet, eta = n2/n1
    double g2 = eta * eta - 1 + c * c; if (g2 < 0) return 1;
    double g = std::sqrt(g2), A = (g - c) / (g + c), B = (c * (g + c) - 1) / (c * (g - c) + 1);
    return 0.5 * A * A * (1 + B * B);
}
enum FresnelKind { F_ONE, F_DIEL, F_SCHLICK };
// Directional albedo of GGX reflection with given Fresnel, by stratified VNDF sampling.
static double albedoGGX(double mu, double a, FresnelKind fk, double eta = 1.5, int N = 512) {
    V3 V{std::sqrt(1 - mu * mu), 0, mu};
    double sum = 0;
    double F0 = std::pow((eta - 1) / (eta + 1), 2);
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) {
        V3 H = sampleVNDF(V, a, (i + 0.5) / N, (j + 0.5) / N);
        double VdH = dot(V, H);
        V3 L = H * (2 * VdH) + V * -1;
        if (L.z <= 0) continue;
        double F = fk == F_ONE ? 1 : fk == F_DIEL ? fresnelDielectric(VdH, eta) : F0 + (1 - F0) * std::pow(1 - VdH, 5);
        sum += F * G2(L.z, mu, a) / G1(mu, a);
    }
    return sum / (double(N) * N);
}
// MaterialX rational fit (mx_ggx_dir_albedo_analytic), returns (A,B) with E = F0*A + F90*B.
static void mxFit(double x, double y, double &A, double &B) {
    double x2 = x * x, y2 = y * y;
    double r[4];
    const double c[9][4] = {{0.1003, 0.9345, 1.0, 1.0}, {-0.6303, -2.323, -1.765, 0.2281}, {9.748, 2.229, 8.263, 15.94},
                            {-2.038, -3.748, 11.53, -55.83}, {29.34, 1.424, 28.96, 13.08}, {-8.245, -0.7684, -7.507, 41.26},
                            {-26.44, 1.436, -36.11, 54.9}, {19.99, 0.2913, 15.86, 300.2}, {-5.448, 0.6286, 33.37, -285.1}};
    double t[9] = {1, x, y, x * y, x2, y2, x2 * y, x * y2, x2 * y2};
    for (int k = 0; k < 4; ++k) { r[k] = 0; for (int m = 0; m < 9; ++m) r[k] += c[m][k] * t[m]; }
    A = std::clamp(r[0] / r[2], 0.0, 1.0); B = std::clamp(r[1] / r[3], 0.0, 1.0);
}
// Qualitative Oren-Nayar (MaterialX mx_oren_nayar_diffuse) albedo, sigma = roughness.
static double albedoQON(double mu, double sigma, int N = 1024) {
    V3 V{std::sqrt(1 - mu * mu), 0, mu};
    double s2 = sigma * sigma, A = 1 - 0.5 * s2 / (s2 + 0.33), B = 0.45 * s2 / (s2 + 0.09), sum = 0;
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) { // cosine-weighted grid
        double r = std::sqrt((i + 0.5) / N), ph = 2 * PI * (j + 0.5) / N;
        V3 L{r * std::cos(ph), r * std::sin(ph), std::sqrt(1 - r * r)};
        double s = dot(L, V) - L.z * mu, st = s > 0 ? s / std::max(L.z, mu) : 0;
        sum += A + B * st;
    }
    return sum / (double(N) * N);
}
int main() {
    const double mus[] = {0.98, 0.875, 0.7, 0.5, 0.3, 0.125};
    printf("GGX E_ss (F=1): numeric vs MaterialX fit (A+B)\n");
    for (double r : {0.2, 0.3, 0.5, 0.7, 1.0}) {
        double a = r * r;
        printf(" r=%.1f:", r);
        for (double mu : mus) { double A, B; mxFit(mu, a, A, B); printf("  mu%.3f %.4f/%.4f", mu, albedoGGX(mu, a, F_ONE), A + B); }
        printf("\n");
    }
    printf("Dielectric R albedo eta=1.5 (exact Fresnel) single-scatter, and MaterialX fit F0*A+B:\n");
    for (double r : {0.0001, 0.2, 0.3, 0.6}) {
        double a = std::max(r * r, 1e-4);
        printf(" r=%.2f:", r);
        for (double mu : mus) { double A, B; mxFit(mu, a, A, B); printf("  mu%.3f %.4f/%.4f", mu, albedoGGX(mu, a, F_DIEL), 0.04 * A + B); }
        printf("\n");
    }
    // Compensated dielectric albedo (Turquin, F0 multiplier) and expected glossy-diffuse with rho=0.5
    printf("glossy-diffuse rho=0.5, r=0.3, eta=1.5: E = Er + (1-Er)*0.5 with Er = Ess_diel*(1+F0(1-E)/E)\n");
    { double r = 0.3, a = r * r;
      for (double mu : mus) {
        double Ed = albedoGGX(mu, a, F_DIEL), E1 = albedoGGX(mu, a, F_ONE);
        double Er = Ed * (1 + 0.04 * (1 - E1) / E1);
        double A, B; mxFit(mu, a, A, B); double comp = 1 + fresnelDielectric(mu, 1.5) * (1 - (A + B)) / (A + B);
        double Emx = (0.04 * A + B) * comp;
        printf("  mu%.3f exact %.4f   mtlx-glsl %.4f\n", mu, Er + (1 - Er) * 0.5, Emx + (1 - Emx) * 0.5);
      } }
    printf("QON albedo sigma=1:");
    for (double mu : mus) printf("  mu%.3f %.4f", mu, albedoQON(mu, 1.0));
    printf("\n");
    return 0;
}
