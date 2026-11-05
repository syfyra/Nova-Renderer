"""Exact (Monte Carlo, 2^21 VNDF samples per point) directional albedos of
GGX lobes, for comparison with the Karma albedo probe. Standard Surface
alpha = roughness^2 (MaterialX roughness_anisotropy)."""
import json, math, sys
import numpy as np

rng = np.random.default_rng(7)
N = 1 << 21
U1, U2 = rng.random(N), rng.random(N)


def vndf(wo, alpha):
    # Heitz 2018, isotropic alpha; wo = (sin, 0, cos)
    vh = np.array([alpha * wo[0], alpha * wo[1], wo[2]])
    vh /= np.linalg.norm(vh)
    lensq = vh[0] ** 2 + vh[1] ** 2
    T1 = np.array([-vh[1], vh[0], 0.0]) / math.sqrt(lensq) if lensq > 0 else np.array([1.0, 0, 0])
    T2 = np.cross(vh, T1)
    r = np.sqrt(U1)
    phi = 2 * math.pi * U2
    t1 = r * np.cos(phi)
    t2 = r * np.sin(phi)
    s = 0.5 * (1 + vh[2])
    t2 = (1 - s) * np.sqrt(np.maximum(0, 1 - t1 * t1)) + s * t2
    nh = (t1[:, None] * T1 + t2[:, None] * T2 + np.sqrt(np.maximum(0, 1 - t1 * t1 - t2 * t2))[:, None] * vh)
    h = np.stack([alpha * nh[:, 0], alpha * nh[:, 1], np.maximum(1e-7, nh[:, 2])], axis=1)
    return h / np.linalg.norm(h, axis=1)[:, None]


def lam(c, a):
    c = np.clip(c, 1e-7, 1)
    return np.sqrt(a * a + (1 - a * a) * c * c)


def g2_over_g1(no, ni, a):
    # height-correlated Smith (MaterialX mx_ggx_smith_G2), divided by G1(wo)
    g2 = 2 * ni * no / (lam(ni, a) * no + lam(no, a) * ni)
    g1 = 2 * no / (no + lam(no, a))
    return g2 / g1


def fresnel_dielectric(c, eta):
    c = np.clip(c, 0, 1)
    g2 = eta * eta - 1 + c * c
    out = np.ones_like(c)
    ok = g2 > 0
    g = np.sqrt(np.where(ok, g2, 0))
    a = (g - c) / (g + c)
    b = (c * (g + c) - 1) / (c * (g - c) + 1)
    out = np.where(ok, 0.5 * a * a * (1 + b * b), 1.0)
    return out


def fresnel_conductor(c, n, k):
    c = np.clip(c, 0, 1)
    c2 = c * c
    s2 = 1 - c2
    n2, k2 = n * n, k * k
    t0 = n2 - k2 - s2
    a2b2 = np.sqrt(t0 * t0 + 4 * n2 * k2)
    t1 = a2b2 + c2
    a = np.sqrt(np.maximum(0.5 * (a2b2 + t0), 0))
    t2 = 2 * a * c
    rs = (t1 - t2) / (t1 + t2)
    t3 = c2 * a2b2 + s2 * s2
    t4 = t2 * s2
    rp = rs * (t3 - t4) / (t3 + t4)
    return 0.5 * (rp + rs)


def artistic_ior(r, g):
    r = min(max(r, 0), 0.99)
    rs = math.sqrt(r)
    nmin = (1 - r) / (1 + r)
    nmax = (1 + rs) / (1 - rs)
    n = nmax * (1 - g) + nmin * g
    k2 = ((n + 1) ** 2 * r - (n - 1) ** 2) / (1 - r)
    return n, math.sqrt(max(k2, 0))


def albedo(mu, alpha, fres):
    """E(mu) = E_h~VNDF[F(wo.h) G2/G1]; also returns E with F = 1."""
    if alpha < 1e-4:
        return float(fres(np.array([mu]))[0]), 1.0
    wo = np.array([math.sqrt(1 - mu * mu), 0.0, mu])
    h = vndf(wo, alpha)
    oh = h @ wo
    wi = 2 * oh[:, None] * h - wo
    ni = wi[:, 2]
    w = np.where(ni > 0, g2_over_g1(mu, np.maximum(ni, 1e-7), alpha), 0.0)
    return float((fres(oh) * w).mean()), float(w.mean())


GL_X, GL_W = np.polynomial.legendre.leggauss(24)
MUS = 0.5 * (GL_X + 1)
MUW = 0.5 * GL_W


def e_avg(fn):
    return float(sum(2 * m * w * fn(m) for m, w in zip(MUS, MUW)))


def main():
    out = {}
    for r in (0.0, 0.2, 0.4, 0.6, 0.8, 1.0):
        a = max(r * r, 1e-6)
        t = f"{int(round(r * 10)):02d}"
        n, k = artistic_ior(1.0, 1.0)
        cache = {}

        def both(mu, fres, key):
            if (mu, key) not in cache:
                cache[(mu, key)] = albedo(mu, a, fres)
            return cache[(mu, key)]
        metal = lambda c: fresnel_conductor(c, n, k)
        diel = lambda c: fresnel_dielectric(c, 1.5)
        e1 = e_avg(lambda m: both(m, metal, "m")[1])
        em = e_avg(lambda m: both(m, metal, "m")[0])
        es = e_avg(lambda m: both(m, diel, "d")[0])
        # Turquin compensated dielectric: E_F + F_avg-ish term approximated per mu: E_F * (1 + F(mu)(1-E1)/E1)
        es_comp = e_avg(lambda m: both(m, diel, "d")[0] * (1 + float(diel(np.array([m]))[0]) *
                                                           (1 - both(m, diel, "d")[1]) / both(m, diel, "d")[1]))
        # metal fully compensated (Turquin, F per mu): E_F * (1 + F(1-E1)/E1)
        em_comp = e_avg(lambda m: both(m, metal, "m")[0] * (1 + float(metal(np.array([m]))[0]) *
                                                            (1 - both(m, metal, "m")[1]) / both(m, metal, "m")[1]))
        out[t] = {"alpha": a, "E1_avg_ss": e1, "metal_white_ss": em, "metal_white_turquin": em_comp,
                  "diel_spec_ss": es, "diel_spec_turquin": es_comp}
        print(t, json.dumps({k2: round(v, 4) for k2, v in out[t].items()}), flush=True)
    json.dump(out, open(sys.argv[1] if len(sys.argv) > 1 else "analytic.json", "w"), indent=1)


if __name__ == "__main__":
    main()
