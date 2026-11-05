"""Compares the Karma albedo-probe profiles (results_<spp>.json) with exact
single-scatter / Turquin-compensated GGX albedos, bin by bin (pixel weight
in a bin ~ mu dmu, as on the orthographic disk)."""
import json, math, sys
import numpy as np
sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
import analytic as A  # noqa: E402

res = json.load(open(sys.argv[1]))
edges = np.linspace(0.25, 1.0, 11)
n, k = A.artistic_ior(1.0, 1.0)
metal = lambda c: A.fresnel_conductor(c, n, k)
diel = lambda c: A.fresnel_dielectric(c, 1.5)


def schlick(c, f0=0.04):
    return f0 + (1 - f0) * (1 - c) ** 5


def bin_means(fn, nq=6):
    out = []
    x, w = np.polynomial.legendre.leggauss(nq)
    for a, b in zip(edges[:-1], edges[1:]):
        mus = a + (b - a) * 0.5 * (x + 1)
        ws = w * mus
        out.append(float(sum(wi * fn(m) for m, wi in zip(mus, ws)) / ws.sum()))
    return np.array(out)


def disk_mean(binvals):
    # bins weighted by their mu dmu mass
    mass = np.array([(b * b - a * a) for a, b in zip(edges[:-1], edges[1:])])
    return float((binvals * mass).sum() / mass.sum())


rows = []
for r in (0.0, 0.2, 0.4, 0.6, 0.8, 1.0):
    a = max(r * r, 1e-6)
    t = f"{int(round(r * 10)):02d}"
    cache = {}

    def alb(mu, fres, key):
        if (round(mu, 6), key) not in cache:
            cache[(round(mu, 6), key)] = A.albedo(mu, a, fres)
        return cache[(round(mu, 6), key)]

    spec_ss = bin_means(lambda m: alb(m, diel, "d")[0])
    spec_tq = bin_means(lambda m: alb(m, diel, "d")[0] * (1 + float(diel(np.array([m]))[0]) *
                                                          (1 - alb(m, diel, "d")[1]) / alb(m, diel, "d")[1]))
    met_ss = bin_means(lambda m: alb(m, metal, "m")[0])
    met_tq = bin_means(lambda m: alb(m, metal, "m")[0] * (1 + float(metal(np.array([m]))[0]) *
                                                          (1 - alb(m, metal, "m")[1]) / alb(m, metal, "m")[1]))
    for case, refs in ((f"ss_speconly_r{t}", {"ss": spec_ss, "turquin": spec_tq}),
                       (f"ss_metal_r{t}", {"ss": met_ss, "turquin": met_tq}),
                       (f"op_metal_r{t}", {"ss": met_ss, "turquin": met_tq})):
        if case not in res:
            continue
        kb = np.array([p[1] for p in res[case]["profile"]])
        line = {"case": case, "karma_disk": round(disk_mean(kb), 4)}
        for name, ref in refs.items():
            line[name + "_disk"] = round(disk_mean(ref), 4)
            line[name + "_rel"] = round(disk_mean(kb) / disk_mean(ref) - 1, 4)
            line[name + "_worstbin_rel"] = round(float(np.max(np.abs(kb / ref - 1))), 4)
        rows.append(line)
        print(json.dumps(line), flush=True)
        if case.startswith("ss_speconly") and t in ("00", "04", "10"):
            print("   mu    karma    exact_ss  turquin")
            for i, m in enumerate(0.5 * (edges[:-1] + edges[1:])):
                print(f"   {m:.3f} {kb[i]:.4f}  {spec_ss[i]:.4f}  {spec_tq[i]:.4f}")
json.dump(rows, open(sys.argv[2], "w"), indent=1)
