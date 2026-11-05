#include <cmath>
#include <cstdio>
#include <algorithm>
const double PI = 3.14159265358979;
double Dcharlie(double NdotH, double r) { double inv = 1 / std::max(r, 0.005); double s2 = 1 - NdotH * NdotH; return (2 + inv) * std::pow(s2, inv * 0.5) / (2 * PI); }
double fsheen(double NL, double NV, double NH, double r) { return Dcharlie(NH, r) / (4 * (NL + NV - NL * NV)); }
double Esheen_fit(double x, double y) {
    double a = 13.67300 - 68.78018 * x + 799.08825 * y - 905.00061 * x * y + 60.28956 * x * x + 1086.96473 * y * y;
    double b = 1.0 + 61.57746 * x + 442.78211 * y + 2597.49308 * x * y + 121.81241 * x * x + 3045.55075 * y * y;
    return std::clamp(a / b, 0.0, 1.0);
}
int main() {
    double r = 0.3, base = 0.05;
    double L[3] = {0.5, 0.0, -0.8660254}; // direction towards the light
    // pixels: orthographic view along -z; V = (0,0,1)
    for (auto bin : {std::pair{0.28, 0.32}, std::pair{0.10, 0.15}}) {
        double sumC = 0, sumK = 0, sumLam = 0; int n = 0;
        for (int j = 0; j < 400; ++j) for (int i = 0; i < 400; ++i) {
            double x = -1 + (i + 0.5) * 2 / 400, y = 1 - (j + 0.5) * 2 / 400, r2 = x * x + y * y;
            if (r2 >= 1) continue; double mu = std::sqrt(1 - r2); if (mu < bin.first || mu >= bin.second) continue;
            double N[3] = {x, y, mu}, NL = N[0] * L[0] + N[1] * L[1] + N[2] * L[2], NV = mu;
            ++n; if (NL <= 0) continue;
            double H[3] = {L[0], L[1], L[2] + 1}; double hl = std::sqrt(H[0]*H[0]+H[1]*H[1]+H[2]*H[2]);
            double NH = (N[0]*H[0]+N[1]*H[1]+N[2]*H[2]) / hl;
            double Es = Esheen_fit(NV, r);
            sumC += PI * fsheen(NL, NV, NH, r) * NL + (1 - Es) * base * NL; // Charlie lobe (MaterialX GLSL) + layered base
            sumK += Es * NL + (1 - Es) * base * NL;                         // albedo-weighted Lambert (Karma CPU VEX)
            sumLam += base * NL;
        }
        printf("bin %.2f-%.2f: charlie %.4f  albedoLambert %.4f  lambert(0.05) %.4f\n", bin.first, bin.second, sumC / n, sumK / n, sumLam / n);
    }
}
