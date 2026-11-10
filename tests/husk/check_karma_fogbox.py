"""husk checks of the Karma Fog Box LOP against Karma and the analytic
transmittance (docs/design/M11-P5.md: media bounded by surfaces).

The scenes (tests/husk/fogbox/*.usda) were built with the actual Fog Box LOP
by fogbox_scenes.py in an isolated hython; Karma CPU's values below were
measured once from the same files (Houdini 22.0.466, isolated husk, 256
samples; scatter and inside at 1024). Nova renders them through husk (and
through Nova XPU where a GPU is present) and is compared with:
- absorb: alpha of pure absorbers, 1 - exp(-density * depth) (unit box
  density 1, box scaled 2 density 0.25, sphere radius 0.5 density 0.5);
- density_source: the shader's density wins over volumeuniformdensity;
  volume-only materials without volumeuniform render nothing in Karma CPU
  (a Karma limit; Nova renders the medium, analytic);
- overlap: overlapping and nested boxes add their densities;
- shadow: the Shadow Density multiplier (0, 1, 3) seen in the ground's
  shadows (analytic: exp(-shadow * sqrt(2) (1 - |offset|)) averaged over the
  region) and the ground seen through the boxes (exp(-1));
- scatter: white, coloured and forward-scattering boxes over a lit ground
  (Karma's values, 3 %);
- inside: the camera inside a 40-unit fog box over the lit ground (Karma);
- emission: an absorbing emitter with volumeuniform: (e / a)(1 - exp(-1)),
  where Karma reads 23 % brighter (its uniform-volume emission estimate); the
  same cube without volumeuniform renders nothing in Karma, the medium in Nova;
- instances: uniform volumes under a point instancer (one instance twice as
  deep) and as native instances (one at half its size): every instance is a
  medium, 1 - exp(-depth), as in Karma (plain USD, no LOP);
- noise: the Fog Box with Add Noise authors a Volume prim with an in-memory
  SOP density field (no mesh), rendered through M8's volumes; the scene is
  regenerated at test time by fogbox_scenes.py in an isolated hython (which
  saves the field), and compared with Karma at volumesteprate 1 (the LOP's
  0.04 makes Karma's absorbing box 10 % brighter: its marching).

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_karma_fogbox.py --hfs ... --stage ... --out ...
"""

import argparse
import math
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

SCENES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fogbox")
E1 = 1.0 - math.exp(-1.0)


def front(x, y=0.0, h=0.1):
    """Pixel slices of [x-h, x+h] x [y-h, y+h] in the front view (30 px per unit, x in [-3, 3], y in [-1, 1])."""
    c0, c1 = int(round((x - h + 3) * 30)), int(round((x + h + 3) * 30))
    r0, r1 = int(round((1 - y - h) * 30)), int(round((1 - y + h) * 30))
    return slice(r0, r1), slice(c0, c1)


def top(x, z, hx=0.1, hz=0.25):
    """Pixel slices in the top view (20 px per unit, x in [-4, 4], z in [-3, 3])."""
    c0, c1 = int(round((x - hx + 4) * 20)), int(round((x + hx + 4) * 20))
    r0, r1 = int(round((z - hz + 3) * 20)), int(round((z + hz + 3) * 20))
    return slice(r0, r1), slice(c0, c1)


def sphere_alpha(cx, density, radius=0.5, h=0.1):
    """Mean alpha over the front region of a sphere absorber (exact chords)."""
    n, total = 60, 0.0
    for i in range(n):
        for j in range(n):
            x = -h + (i + 0.5) * 2 * h / n
            y = -h + (j + 0.5) * 2 * h / n
            r2 = x * x + y * y
            chord = 2.0 * math.sqrt(max(radius * radius - r2, 0.0))
            total += 1.0 - math.exp(-density * chord)
    return total / (n * n)


def shadow_ratio(scale, h=0.1):
    """Mean of exp(-scale * sqrt(2) (1 - |u|)) over |u| < h: the 45-degree light through a unit box."""
    n = 200
    return sum(math.exp(-scale * math.sqrt(2.0) * (1.0 - abs(-h + (i + 0.5) * 2 * h / n))) for i in range(n)) / n


LIT = 0.5 / math.pi * 2.0 * math.cos(math.radians(45.0))  # the open ground: albedo / pi * E

# (scene, region, channel, expected, tolerance, what). Channel 3 is alpha.
# Analytic expectations carry tight tolerances (the estimators' noise), Karma
# values 3 % (+ a small absolute floor).
CHECKS = [
    ("absorb", front(-2.25), 3, E1, 0.006, "unit box, density 1 (Karma 0.6310)"),
    ("absorb", front(0.0), 3, 1.0 - math.exp(-0.5), 0.006, "box scaled 2, density 0.25 (Karma 0.3937)"),
    ("absorb", front(2.25), 3, sphere_alpha(2.25, 0.5), 0.008, "sphere r 0.5, density 0.5 (Karma 0.3854)"),
    ("density_source", front(-2.25), 3, 1.0 - math.exp(-0.5), 0.006,
     "volumeuniform 1: the shader's density 0.5, not volumeuniformdensity 0.25 (Karma 0.3917)"),
    ("density_source", front(0.0), 3, 1.0 - math.exp(-0.5), 0.006,
     "volumeuniform 0: a medium in Nova (Karma CPU: nothing)"),
    ("density_source", front(2.25), 3, 1.0 - math.exp(-0.5), 0.006,
     "no volumeuniform: a medium in Nova (Karma CPU: nothing)"),
    ("overlap", front(-1.5), 3, 1.0 - math.exp(-0.5), 0.006, "box A alone (Karma 0.3932)"),
    ("overlap", front(-0.75), 3, 1.0 - math.exp(-0.75), 0.006, "A and B overlapping add (Karma 0.5265)"),
    ("overlap", front(0.75), 3, 1.0 - math.exp(-0.25), 0.006, "box B alone (Karma 0.2205)"),
    ("overlap", front(2.25), 3, 1.0 - math.exp(-0.5), 0.008, "C nested in D add (Karma 0.3967)"),
    ("shadow", top(2.5, 0.0), 0, LIT, 0.002, "open ground (Karma 0.22508)"),
    ("shadow", top(-2.0, -2.0), 0, LIT, 0.002, "Shadow Density 0: no shadow (Karma 0.22508)"),
    ("shadow", top(-2.0, 0.0), 0, LIT * shadow_ratio(1.0), 0.002, "Shadow Density 1 (Karma 0.05877)"),
    ("shadow", top(-2.0, 2.0), 0, LIT * shadow_ratio(3.0), 0.0004, "Shadow Density 3 (Karma 0.00402)"),
    ("shadow", top(0.0, -2.0), 0, LIT * math.exp(-1.0), 0.0015, "ground through box A (Karma 0.08235)"),
    ("shadow", top(0.0, 0.0), 0, LIT * math.exp(-1.0), 0.0015, "ground through box B (Karma 0.08259)"),
    ("shadow", top(0.0, 2.0), 0, LIT * math.exp(-1.0), 0.0015, "ground through box C (Karma 0.08214)"),
    ("scatter", top(0.0, -2.0), 0, 0.18753, "karma", "white box, density 0.5"),
    ("scatter", top(-2.0, -2.0), 0, 0.11667, "karma", "its shadow"),
    ("scatter", top(0.0, -1.5), 0, 0.20787, "karma", "white box, edge"),
    ("scatter", top(-2.0, -1.5), 0, 0.17186, "karma", "its shadow, edge"),
    ("scatter", top(0.0, 0.0), 0, 0.13619, "karma", "coloured box (R)"),
    ("scatter", top(0.0, 0.0), 1, 0.11642, "karma", "coloured box (G)"),
    ("scatter", top(0.0, 0.0), 2, 0.09665, "karma", "coloured box (B)"),
    ("scatter", top(-2.0, 0.0), 0, 0.06131, "karma", "its shadow"),
    ("scatter", top(0.0, 2.0), 0, 0.09789, "karma", "forward-scattering box (g 0.6)"),
    ("scatter", top(-2.0, 2.0), 0, 0.06669, "karma", "its shadow"),
    ("scatter", top(0.0, 1.5), 0, 0.16381, "karma", "forward-scattering box, edge"),
    ("scatter", top(2.5, 0.0), 0, 0.22583, "karma", "open ground with the fog's light"),
    ("emission", front(-1.5), 0, 0.5 * E1, 0.004, "uniform emitter R: (e / a)(1 - e^-1) (Karma 0.3880, +23 %)"),
    ("emission", front(-1.5), 2, 0.125 * E1, 0.002, "uniform emitter B (Karma 0.0970)"),
    ("emission", front(-1.5), 3, E1, 0.006, "uniform emitter alpha (Karma 0.6329)"),
    ("emission", front(1.5), 0, 0.5 * E1, 0.004, "the emitter without volumeuniform (Karma: nothing)"),
    ("instances", front(-2.25), 3, E1, 0.006, "point instance of a unit cube (Karma 0.6328)"),
    ("instances", front(-0.75), 3, 1.0 - math.exp(-2.0), 0.006, "point instance twice as deep (Karma 0.8663)"),
    ("instances", front(0.75), 3, E1, 0.006, "native instance of a unit cube (Karma 0.6337)"),
    ("instances", front(2.25), 3, 1.0 - math.exp(-0.5), 0.006, "native instance at half size (Karma 0.3928)"),
]

# The noise scene (generated at test time): box A scatters, box B absorbs;
# Karma CPU at volumesteprate 1, 1024 samples (at the LOP's 0.04: 0.19003,
# 0.14743, 0.07723, 0.04733).
NOISE = [(top(0.0, -1.5), 0.18949, "scattering noise box"), (top(-2.0, -1.5), 0.14630, "its shadow"),
         (top(0.0, 1.5), 0.07037, "absorbing noise box"), (top(-2.0, 1.5), 0.04502, "its shadow"),
         (top(2.5, 0.0), 0.22525, "open ground")]

# The camera inside the big fog box: Karma's mean RGB and alpha per band of
# ten rows (rows 0-30 the fog against the black sky, 40-100 the ground).
INSIDE = [(0, 0.03743, 0.72877), (10, 0.03586, 0.72560), (20, 0.03460, 0.72454), (50, 0.07207, 1.0),
          (70, 0.07451, 1.0), (90, 0.07544, 1.0)]


def load(oiio, np, path):
    image = oiio.ImageInput.open(path)
    if not image:
        return None
    spec = image.spec()
    px = np.asarray(image.read_image(0, 0, 0, spec.nchannels, oiio.FLOAT)).reshape(spec.height, spec.width, -1)
    image.close()
    return px


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--renderers", default="Nova,Nova XPU")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)
    # The noise scene with its SOP volume saved: the LOP in an isolated hython.
    generated = os.path.join(args.out, "generated")
    here = os.path.dirname(os.path.abspath(__file__))
    gen = run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               os.path.join(here, "..", "houdini", "run_hython.ps1"), "-Script", os.path.join(here, "fogbox_scenes.py"),
               "-Hfs", args.hfs, generated, "--only", "noise"], dict(os.environ), args.out)
    check(gen.returncode == 0 and os.path.exists(os.path.join(generated, "noise.usda")),
          "the Fog Box LOP writes the noise scene and its volumes")
    scenes = {name: os.path.join(SCENES, name + ".usda")
              for name in ["absorb", "density_source", "overlap", "shadow", "scatter", "inside", "emission",
                           "instances"]}
    scenes["noise"] = os.path.join(generated, "noise.usda")
    for renderer in args.renderers.split(","):
        tag = renderer.replace(" ", "_")
        images = {}
        for name, usd in scenes.items():
            exr = os.path.join(args.out, f"{name}_{tag}.exr")
            proc = run([tool(args, "husk.exe"), "--renderer", renderer, "--headlight", "none", "-s", "/Render/s",
                        "-o", exr, usd], env, args.out)
            check(proc.returncode == 0, f"{renderer}: husk {name}.usda exits with 0")
            log = proc.stdout + proc.stderr
            check("Warning" not in log and "warning" not in log, f"{renderer}: {name} prints no warnings")
            images[name] = load(oiio, np, exr)
            check(images[name] is not None, f"{renderer}: {name} rendered")
        for name, (rows, cols), channel, expected, tol, what in CHECKS:
            px = images.get(name)
            if px is None:
                continue
            value = float(px[rows, cols, channel].mean())
            if tol == "karma":
                tol = 0.03 * expected + 0.002
            check(abs(value - expected) <= tol, f"{renderer} {name}: {what}: Nova {value:.5f}, expected {expected:.5f}")
        inside = images.get("inside")
        if inside is not None:
            for r0, karma_rgb, karma_alpha in INSIDE:
                band = inside[r0:r0 + 10]
                rgb, alpha = float(band[..., 0].mean()), float(band[..., 3].mean())
                check(abs(rgb - karma_rgb) <= 0.03 * karma_rgb + 0.001,
                      f"{renderer} inside rows {r0}-{r0 + 10}: Nova {rgb:.5f}, Karma {karma_rgb:.5f}")
                check(abs(alpha - karma_alpha) <= 0.01, f"{renderer} inside rows {r0}-{r0 + 10} alpha: Nova {alpha:.4f}, Karma {karma_alpha:.4f}")
        noise = images.get("noise")
        if noise is not None:
            for (rows, cols), karma, what in NOISE:
                value = float(noise[rows, cols, 0].mean())
                check(abs(value - karma) <= 0.03 * karma + 0.002,
                      f"{renderer} noise: {what}: Nova {value:.5f}, Karma {karma:.5f}")
            box = noise[top(0.0, 1.5, 0.4, 0.4)][..., 0]
            check(float(box.std()) > 0.004, f"{renderer} noise: the absorbing box varies (std {float(box.std()):.5f})")
    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
