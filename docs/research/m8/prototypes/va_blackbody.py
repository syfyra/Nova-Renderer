"""M8 research prototype: blackbody emission conventions (Planck -> CIE XYZ -> linear Rec.709).

Run with any Python 3 + numpy, e.g. $HFS/python313/python.exe va_blackbody.py

Prints
  1. the luminance-normalized linear Rec.709 colour of a blackbody at T (the
     "colour only" convention: Cycles' Blackbody node, Arnold/RenderMan colour
     ramps; intensity is a separate factor),
  2. three intensity conventions relative to 1500 K: Stefan-Boltzmann T^4
     (total radiance, Cycles' Principled Volume "blackbody intensity" factor),
     the photometric luminance of the Planck spectrum (what a camera sees), and
     pbrt-v4's peak-normalized spectrum (luminance of B(lambda)/B(lambda_max)),
  3. the error of a small 1D lookup table in 1/T (what a GPU kernel would use).

CIE 1931 2-degree colour matching functions: the multi-lobe Gaussian fit of
Wyman, Sloan, Shirley, "Simple Analytic Approximations to the CIE XYZ Color
Matching Functions", JCGT 2(2), 2013 (good to about 1 % of the tabulated CMFs).
"""
import numpy as np

H = 6.62607015e-34   # Planck
C = 2.99792458e8     # speed of light
KB = 1.380649e-23    # Boltzmann
SIGMA_SB = 5.670374419e-8

XYZ_TO_REC709 = np.array([[3.2404542, -1.5371385, -0.4985314],
                          [-0.9692660, 1.8760108, 0.0415560],
                          [0.0556434, -0.2040259, 1.0572252]])


def g(lam, mu, s1, s2):
    s = np.where(lam < mu, s1, s2)
    return np.exp(-0.5 * ((lam - mu) / s) ** 2)


def cmf(lam):
    x = 1.056 * g(lam, 599.8, 37.9, 31.0) + 0.362 * g(lam, 442.0, 16.0, 26.7) - 0.065 * g(lam, 501.1, 20.4, 26.2)
    y = 0.821 * g(lam, 568.8, 46.9, 40.5) + 0.286 * g(lam, 530.9, 16.3, 31.1)
    z = 1.217 * g(lam, 437.0, 11.8, 36.0) + 0.681 * g(lam, 459.0, 26.0, 13.8)
    return x, y, z


LAM = np.arange(360.0, 830.0 + 0.5, 1.0)  # nm
XB, YB, ZB = cmf(LAM)


def planck(lam_nm, T):
    """Spectral radiance, W / (m^2 sr m)."""
    lam = lam_nm * 1e-9
    return 2.0 * H * C * C / lam ** 5 / np.expm1(H * C / (lam * KB * T))


def xyz(T):
    b = planck(LAM, T)
    d = 1e-9  # 1 nm steps, in metres
    return np.array([np.sum(b * XB) * d, np.sum(b * YB) * d, np.sum(b * ZB) * d])


def rgb_normalized(T):
    X = xyz(T)
    rgb = XYZ_TO_REC709 @ X
    return rgb / X[1]  # luminance (Y) = 1


def main():
    print("1. luminance-normalized linear Rec.709 colour of a blackbody")
    print(f"{'T [K]':>7} {'r':>8} {'g':>8} {'b':>8}   {'T^4 rel':>10} {'Y(Planck) rel':>14} {'Y(peak-norm) rel':>16}")
    T0 = 1500.0
    Y0 = xyz(T0)[1]
    lam_peak0 = 2.8977721e-3 / T0 * 1e9
    Yp0 = xyz(T0)[1] / planck(np.array([lam_peak0]), T0)[0]
    for T in [800, 1000, 1200, 1500, 2000, 2500, 3000, 4000, 5000, 6500, 10000, 20000]:
        rgb = rgb_normalized(T)
        Y = xyz(T)[1]
        lam_peak = 2.8977721e-3 / T * 1e9
        Yp = Y / planck(np.array([lam_peak]), T)[0]
        print(f"{T:7.0f} {rgb[0]:8.4f} {rgb[1]:8.4f} {rgb[2]:8.4f}   {(T / T0) ** 4:10.4g} {Y / Y0:14.4g} {Yp / Yp0:16.4g}")
    print()
    print("Luminance in cd/m^2 of the Planck radiance (683 lm/W * int B ybar):")
    for T in [1000, 1500, 2000, 3000, 6500]:
        print(f"  {T:6.0f} K: {683.0 * xyz(T)[1]:.4g} cd/m^2; total radiance sigma T^4 / pi = {SIGMA_SB * T ** 4 / np.pi:.4g} W/m^2/sr")
    print()

    # 3. lookup table in u = 1/T (colour changes smoothly in 1/T)
    Tmin, Tmax = 500.0, 40000.0
    for n in [32, 64, 128, 256]:
        u = np.linspace(1.0 / Tmax, 1.0 / Tmin, n)
        table = np.array([rgb_normalized(1.0 / ui) for ui in u])
        worst = 0.0
        for T in np.geomspace(Tmin, Tmax, 2000):
            ui = 1.0 / T
            f = (ui - u[0]) / (u[-1] - u[0]) * (n - 1)
            i = min(int(f), n - 2)
            w = f - i
            approx = table[i] * (1 - w) + table[i + 1] * w
            exact = rgb_normalized(T)
            worst = max(worst, float(np.max(np.abs(approx - exact) / np.maximum(np.max(np.abs(exact)), 1e-6))))
        print(f"LUT {n:4d} entries linear in 1/T over [{Tmin:.0f}, {Tmax:.0f}] K: max error {worst:.2e} (relative to max channel)")


if __name__ == "__main__":
    main()
