"""M7 adaptive sampling prototype, step 2: replay stopping rules on Nova
sample ladders (as_nova_ladder.py).

For a pixel p the ladder holds I_N(p), the mean of its first N samples, for
N = 2^k. An adaptive Nova that checks convergence at those counts and stops
p at n_p shows exactly I_{n_p}(p): the emulation is exact for every rule
that only looks at the film at the checkpoints (it cannot model rules that
look at single samples). The half-buffer error at a checkpoint c uses the
prefix halves: I_c - I_{c/2} = (B - A) / 2, with A the mean of samples
[0, c/2) and B of [c/2, c); this is the quantity Cycles compares (its A is
the mean of a well-stratified half of the samples, see as_sobol_split.py).

Rules (--rule):
  cycles     e = sum_rgb |I_c - I_c/2| / (1e-4 + n(sum_rgb I_c)), n(s) = sqrt(s)
             for s < 1, else s (Cycles' film_adaptive_sampling_convergence_check,
             including its internal threshold factor 5), then a pixel that is
             not converged keeps its x and y neighbours going (Cycles' filter_x/y)
  cycles_nodilate  the same without the neighbour dilation
  pool3      as cycles, but the squared error is averaged over 3x3 pixels
             before the threshold test (a less noisy one-sample estimate)
  pool3n     pool3 with the normaliser n() also taken from the 3x3 mean of I
  pool3nd    pool3n plus Cycles' neighbour dilation
  tileN      pool3 per pixel, but a tile of N x N pixels only stops when all
             its pixels have converged (whole-tile skipping)
Truth: the scene's Karma reference (tests/reference/images, its sigma
subtracted) or a pair of independent Nova renders t<seed>_n<N>.exr.

Metrics: relMSE = mean over pixels and RGB of (x - t)^2 / (t^2 + 0.01),
minus the truth's own variance term; p99 = 99th percentile of the per-pixel
relative squared error (RGB mean); cost = mean samples per pixel, and with
--cost-map the samples weighted by a per-pixel cost per sample (Karma's
cputime AOV / samples, as_karma_cost.py). Equal-error savings use a
log-log fit of the uniform ladder.

    $HFS/python313/python.exe -I as_adaptive_emulator.py --ladder <work>/<scene> --scene <name>
        [--truth karma|nova] [--seeds 1] [--min 16] [--max 1024] [--rules cycles,pool3,tile8]
        [--thresholds 0.02,0.01,0.005] [--cost-map <exr>] [--json out.json]
"""
import argparse
import glob
import json
import os
import re
import sys

HFS = os.environ.get("HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466")
os.add_dll_directory(os.path.join(HFS, "bin"))
import numpy as np  # noqa: E402
import OpenImageIO as oiio  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))


def read_rgb(path):
    buf = oiio.ImageBuf(path)
    px = np.asarray(buf.get_pixels(oiio.FLOAT), dtype=np.float64)
    return px[..., :3]


def load_ladder(ladder, seed, nmax):
    imgs = {}
    for f in glob.glob(os.path.join(ladder, f"s{seed}_n*.exr")):
        n = int(re.search(r"_n(\d+)\.exr$", f).group(1))
        if n <= nmax:
            imgs[n] = read_rgb(f)
    return dict(sorted(imgs.items()))


def load_truth(args):
    if args.truth == "karma":
        ref = read_rgb(os.path.join(REPO, "tests", "reference", "images", args.scene + ".exr"))
        sig = read_rgb(os.path.join(REPO, "tests", "reference", "images", args.scene + ".sigma.exr"))
        return ref, sig * sig
    files = sorted(glob.glob(os.path.join(args.ladder, "t*_n*.exr")))
    a, b = read_rgb(files[0]), read_rgb(files[1])
    return (a + b) / 2, (a - b) ** 2 / 4


def norm(s):
    return np.where(s < 1.0, np.sqrt(np.maximum(s, 0.0)), s)


def pixel_error(I, H):
    """Cycles' per-pixel error at a checkpoint (I = mean of c, H = of c/2)."""
    return np.abs(I - H).sum(-1) / (1e-4 + norm(I.sum(-1)))


def box3(x):
    p = np.pad(x, 1, mode="edge")
    h, w = x.shape
    return sum(p[dy:dy + h, dx:dx + w] for dy in range(3) for dx in range(3)) / 9.0


def dilate_xy(active):
    """Cycles' filter_x then filter_y: an active pixel keeps its neighbours."""
    a = active.copy()
    a[:, 1:] |= active[:, :-1]
    a[:, :-1] |= active[:, 1:]
    b = a.copy()
    b[1:, :] |= a[:-1, :]
    b[:-1, :] |= a[1:, :]
    return b


def emulate(ladder, rule, threshold, nmin, nmax, schedule="pow2"):
    """schedule pow2: checkpoints at 2^k; all: at every ladder count whose
    half is also in the ladder (16, 24, 32, 48, ... = about sqrt(2) apart)."""
    ns = [n for n in ladder if n <= nmax]
    h, w, _ = ladder[ns[0]].shape
    n_p = np.full((h, w), nmax, dtype=np.int64)
    active = np.ones((h, w), dtype=bool)
    for n in ns:
        if n < nmin or n == nmax or n // 2 not in ladder or n % 2:
            continue
        if schedule == "pow2" and n & (n - 1):
            continue
        I, H = ladder[n], ladder[n // 2]
        if rule in ("cycles", "cycles_nodilate"):
            e = pixel_error(I, H)
            notconv = e >= 5.0 * threshold
            if rule == "cycles":
                notconv = dilate_xy(notconv)
        elif rule in ("pool3n", "pool3nd"):
            # Numerator and normaliser both pooled over 3x3: the decision of
            # a pixel depends much less on its own estimate (less bias).
            d2 = (np.abs(I - H).sum(-1)) ** 2
            e = np.sqrt(box3(d2)) / (1e-4 + norm(box3(I.sum(-1))))
            notconv = e >= 5.0 * threshold
            if rule == "pool3nd":
                notconv = dilate_xy(notconv)
        else:
            # Squared one-sample error pooled over 3x3, same normalisation.
            d2 = (np.abs(I - H).sum(-1)) ** 2
            e = np.sqrt(box3(d2)) / (1e-4 + norm(I.sum(-1)))
            notconv = e >= 5.0 * threshold
            if rule.startswith("tile"):
                t = int(rule[4:])
                for y in range(0, h, t):
                    for x in range(0, w, t):
                        blk = notconv[y:y + t, x:x + t]
                        blk[...] = blk.any()
        stop = active & ~notconv
        n_p[stop] = n
        active &= notconv
        if not active.any():
            break
    out = np.empty_like(ladder[ns[0]])
    for n in ns:
        m = n_p == n
        out[m] = ladder[n][m]
    return out, n_p


def metrics(img, truth, tvar, cost=None, n_p=None):
    d2 = (img - truth) ** 2
    den = truth * truth + 1e-2
    rel = d2 / den
    r = {"relmse": float(rel.mean() - (tvar / den).mean()),
         "p99": float(np.percentile(rel.mean(-1), 99)),
         "rmse": float(np.sqrt(max(d2.mean() - tvar.mean(), 0.0)))}
    if n_p is not None:
        r["spp"] = float(n_p.mean())
        if cost is not None:
            r["cost"] = float((n_p * cost).sum() / cost.sum())
    return r


def fit_uniform(rows, key):
    pts = [(np.log(r["n"]), np.log(r[key])) for r in rows if r["n"] >= 32 and r[key] > 0]
    x, y = np.array(pts).T
    slope, icpt = np.polyfit(x, y, 1)
    return slope, icpt


def equal_n(fit, value):
    slope, icpt = fit
    return float(np.exp((np.log(value) - icpt) / slope))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ladder", required=True)
    ap.add_argument("--scene", required=True)
    ap.add_argument("--truth", default="karma", choices=["karma", "nova"])
    ap.add_argument("--seeds", default="1")
    ap.add_argument("--min", type=int, default=16)
    ap.add_argument("--max", type=int, default=1024)
    ap.add_argument("--rules", default="cycles,cycles_nodilate,pool3,tile8,tile32")
    ap.add_argument("--thresholds", default="0.04,0.02,0.01,0.005")
    ap.add_argument("--cost-map", default="")
    ap.add_argument("--schedule", default="pow2", choices=["pow2", "all"])
    ap.add_argument("--coverage", default="", help="Karma EXR with a primarysamples part (uniform oracle)")
    ap.add_argument("--cost-bg", type=float, default=1.0)
    ap.add_argument("--cost-geo", type=float, default=1.0)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    truth, tvar = load_truth(args)
    cost = None
    if args.cost_map:
        cost = read_rgb(args.cost_map)[..., 0]
        cost = np.maximum(cost, 1e-6 * cost.max())
    if args.coverage:
        # Two-class cost model: cost per sample = bg + (geo - bg) * coverage,
        # coverage = the share of camera samples that hit geometry (Karma's
        # primarysamples of a uniform render / its sample count).
        inp = oiio.ImageInput.open(args.coverage)
        sub = 0
        while inp.seek_subimage(sub, 0):
            spec = inp.spec()
            if "primarysamples" in (spec.getattribute("name") or ""):
                ps = np.asarray(inp.read_image(sub, 0, 0, 1, oiio.UINT), dtype=np.float64)[..., 0]
            sub += 1
        inp.close()
        cov = ps / ps.max()
        cost = args.cost_bg + (args.cost_geo - args.cost_bg) * cov
        print(f"cost model: bg {args.cost_bg} geo {args.cost_geo}, mean coverage {cov.mean():.3f}, "
              f"mean cost {cost.mean():.3f}")
    seeds = [int(s) for s in args.seeds.split(",")]
    result = {"scene": args.scene, "truth": args.truth, "min": args.min, "max": args.max, "schedule": args.schedule,
              "uniform": [],
              "adaptive": [], "bias": []}

    ladders = {s: load_ladder(args.ladder, s, args.max) for s in seeds}
    lad = ladders[seeds[0]]
    for n, img in lad.items():
        r = metrics(img, truth, tvar, cost, np.full(img.shape[:2], n))
        r["n"] = n
        result["uniform"].append(r)
        print(f"uniform {n:5d}: relmse {r['relmse']:.3e}  p99 {r['p99']:.3e}  rmse {r['rmse']:.4f}")
    fits = {k: fit_uniform(result["uniform"], k) for k in ("relmse", "p99")}
    print("uniform fit slopes: relmse %.3f  p99 %.3f" % (fits["relmse"][0], fits["p99"][0]))

    for rule in args.rules.split(","):
        for thr in [float(t) for t in args.thresholds.split(",")]:
            img, n_p = emulate(lad, rule, thr, args.min, args.max, args.schedule)
            r = metrics(img, truth, tvar, cost, n_p)
            r.update({"rule": rule, "threshold": thr,
                      "n_hist": {str(int(k)): int(c) for k, c in zip(*np.unique(n_p, return_counts=True))}})
            r["eq_n_relmse"] = equal_n(fits["relmse"], r["relmse"]) if r["relmse"] > 0 else None
            r["eq_n_p99"] = equal_n(fits["p99"], r["p99"])
            c = r.get("cost", r["spp"])
            r["saving_relmse"] = r["eq_n_relmse"] / c if r["eq_n_relmse"] else None
            r["saving_p99"] = r["eq_n_p99"] / c
            result["adaptive"].append(r)
            print(f"{rule:16s} t {thr:6.3f}: spp {r['spp']:7.1f}" + (f" cost {r['cost']:7.1f}" if cost is not None else "")
                  + f"  relmse {r['relmse']:.3e} (=uniform {r['eq_n_relmse'] or 0:7.1f}, x{r['saving_relmse'] or 0:.2f})"
                  f"  p99 {r['p99']:.3e} (=uniform {r['eq_n_p99']:7.1f}, x{r['saving_p99']:.2f})", flush=True)

    # Bias: the mean over seeds of the adaptive image against the truth.
    if len(seeds) > 1:
        for rule in args.rules.split(","):
            for thr in [float(t) for t in args.thresholds.split(",")]:
                imgs, unis = [], []
                for s in seeds:
                    img, n_p = emulate(ladders[s], rule, thr, args.min, args.max, args.schedule)
                    imgs.append(img)
                    unis.append(ladders[s][args.max])
                for tag, stack in (("adaptive", imgs), ("uniform", unis)):
                    m = np.mean(stack, axis=0)
                    lum_w = np.array([0.2126, 0.7152, 0.0722])
                    tot_bias = float(((m - truth) @ lum_w).sum() / (truth @ lum_w).sum())
                    # Standard error of the global relative bias from the seed spread.
                    per_seed = [float(((x - truth) @ lum_w).sum() / (truth @ lum_w).sum()) for x in stack]
                    se = float(np.std(per_seed, ddof=1) / np.sqrt(len(per_seed)))
                    # Bias of the pixels that stopped before the maximum.
                    # 8x8 blocks: the block whose seed mean deviates most
                    # (relative) from the truth, and the z of that deviation
                    # from the seed spread. The truth's own noise is not in
                    # the z, so compare against the uniform control row.
                    lum = np.stack([x @ lum_w for x in stack])  # seeds x h x w
                    tl = truth @ lum_w
                    h, w = tl.shape
                    worst = (0.0, 0.0)
                    for y in range(0, h - 7, 8):
                        for x in range(0, w - 7, 8):
                            b = lum[:, y:y + 8, x:x + 8].mean((1, 2))
                            t = tl[y:y + 8, x:x + 8].mean()
                            if t <= 1e-4:
                                continue
                            rel = (b.mean() - t) / t
                            z = rel / (b.std(ddof=1) / np.sqrt(len(b)) / t + 1e-12)
                            if abs(rel) > abs(worst[0]):
                                worst = (float(rel), float(z))
                    row = {"rule": rule, "threshold": thr, "kind": tag, "global_rel_bias": tot_bias, "se": se,
                           "per_seed": per_seed, "worst_block_rel": worst[0], "worst_block_z": worst[1]}
                    result["bias"].append(row)
                    print(f"bias {tag:8s} {rule:16s} t {thr:6.3f}: global {tot_bias:+.5f} +- {se:.5f}"
                          f"   worst 8x8 block {worst[0]:+.4f} (z {worst[1]:+.1f})")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=1)


if __name__ == "__main__":
    main()
