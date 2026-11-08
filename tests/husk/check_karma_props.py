"""husk checks of Karma's render settings in Nova (docs/design/M11-P1.md).

The scenes in karma_props/ hold what Houdini's own LOPs author: a Karma
Render Settings LOP (karmarendersettings) and, for per-object properties, a
Render Geometry Settings LOP's Karma tab, over small geometry layers
(karma_props/build_karma_props.py writes them with an isolated hython). Nova
renders them through husk; the region means are compared with Karma CPU's,
measured once with isolated husk at the scenes' sampling (512 Path Traced
samples, uniform oracle) and stored below, as check_lights.py does:

- glass: Karma's default fake caustics (causticsenable off) with Karma's
  shadow factor, its colour, opacity, BSDF and Fresnel-darken controls, true
  caustics with and without the roughness clamp, internal reflection;
- rough: constrained roughness along paths (Karma's default) and off;
- clamp: which contributions Karma's colour limits clamp (the camera-visible
  emitter, emission found by the first bounce, light samples at the first
  hit, indirect light);
- basic: a per-object diffuse limit, the curve and point style menus on the
  render settings, motion blur off on the render settings and per object;
- sampling: the sample count Nova reads from Karma's convergence modes (a
  ray:pixelsamples var added by the script), against the paths Karma traced
  (its render stats: camera rays times secondary rays).

A check passes within 3 % of Karma's value (plus a small absolute margin for
dark regions), or within the stated bounds.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_karma_props.py --hfs ... --stage ... --out ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "karma_props")


def ortho(x0, z0, ppu):
    """Pixel slices of [x-h, x+h] x [z-h, z+h] for a top-down orthographic
    camera whose image starts at world (x0, z0), ppu pixels per unit."""
    def region(x, z, h):
        c0, c1 = int(round((x - h - x0) * ppu)), int(round((x + h - x0) * ppu))
        r0, r1 = int(round((z - h - z0) * ppu)), int(round((z + h - z0) * ppu))
        return slice(r0, max(r1, r0 + 1)), slice(c0, max(c1, c0 + 1))
    return region


G = ortho(-2.5, -2.0, 40)  # geo_glass.usda: 240 x 160 pixels, 6 x 4 units
C = ortho(-5.0, -2.5, 30)  # geo_clamp.usda: 300 x 150 pixels, 10 x 5 units
REGIONS = {
    "glass": {"ball": G(0, 0, 0.2), "ballrim": G(-0.35, 0, 0.08), "shadow": G(1.0, 0, 0.12),
              "shadowring": G(1.6, 0, 0.1), "lit": G(2.6, 0, 0.12)},
    "clamp": {"emitter": C(-3.15, 0, 0.2), "nearemit": C(-2.3, 0, 0.15), "direct": C(1.2, 0, 0.15),
              "directfar": C(-0.5, 0, 0.2), "farwall": C(4.0, 1.8, 0.2)},
    # Perspective scenes: (rows, columns).
    "rough": {"sphere": (slice(40, 70), slice(100, 140)), "streakcore": (slice(110, 150), slice(110, 125)),
              "streaksides": (slice(110, 150), slice(95, 108)), "floor": (slice(120, 160), slice(20, 80))},
    "basic": {"balls": (slice(40, 70), slice(30, 160)), "dots": (slice(80, 95), slice(15, 180)),
              "strands": (slice(0, 30), slice(20, 170)), "mover": (slice(60, 90), slice(165, 192)),
              "ground": (slice(95, 115), slice(0, 192))},
}

# (case, region, Karma's region mean[, relative tolerance]). Karma CPU 22.0.466, 512 samples,
# measured 2026-11-06 (scratch probes of docs/design/M11-P1.md).
KARMA = [
    # Fake caustics, Karma's default: a light shadow, darker rim.
    ("glass_base", "shadow", 0.04668), ("glass_base", "shadowring", 0.03723), ("glass_base", "ball", 0.07902),
    ("glass_base", "ballrim", 0.04903), ("glass_base", "lit", 0.02930),
    # Its controls (per crossing: colour 0.5, opacity 0.5, colour alone,
    # Fresnel darken 0.5).
    ("glass_fake_color", "shadow", 0.01167), ("glass_fake_color", "shadowring", 0.00931),
    ("glass_fake_opacity", "shadow", 0.04968), ("glass_fake_opacity", "shadowring", 0.03966),
    ("glass_fake_nobsdf", "shadow", 0.01319), ("glass_fake_nobsdf", "shadowring", 0.01054),
    ("glass_fake_darken", "shadow", 0.04571), ("glass_fake_darken", "shadowring", 0.02379),
    # Internal reflection on: the exits lose the Fresnel share, also in
    # the shadow rays' factor.
    ("glass_internal", "shadow", 0.04481), ("glass_internal", "ball", 0.07618), ("glass_internal", "ballrim", 0.04358),
    # True caustics (the focus over the shadow; noisy: 10 %), without and
    # with Karma's roughness clamp 0.2, which scatters light around the
    # shadow ("lit" +10 % in both). Under the sphere's rim Nova's true
    # caustics are darker than Karma's (docs/design/M11-P1.md 7): no check.
    ("glass_caustics", "shadow", 0.21356, 0.1), ("glass_caustics_clamp", "shadow", 0.19533, 0.1),
    ("glass_caustics_clamp", "lit", 0.03237),
    # Colour limits off (1e6), at the LOP's 20, at 1 (linked), indirect 0.5
    # and direct 0.5 (unlinked).
    ("clamp_off", "emitter", 20.0), ("clamp_off", "nearemit", 0.46029), ("clamp_off", "direct", 3.83376),
    ("clamp_off", "directfar", 0.46248), ("clamp_off", "farwall", 0.08129),
    ("clamp_default", "emitter", 20.0), ("clamp_default", "direct", 3.83376),
    # (Under the light Karma's indirect clamp cuts the bounce light a little
    # more: Nova +4.7 %.)
    ("clamp_1", "emitter", 1.0), ("clamp_1", "nearemit", 0.10755), ("clamp_1", "direct", 1.05558, 0.06),
    ("clamp_1", "directfar", 0.37270), ("clamp_1", "farwall", 0.07922),
    ("clamp_indirect", "emitter", 20.0), ("clamp_indirect", "nearemit", 0.08778), ("clamp_indirect", "direct", 3.46482),
    ("clamp_indirect", "directfar", 0.36703), ("clamp_indirect", "farwall", 0.07910),
    ("clamp_direct", "emitter", 0.5), ("clamp_direct", "nearemit", 0.46029), ("clamp_direct", "direct", 0.89903),
    ("clamp_direct", "directfar", 0.46248),
    # Roughness constrained along paths (default) and free.
    # The streak (the double reflection camera - floor - sphere - light) is
    # noisy: 6 %. Without the constraint Nova's streak is narrower than
    # Karma's (sides -8 %, a GGX difference outside P1): 10 %.
    ("rough_base", "sphere", 0.76956), ("rough_base", "streakcore", 0.72139, 0.06),
    ("rough_base", "streaksides", 0.19983, 0.06), ("rough_free", "streakcore", 0.75852, 0.06),
    ("rough_free", "streaksides", 0.18661, 0.1),
    # Everyday properties.
    ("basic_base", "balls", 0.03019), ("basic_base", "dots", 0.05160), ("basic_base", "strands", 0.05073),
    ("basic_base", "mover", 0.01687), ("basic_base", "ground", 0.06260),
    ("basic_difflimit", "ground", 0.05989), ("basic_difflimit", "dots", 0.04966),
    # Flat ribbons: Nova's camera-facing ribbons shade 6 % brighter than
    # Karma's (M9's curves, outside P1); the style itself switches (Nova
    # +10 %, Karma +4 % over round tubes): 8 %.
    ("basic_ribbons", "strands", 0.05279, 0.08), ("basic_discs", "balls", 0.03713),
    ("basic_discs", "dots", 0.05047),
    ("basic_nomblur", "mover", 0.01394), ("basic_objmblur", "mover", 0.01394), ("basic_box", "ground", 0.06258),
]

# Sampling: (case, Karma's paths per pixel from its render stats, the
# bounds of Nova's mean samples per pixel as factors of it).
SAMPLING = [
    ("sampling_default", 37.0, (0.5, 2.0)),  # Variance: 4.9 camera rays x 7.6 secondary rays
    ("sampling_variance", 15.1, (0.5, 2.0)),  # 4 x 6 at most, threshold 0.02: 2.5 x 6
    ("sampling_path", 24.0, (1.0, 1.0)),  # Path Traced, uniform: exactly 24
]


def scene_of(case):
    return case.split("_")[0]


def read(path, np, oiio):
    image = oiio.ImageInput.open(path)
    if not image:
        return {}
    layers = {}
    part = 0
    while image.seek_subimage(part, 0):
        spec = image.spec()
        px = np.asarray(image.read_image(part, 0, 0, spec.nchannels, oiio.FLOAT)).reshape(spec.height, spec.width, -1)
        layers[spec.getattribute("name") or "C"] = px
        part += 1
    image.close()
    return layers


def sampling_layer(case, out):
    """The case with a ray:pixelsamples var in its product."""
    path = os.path.join(out, case + "_samples.usda")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write('''#usda 1.0
(
    subLayers = [@%s@]
)
over "Render"
{
    over "Products"
    {
        over "renderproduct"
        {
            rel orderedVars = [</Render/Vars/beauty>, </Render/Vars/samples>]
        }
    }
    over "Vars"
    {
        def RenderVar "samples"
        {
            uniform token dataType = "float"
            uniform string sourceName = "ray:pixelsamples"
            uniform token sourceType = "raw"
            token driver:parameters:aov:format = "float"
            string driver:parameters:aov:name = "samples"
        }
    }
}
''' % os.path.join(HERE, case + ".usda").replace("\\", "/"))
    return path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--renderer", default="Nova", help="BRAY_HdKarma re-measures Karma")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)
    karma = args.renderer.startswith("BRAY")
    means = {}
    for case in sorted({entry[0] for entry in KARMA}):
        exr = os.path.join(args.out, case + ".exr")
        proc = run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s",
                    "/Render/rendersettings", "-o", exr, os.path.join(HERE, case + ".usda")], env, args.out)
        check(proc.returncode == 0, f"husk {case}.usda exits with 0")
        layers = read(exr, np, oiio)
        if not layers:
            check(False, f"{case}: no image")
            continue
        lum = next(iter(layers.values()))[..., :3].mean(axis=2)
        for name, (r, c) in REGIONS[scene_of(case)].items():
            means[(case, name)] = float(lum[r, c].mean())
    for entry in KARMA:
        case, region, value = entry[:3]
        tolerance = entry[3] if len(entry) > 3 else 0.03
        got = means.get((case, region))
        if got is None:
            continue
        if karma or value is None:
            print(f"     {case} {region}: {got:.5f}")
            continue
        ok = abs(got - value) <= tolerance * abs(value) + 2e-4
        check(ok, f"{case} {region}: Nova {got:.5f}, Karma {value:.5f}")
    if not karma:
        # Karma's constrained roughness narrows the double reflection's
        # streak (the core brighter, the sides darker without it): Nova's
        # change goes the same way.
        core = means[("rough_free", "streakcore")] / means[("rough_base", "streakcore")]
        sides = means[("rough_free", "streaksides")] / means[("rough_base", "streaksides")]
        check(core > 1.0 and sides < 1.0,
              f"rough: without constraint the streak's core x{core:.3f} (Karma x1.051), sides x{sides:.3f} (Karma x0.934)")
        # The curve style menu label on the render settings takes effect.
        ribbons = means[("basic_ribbons", "strands")] / means[("basic_base", "strands")]
        check(ribbons > 1.02, f"basic_ribbons: Flat Ribbons x{ribbons:.3f} over round tubes (Karma x1.041)")

    for case, paths, (lo, hi) in SAMPLING:
        exr = os.path.join(args.out, case + ".exr")
        layer = sampling_layer(case, args.out) if not karma else os.path.join(HERE, case + ".usda")
        proc = run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s",
                    "/Render/rendersettings", "-o", exr, layer], env, args.out)
        check(proc.returncode == 0, f"husk {case} exits with 0")
        if karma:
            continue
        layers = read(exr, np, oiio)
        samples = layers.get("samples")
        if samples is None:
            check(False, f"{case}: no samples plane")
            continue
        mean = float(samples[..., 0].mean())
        check(lo * paths - 1e-3 <= mean <= hi * paths + 1e-3,
              f"{case}: Nova {mean:.1f} samples per pixel, Karma {paths:.1f} paths (bounds x{lo}..x{hi})")
    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
