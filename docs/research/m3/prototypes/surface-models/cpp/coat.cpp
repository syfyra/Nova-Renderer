#include <cmath>
#include <cstdio>
#include <initializer_list>
static double fresnelDielectric(double c, double eta) {
    double g2 = eta * eta - 1 + c * c; if (g2 < 0) return 1;
    double g = std::sqrt(g2), A = (g - c) / (g + c), B = (c * (g + c) - 1) / (c * (g - c) + 1);
    return 0.5 * A * A * (1 + B * B);
}
static double zeltnerR(double x, double y) {
    double s = y*(0.0206607 + 1.58491*y)/(0.0379424 + y*(1.32227 + y));
    double m = y*(-0.193854 + y*(-1.14885 + y*(1.7932 - 0.95943*y*y)))/(0.046391 + y);
    double o = y*(0.000654023 + (-0.0207818 + 0.119681*y)*y)/(1.26264 + y*(-1.92021 + y));
    return std::exp(-0.5*((x - m)/s)*((x - m)/s))/(s*std::sqrt(2.0*3.14159265358979)) + o;
}
static double EF(double eta) { return std::log((10893*eta - 1438.2)/(-774.4*eta*eta + 10212*eta + 1)); }
int main() {
    const double mus[] = {0.98, 0.875, 0.7, 0.5, 0.3, 0.125};
    double eta = 1.6, F0 = std::pow((eta - 1) / (eta + 1), 2), Eb = 0.5;
    double Kmx = 1 - (1 - F0) / (eta * eta), Dmx = (1 - Kmx) / (1 - Eb * Kmx);
    double Ksp = 1 - (1 - EF(eta)) / (eta * eta), Dsp = (1 - Ksp) / (1 - Eb * Ksp);
    printf("OpenPBR coat(eta 1.6, smooth) over diffuse 0.5: K_mtlx=%.4f D=%.4f | K_spec(E_F)=%.4f D=%.4f EF=%.5f\n", Kmx, Dmx, Ksp, Dsp, EF(eta));
    for (double mu : mus) { double F = fresnelDielectric(mu, eta); printf("  mu%.3f  mtlx %.4f  spec %.4f\n", mu, F + (1 - F) * Dmx * Eb, F + (1 - F) * Dsp * Eb); }
    printf("Zeltner fuzz albedo R(mu, r):\n");
    for (double r : {0.25, 0.5, 0.75, 1.0}) { printf("  r=%.2f:", r); for (double mu : mus) printf(" %.4f", zeltnerR(mu, r)); printf("\n"); }
    printf("E_F(1.5)=%.5f E_F(1.6)=%.5f\n", EF(1.5), EF(1.6));
}
