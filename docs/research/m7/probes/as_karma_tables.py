"""Tables for the report from as_karma_adaptive.py results: per scene and
run the primary rays per pixel, Karma's render wall time, the relMSE
against the reference (reference variance subtracted), and for the variance
oracle runs the equal-error saving against the uniform runs (log-log fit of
relMSE over rays per pixel and over render time).

    $HFS/python313/python.exe -I as_karma_tables.py <work>/*_karma_adaptive.json
"""
import json
import math
import sys


def fit(xs, ys):
    lx = [math.log(x) for x in xs]
    ly = [math.log(y) for y in ys]
    n = len(lx)
    mx, my = sum(lx) / n, sum(ly) / n
    sxx = sum((a - mx) ** 2 for a in lx)
    sxy = sum((a - mx) * (b - my) for a, b in zip(lx, ly))
    slope = sxy / sxx
    return slope, my - slope * mx


def invert(f, y):
    slope, icpt = f
    return math.exp((math.log(y) - icpt) / slope)


for path in sys.argv[1:]:
    rows = json.load(open(path))
    scene = path.replace("\\", "/").rsplit("/", 1)[-1].replace("_karma_adaptive.json", "")
    px = None
    uni = [r for r in rows if r["kind"] == "uniform"]
    for r in rows:
        r["rays_px"] = r["primary_rays"] / (r["primary_rays"] / r["spp"]) if r["kind"] == "uniform" else None
    pixels = uni[0]["primary_rays"] / uni[0]["spp"]
    for r in rows:
        r["rays_px"] = r["primary_rays"] / pixels
    f_rays = fit([r["rays_px"] for r in uni], [r["relmse_minus_ref"] for r in uni])
    f_time = fit([r["render_s"] for r in uni], [r["relmse_minus_ref"] for r in uni])
    print(f"\n{scene} (uniform fit: relMSE ~ rays^{f_rays[0]:.2f})")
    print("| run | rays/px | render s | relMSE | equal-error uniform rays/px | saving rays | saving time |")
    print("|---|---|---|---|---|---|---|")
    for r in rows:
        name = f"uniform {r['spp']}" if r["kind"] == "uniform" else f"variance {r['threshold']:g} (max {r['spp']})"
        e = r["relmse_minus_ref"]
        if r["kind"] == "uniform":
            print(f"| {name} | {r['rays_px']:.0f} | {r['render_s']:.2f} | {e:.2e} | | | |")
        else:
            nr = invert(f_rays, e)
            nt = invert(f_time, e)
            print(f"| {name} | {r['rays_px']:.0f} | {r['render_s']:.2f} | {e:.2e} | {nr:.0f} | "
                  f"{nr / r['rays_px']:.2f}x | {nt / r['render_s']:.2f}x |")
