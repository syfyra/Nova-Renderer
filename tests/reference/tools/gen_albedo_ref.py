"""Independent oracle for Nova's directional albedos (docs/design/M3.md, M3.12;
docs/research/m3/test-strategy.md section 7.1).

Writes tests/data/albedo_ref.json: the directional albedo E(mu) of each probe
material in a white uniform environment of radiance 1, on a grid in
u = (2 / pi) asin(sqrt(mu)): E is smooth in it at both ends (like sqrt(mu) at
grazing angles, like sin(theta) at normal incidence, where rough diffuse
albedos have an infinite slope in mu). nova_tests renders the same materials on an analytic sphere seen
orthographically (direct light only) and compares the radial profile in ten
mu bins at 0.3 % absolute (tests/test_material_furnace.cpp).

Nothing here comes from Nova's code or tables. Microfacet albedos are
stratified Monte Carlo over visible normals (Heitz 2018) with the
height-correlated Smith G2 of MaterialX (mx_ggx_smith_G2); the diffuse ones are
Gauss-Legendre quadratures of the BRDF formulas (MaterialX
mx_oren_nayar_diffuse; Portsmouth, Kutz and Hill 2024 for OpenPBR's EON). The
compensations are the ones Nova chose (docs/design/M3.md section 5):
conductors Turquin 2019 with Kulla and Conty's average Fresnel (for the exact
conductor Cycles' F82 estimate of it), dielectric layers E_R / (E_R + E_T).

Run with any Python 3 with numpy, e.g. Houdini's (about four minutes):
  "$HFS/python313/python.exe" tests/reference/tools/gen_albedo_ref.py
"""
import json
import math
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "data", "albedo_ref.json"))

GRID = 80  # u nodes j / GRID
US = np.arange(GRID + 1) / GRID
MUS = np.maximum(np.sin(0.5 * math.pi * US) ** 2, 1e-4)

# 2^20 jittered stratified samples on [0, 1)^2.
SIDE = 1024
_rng = np.random.default_rng(20261013)
_i, _j = np.meshgrid(np.arange(SIDE), np.arange(SIDE), indexing="ij")
U1 = (_i.ravel() + _rng.random(SIDE * SIDE)) / SIDE
U2 = (_j.ravel() + _rng.random(SIDE * SIDE)) / SIDE

GL_X, GL_W = np.polynomial.legendre.leggauss(64)


def gauss(a, b):
    """Gauss-Legendre nodes and weights on [a, b]."""
    return 0.5 * (b - a) * GL_X + 0.5 * (b + a), 0.5 * (b - a) * GL_W


# ---------------------------------------------------------------------------
# Fresnel
# ---------------------------------------------------------------------------

def fresnel_dielectric(c, eta):
    c = np.clip(c, 0.0, 1.0)
    g2 = eta * eta - 1.0 + c * c
    g = np.sqrt(np.maximum(g2, 0.0))
    a = (g - c) / (g + c)
    b = (c * (g + c) - 1.0) / (c * (g - c) + 1.0)
    return np.where(g2 > 0.0, 0.5 * a * a * (1.0 + b * b), 1.0)


def fresnel_conductor(c, n, k):
    c = np.clip(c, 0.0, 1.0)
    c2 = c * c
    s2 = 1.0 - c2
    n2, k2 = n * n, k * k
    t0 = n2 - k2 - s2
    a2b2 = np.sqrt(t0 * t0 + 4.0 * n2 * k2)
    t1 = a2b2 + c2
    a = np.sqrt(np.maximum(0.5 * (a2b2 + t0), 0.0))
    t2 = 2.0 * a * c
    rs = (t1 - t2) / (t1 + t2)
    t3 = c2 * a2b2 + s2 * s2
    t4 = t2 * s2
    rp = rs * (t3 - t4) / (t3 + t4)
    return 0.5 * (rp + rs)


def artistic_ior(r, g):
    """Gulbrandsen 2014 (MaterialX mx_artistic_ior), reflectivity clamped to 0.99."""
    r = min(max(r, 0.0), 0.99)
    rs = math.sqrt(r)
    n = (1.0 + rs) / (1.0 - rs) * (1.0 - g) + (1.0 - r) / (1.0 + r) * g
    k2 = ((n + 1.0) ** 2 * r - (n - 1.0) ** 2) / (1.0 - r)
    return n, math.sqrt(max(k2, 0.0))


F82_MU = 1.0 / 7.0
F82_DEN = F82_MU * (1.0 - F82_MU) ** 6


def fresnel_f82(c, f0, tint):
    """Hoffman's F82-tint (MaterialX mx_fresnel_hoffman_schlick, exponent 5), clamped at 0."""
    c = np.clip(c, 0.0, 1.0)
    b = (f0 + (1.0 - f0) * (1.0 - F82_MU) ** 5) * (1.0 - tint) / F82_DEN
    return np.maximum(f0 + (1.0 - f0) * (1.0 - c) ** 5 - b * c * (1.0 - c) ** 6, 0.0)


def hemispherical_average(fn):
    """2 int F(mu) mu dmu by quadrature."""
    x, w = gauss(0.0, 1.0)
    return float(np.sum(2.0 * x * w * fn(x)))


# ---------------------------------------------------------------------------
# GGX: visible normals and height-correlated Smith masking
# ---------------------------------------------------------------------------

def view(mu):
    return np.array([math.sqrt(max(0.0, 1.0 - mu * mu)), 0.0, mu])


def visible_normals(wo, alpha):
    """Heitz 2018, isotropic alpha, for every (U1, U2)."""
    vh = np.array([alpha * wo[0], alpha * wo[1], wo[2]])
    vh /= np.linalg.norm(vh)
    lensq = vh[0] ** 2 + vh[1] ** 2
    t1v = np.array([-vh[1], vh[0], 0.0]) / math.sqrt(lensq) if lensq > 0.0 else np.array([1.0, 0.0, 0.0])
    t2v = np.cross(vh, t1v)
    r = np.sqrt(U1)
    phi = 2.0 * math.pi * U2
    t1 = r * np.cos(phi)
    t2 = r * np.sin(phi)
    s = 0.5 * (1.0 + vh[2])
    t2 = (1.0 - s) * np.sqrt(np.maximum(0.0, 1.0 - t1 * t1)) + s * t2
    nz = np.sqrt(np.maximum(0.0, 1.0 - t1 * t1 - t2 * t2))
    nh = t1[:, None] * t1v + t2[:, None] * t2v + nz[:, None] * vh
    h = np.stack([alpha * nh[:, 0], alpha * nh[:, 1], np.maximum(1e-9, nh[:, 2])], axis=1)
    return h / np.linalg.norm(h, axis=1)[:, None]


def smith_lambda(z, alpha):
    z = np.maximum(np.abs(z), 1e-12)
    return (np.sqrt(alpha * alpha * (1.0 - z * z) + z * z) - z) / (2.0 * z)


def ggx_weights(mu, alpha, eta=None):
    """Per visible normal: wo.h, the reflection weight G2 / G1(wo) (0 below the
    horizon) and, for a dielectric of index eta seen from outside, the
    refraction weight. The estimators of int f cos are F (wo.h) times the
    reflection weight and (1 - F) times the refraction weight (energy units)."""
    wo = view(mu)
    h = visible_normals(wo, alpha)
    oh = h @ wo
    lo = smith_lambda(mu, alpha)
    zr = 2.0 * oh * h[:, 2] - mu
    wr = np.where(zr > 0.0, (1.0 + lo) / (1.0 + lo + smith_lambda(zr, alpha)), 0.0)
    if eta is None:
        return oh, wr, None
    e = 1.0 / eta
    k = 1.0 - e * e * (1.0 - oh * oh)
    zt = -e * mu + (e * oh - np.sqrt(np.maximum(k, 0.0))) * h[:, 2]
    wt = np.where((zt < 0.0) & (k > 0.0), (1.0 + lo) / (1.0 + lo + smith_lambda(zt, alpha)), 0.0)
    return oh, wr, wt


_e1_cache = {}


def ggx_albedo(mu, alpha):
    """E(mu) with F = 1."""
    key = (round(mu, 12), alpha)
    if key not in _e1_cache:
        _e1_cache[key] = float(ggx_weights(mu, alpha)[1].mean())
    return _e1_cache[key]


def ggx_albedo_avg(alpha):
    x, w = gauss(0.0, 1.0)
    return float(sum(2.0 * xi * wi * ggx_albedo(float(xi), alpha) for xi, wi in zip(x, w)))


def compensated_conductor(alpha, fresnel, favg):
    """Turquin 2019: E_F (1 + F_ms (1 - E) / E), F_ms = F_avg E_avg / (1 - F_avg (1 - E_avg))."""
    eavg = ggx_albedo_avg(alpha)
    fms = favg * eavg / (1.0 - favg * (1.0 - eavg))
    out = []
    for mu in MUS:
        oh, wr, _ = ggx_weights(float(mu), alpha)
        e1 = float(wr.mean())
        ef = np.array([float((f(oh) * wr).mean()) for f in fresnel])
        out.append(ef * (1.0 + fms * (1.0 - e1) / e1))
    return out


def ss_metal(base, edge, roughness):
    """Standard Surface metal: the exact conductor of the artistic IOR."""
    alpha = roughness * roughness
    nk = [artistic_ior(b, g) for b, g in zip(base, edge)]
    fresnel = [lambda c, n=n, k=k: fresnel_conductor(c, n, k) for n, k in nk]
    favg = []
    for n, k in nk:  # Cycles: the F82-tint curve through F(1) and F(1/7), averaged
        f0 = float(fresnel_conductor(np.array(1.0), n, k))
        f82 = float(fresnel_conductor(np.array(F82_MU), n, k))
        schlick82 = f0 + (1.0 - f0) * (1.0 - F82_MU) ** 5
        favg.append(min(max(f0 + (1.0 - f0) / 21.0 - (schlick82 - f82) / (F82_DEN * 126.0), 0.0), 1.0))
    return compensated_conductor(alpha, fresnel, np.array(favg))


def pbr_metal(base, tint, roughness):
    """OpenPBR metal: F82-tint, its average by quadrature."""
    alpha = roughness * roughness
    fresnel = [lambda c, f0=f0, t=t: fresnel_f82(c, f0, t) for f0, t in zip(base, tint)]
    favg = np.array([hemispherical_average(f) for f in fresnel])
    return compensated_conductor(alpha, fresnel, favg)


def ss_dielectric(eta, roughness):
    """A reflection-only dielectric layer over black: E_R / (E_R + E_T)."""
    alpha = roughness * roughness
    out = []
    for mu in MUS:
        oh, wr, wt = ggx_weights(float(mu), alpha, eta)
        f = fresnel_dielectric(oh, eta)
        er = float((f * wr).mean())
        et = float(((1.0 - f) * wt).mean())
        out.append(np.full(3, er / (er + et)))
    return out


# ---------------------------------------------------------------------------
# Rough diffuse: hemisphere quadrature
# ---------------------------------------------------------------------------

def diffuse_albedo(mu, brdf):
    """int brdf(mu_i, s, mu) mu_i dw_i / pi over the hemisphere, brdf without
    the 1 / pi; s = wi.wo - mu_i mu. Split at theta_i = theta_o (max) and
    phi = pi / 2 (sign of s), so every piece is smooth."""
    to = math.acos(min(max(mu, 0.0), 1.0))
    total = 0.0
    for t0, t1 in ((0.0, to), (to, 0.5 * math.pi)):
        if t1 - t0 < 1e-12:
            continue
        th, wth = gauss(t0, t1)
        for p0, p1 in ((0.0, 0.5 * math.pi), (0.5 * math.pi, math.pi)):
            ph, wph = gauss(p0, p1)
            T, P = np.meshgrid(th, ph, indexing="ij")
            W = np.outer(wth, wph)
            mi = np.cos(T)
            s = math.sqrt(max(0.0, 1.0 - mu * mu)) * np.sin(T) * np.cos(P)
            total += float(np.sum(W * brdf(mi, s, mu) * mi * np.sin(T)))
    return 2.0 * total / math.pi  # phi in [0, pi] covers half the hemisphere


def qon_brdf(sigma):
    """MaterialX mx_oren_nayar_diffuse."""
    s2 = sigma * sigma
    A = 1.0 - 0.5 * s2 / (s2 + 0.33)
    B = 0.45 * s2 / (s2 + 0.09)

    def f(mi, s, mu):
        return A + B * np.where(s > 0.0, s / np.maximum(mi, mu), 0.0)
    return f


def fon_brdf(r):
    """Fujii's Oren-Nayar, EON's single-scattering part (white)."""
    A = 1.0 / (1.0 + (0.5 - 2.0 / (3.0 * math.pi)) * r)

    def f(mi, s, mu):
        return A * (1.0 + r * np.where(s > 0.0, s / np.maximum(mi, mu), s))
    return f


def ss_qon(sigma):
    f = qon_brdf(sigma)
    return [np.full(3, diffuse_albedo(float(mu), f)) for mu in MUS]


def pbr_eon(rho, r):
    """E = rho E_FON + rho_ms (1 - E_FON), rho_ms = rho^2 Eavg / (1 - rho (1 - Eavg))
    (the multiple-scattering lobe (1 - E_FON(mu_o)) (1 - E_FON(mu_i)) / (1 - Eavg)
    integrated over wi)."""
    f = fon_brdf(r)
    x, w = gauss(0.0, 1.0)
    eavg = float(sum(2.0 * xi * wi * diffuse_albedo(float(xi), f) for xi, wi in zip(x, w)))
    rho = np.array(rho)
    rho_ms = rho * rho * eavg / (1.0 - rho * (1.0 - eavg))
    out = []
    for mu in MUS:
        e = diffuse_albedo(float(mu), f)
        out.append(rho * e + rho_ms * (1.0 - e))
    return out


# ---------------------------------------------------------------------------

COPPER = (0.95, 0.64, 0.54)
WARM = (0.8, 0.5, 0.2)

# name -> (material, as tests/test_material_furnace.cpp builds it; oracle)
PROBES = {
    "ss_lambert": ("Standard Surface base_color (0.8, 0.5, 0.2), specular 0",
                   lambda: [np.array(WARM)] * len(MUS)),
    "ss_qon_r05": ("Standard Surface base_color 1, specular 0, diffuse_roughness 0.5", lambda: ss_qon(0.5)),
    "ss_qon_r10": ("Standard Surface base_color 1, specular 0, diffuse_roughness 1", lambda: ss_qon(1.0)),
    "pbr_eon_r05": ("OpenPBR base_color (0.8, 0.5, 0.2), specular_weight 0, base_diffuse_roughness 0.5",
                    lambda: pbr_eon(WARM, 0.5)),
    "pbr_eon_r10": ("OpenPBR base_color (0.8, 0.5, 0.2), specular_weight 0, base_diffuse_roughness 1",
                    lambda: pbr_eon(WARM, 1.0)),
    "ss_metal_r02": ("Standard Surface metalness 1, base_color (0.95, 0.64, 0.54), specular_roughness 0.2",
                     lambda: ss_metal(COPPER, (1, 1, 1), 0.2)),
    "ss_metal_r05": ("Standard Surface metalness 1, base_color (0.95, 0.64, 0.54), specular_roughness 0.5",
                     lambda: ss_metal(COPPER, (1, 1, 1), 0.5)),
    "ss_metal_r08": ("Standard Surface metalness 1, base_color (0.95, 0.64, 0.54), specular_roughness 0.8",
                     lambda: ss_metal(COPPER, (1, 1, 1), 0.8)),
    "ss_metal_edge_r05": ("Standard Surface metalness 1, base_color 0.9, specular_color (1, 0.6, 0.3), "
                          "specular_roughness 0.5", lambda: ss_metal((0.9, 0.9, 0.9), (1.0, 0.6, 0.3), 0.5)),
    "ss_metal_white_r10": ("Standard Surface metalness 1, base_color 1, specular_roughness 1",
                           lambda: ss_metal((1, 1, 1), (1, 1, 1), 1.0)),
    "pbr_metal_r03": ("OpenPBR base_metalness 1, base_color (0.95, 0.64, 0.54), specular_color (1, 0.9, 0.75), "
                      "specular_roughness 0.3", lambda: pbr_metal(COPPER, (1.0, 0.9, 0.75), 0.3)),
    "pbr_metal_r07": ("OpenPBR base_metalness 1, base_color (0.95, 0.64, 0.54), specular_color (1, 0.9, 0.75), "
                      "specular_roughness 0.7", lambda: pbr_metal(COPPER, (1.0, 0.9, 0.75), 0.7)),
    "ss_dielectric_r02": ("Standard Surface base_color 0, specular_IOR 1.5, specular_roughness 0.2",
                          lambda: ss_dielectric(1.5, 0.2)),
    "ss_dielectric_r05": ("Standard Surface base_color 0, specular_IOR 1.5, specular_roughness 0.5",
                          lambda: ss_dielectric(1.5, 0.5)),
    "ss_dielectric_r08": ("Standard Surface base_color 0, specular_IOR 1.5, specular_roughness 0.8",
                          lambda: ss_dielectric(1.5, 0.8)),
}


def self_checks():
    # QON at normal incidence is A; the Fujii albedo has a closed form.
    assert abs(diffuse_albedo(1.0, qon_brdf(1.0)) - 0.624060) < 1e-6
    k1 = 0.5 - 2.0 / (3.0 * math.pi)
    for mu in (0.05, 0.3, 0.7, 1.0):
        for r in (0.5, 1.0):
            si = math.sqrt(1.0 - mu * mu)
            G = si * (math.acos(mu) - si * mu) + 2.0 * ((si / mu) * (1.0 - si ** 3) - si) / 3.0
            closed = (1.0 + r * G / math.pi) / (1.0 + k1 * r)
            assert abs(diffuse_albedo(mu, fon_brdf(r)) - closed) < 1e-6, (mu, r)
    # A white furnace for the F = 1 lobe with full compensation.
    for alpha in (0.25, 1.0):
        assert 0.0 < ggx_albedo(0.5, alpha) < 1.0


def main():
    self_checks()
    probes = {}
    for name, (material, fn) in PROBES.items():
        values = fn()
        probes[name] = {"material": material,
                        "E": [[round(float(c), 6) for c in v] for v in values]}
        print(f"{name:20s} E(mu = 0.0015, 0.5, 1) green: {values[2][1]:.4f} {values[40][1]:.4f} "
              f"{values[-1][1]:.4f}", flush=True)
    out = {"generator": "tests/reference/tools/gen_albedo_ref.py",
           "note": "directional albedo in a white environment of radiance 1; rows at mu[j], RGB",
           "u": [round(float(u), 6) for u in US],
           "mu": [round(float(m), 8) for m in MUS],
           "probes": probes}
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
