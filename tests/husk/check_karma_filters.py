"""husk checks of Karma's Blocker Light Filter against Karma CPU
(docs/design/M11-P4.md).

The scenes in tests/scenes/karma_filters were built once in an isolated hython
with the actual Karma Blocker Light Filter LOP (karmablockerlightfilter) and
saved without Houdini's editor data: a grey ground (Standard Surface, no
specular) seen straight down by an orthographic camera, 8 x 8 units in 320 x
320 pixels (40 per unit; image top = -z), lit by a delta distant light
straight down (the factor itself shows), a rect light, a dome, or a rect over
a mirror ground, with blockers of every shape. Nova renders them through husk;
each value is the ratio of a region's mean to the same region in the scene's
unfiltered base, compared with Karma CPU's (measured once with isolated husk,
16 samples, box pixel filter):
- the falloff profiles of the sphere, box (rounded corners) and capsule, in
  the filter's (scaled) space;
- intensity, exposure, tint and invert;
- the point tested is the shading point only: a blocker over half of a rect
  light changes nothing, one at the ground blocks it (light samples and BSDF
  rays alike);
- a mirror's reflection of a light is blocked at the mirror point, a
  camera-visible light at the camera ray's origin; a dome alike;
- the filters of a light multiply;
- the space: another light filter or a camera (the inverse of its transform),
  a light (Karma maps world points by the light's transform itself), an Xform
  (world);
- the filter prim's visibility is ignored;
- filterLink: Karma ignores it (every object is blocked, measured with
  explicit includes, expandPrims, the LOP's collection pattern, a path
  expression and includeRoot with excludes). Nova applies it as UsdLux
  defines (docs/design/M11-P4.md, a stated deviation): the empty collection
  the LOP authors by default acts on everything, as in Karma; one that names
  sphere A blocks A only. Those rows compare with Nova's own expectation and
  print Karma's for information.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_karma_filters.py --hfs ... --stage ... --out ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

PX = 40  # pixels per unit; the view covers [-4, 4] x [-4, 4]
SCENES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scenes", "karma_filters")

# (scene, base scene, [(x, z, half size, channel, Karma's ratio, Nova's expectation or None)]).
# half size 0: the one pixel at (x, z). Karma's ratios are its pixel means
# over its own base render.
CHECKS = [
    # Sphere radius 1, falloff 0.5: smoothstep over [0.5, 1.5].
    ("sph_fall05", "base_sun", [(0.6125, 0, 0, 0, 0.0352, None), (0.8125, 0, 0, 0, 0.2321, None),
                                (1.0125, 0, 0, 0, 0.5189, None), (1.2125, 0, 0, 0, 0.7996, None),
                                (1.4125, 0, 0, 0, 0.9784, None), (0, 0, 0.2, 0, 0.0, None)]),
    # Box 4 x 6 x 5, falloff 0.5 of its smallest half extent (2): [1, 3] in x.
    ("box_big_fall", "base_sun", [(1.5125, 0, 0, 0, 0.1633, None), (2.0375, 0, 0, 0, 0.5281, None),
                                  (0, 2.4875, 0, 0, 0.4906, None), (0, 3.0125, 0, 0, 0.8507, None)]),
    # Box 3 x 2 x 2, rounded corner 0.5, falloff 0.5: the corner region.
    ("box_round_fall", "base_sun", [(1.2125, 0, 0, 0, 0.1163, None), (0, 0.9875, 0, 0, 0.4813, None),
                                    (1.2625, 0.7625, 0, 0, 0.3111, None), (0, 0, 0.3, 0, 0.0, None)]),
    # Capsule radius 0.5, height 3, rotated to Z, falloff 0.5.
    ("cap_fall", "base_sun", [(0.4625, 0, 0, 0, 0.3883, None), (0, 2.0375, 0, 0, 0.6121, None)]),
    # Sphere radius 1, falloff 0.5, the filter scaled (2, 1, 1).
    ("sph_scaled_fall", "base_sun", [(2.0375, 0, 0, 0, 0.5282, None), (0, 0.9875, 0, 0, 0.4813, None)]),
    # Intensity 0.5, inverted.
    ("sph_fall_int_inv", "base_sun", [(0, 0, 0.2, 0, 1.0, None), (1.0125, 0, 0, 0, 0.7406, None),
                                      (2.5, 2.5, 0.2, 0, 0.5, None)]),
    # Intensity 0.5, tint (1, 0.5, 0.25).
    ("sph_tint", "base_sun", [(0, 0, 0.2, 0, 0.5, None), (0, 0, 0.2, 1, 0.25, None), (0, 0, 0.2, 2, 0.125, None),
                              (2.5, 2.5, 0.2, 1, 1.0, None)]),
    # Intensity 0.5, exposure 1.
    ("sph_exp1", "base_sun", [(0, 0, 0.2, 0, 1.0, None)]),
    # A box over half of the rect light, not reaching the ground: no effect.
    ("rect_half", "rect_base", [(0, 0, 0.2, 0, 1.0, None), (1.0, 0, 0.1, 0, 1.0, None), (-1.0, 0, 0.1, 0, 1.0, None)]),
    # A sphere on the ground under the rect light.
    ("rect_ground", "rect_base", [(0, 0, 0.2, 0, 0.0, None), (1.5, 0, 0.1, 0, 1.0, None)]),
    # A mirror reflecting the rect light: blocked at the mirror point.
    ("mirror_blk", "mirror_base", [(0, 0, 0.2, 0, 0.0, None), (0.85, 0, 0, 0, 1.0, None)]),
    # A camera-visible light: blocked at the camera ray's origin.
    ("camvis_cam", "camvis_base", [(0, 0, 0.2, 0, 0.0, None), (0.925, 0.925, 0, 0, 1.0, None)]),
    ("camvis_light", "camvis_base", [(0, 0, 0.2, 0, 1.0, None)]),
    # A dome.
    ("dome_blk", "dome_base", [(0, 0, 0.2, 0, 0.0, None), (2.5, 2.5, 0.2, 0, 1.0, None)]),
    # Two filters on one light multiply (an inverted one of 0.25 and one of 0.5).
    ("two_filters_inv", "base_sun", [(-2.5, 0, 0.1, 0, 0.25, None), (-1.5, 0, 0.1, 0, 1.0, None),
                                     (0, 0, 0.1, 0, 0.5, None), (1.2, 0, 0.05, 0, 0.125, None),
                                     (2.0, 0, 0.1, 0, 0.25, None)]),
    # The space: another filter (radius 1 x 1.5 at (-2, 0, 1); a small sphere
    # floats above its centre), the rect light it is on (its transform
    # itself), an Xform (world).
    ("space_filter", "base_sun", [(-2, 2, 0.2, 0, 0.0, None), (-3.2, 1, 0.1, 0, 0.0, None),
                                  (0, 0, 0.2, 0, 1.0, None)]),
    ("space_own_light", "rect_base", [(0, -2, 0.2, 0, 0.0, None), (0, 0, 0.2, 0, 1.0, None)]),
    # A camera at (2, 0, -1) (not the render camera): the inverse of its transform.
    ("space_cam2", "base_sun", [(2, -1, 0.2, 0, 0.0, None), (0, 0, 0.2, 0, 1.0, None)]),
    ("sph_space", "base_sun", [(0, 0, 0.2, 0, 0.0, None), (1.5, 0, 0.1, 0, 1.0, None)]),
    # The filter prim made invisible still blocks.
    ("filt_invisible", "base_sun", [(0, 0, 0.2, 0, 0.0, None)]),
    # filterLink: the LOP's empty collection acts on everything; one that
    # names sphere A (Karma ignores it).
    ("link_default", "link_base", [(-2, 2.5, 0.1, 0, 0.0, None), (2, 2.5, 0.1, 0, 0.0, None),
                                   (0, -2, 0.2, 0, 0.0, None)]),
    ("link_A", "link_base", [(-2, 2.5, 0.1, 0, 0.0, 0.0), (2, 2.5, 0.1, 0, 0.0, 1.0), (0, -2, 0.2, 0, 0.0, 1.0)]),
]


def region(img, x, z, h, ch):
    c, r = int((x + 4) * PX), int((z + 4) * PX)
    hp = int(round(h * PX))
    return float(img[max(r - hp, 0):r + hp + 1, max(c - hp, 0):c + hp + 1, ch].mean())


def ratio(img, base, x, z, h, ch):
    b = region(base, x, z, h, ch)
    return region(img, x, z, h, ch) / b if b > 1e-8 else 0.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--renderer", default="Nova", help="for measuring: BRAY_HdKarma prints Karma's ratios")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)
    images = {}

    def render(name):
        if name in images:
            return images[name]
        exr = os.path.join(args.out, name + ".exr")
        if os.path.exists(exr):
            os.remove(exr)
        proc = run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s", "/Render/s",
                    "-o", exr, os.path.join(SCENES, name + ".usda")], env, args.out)
        check(proc.returncode == 0, f"husk {name}.usda exits with 0")
        image = oiio.ImageInput.open(exr)
        if not image:
            check(False, f"{name}: no image")
            images[name] = None
            return None
        spec = image.spec()
        px = np.asarray(image.read_image(0, 0, 0, spec.nchannels, oiio.FLOAT)).reshape(spec.height, spec.width, -1)
        image.close()
        images[name] = px
        return px

    measure = args.renderer != "Nova"
    for name, base, points in CHECKS:
        img, ref = render(name), render(base)
        if img is None or ref is None:
            continue
        for x, z, h, ch, karma, nova in points:
            value = ratio(img, ref, x, z, h, ch)
            where = f"{name} ({x}, {z}) ch{ch}"
            if measure:
                print(f"{where}: {args.renderer} {value:.4f}")
                continue
            if karma is None:
                print(f"     {where}: Nova {value:.4f} (no Karma value)")
                continue
            expected = karma if nova is None else nova
            tol = 0.03 * expected + 2e-3
            note = "" if nova is None else f" (Nova's filterLink; Karma {karma:.4f})"
            check(abs(value - expected) <= tol, f"{where}: Nova {value:.4f}, expected {expected:.4f}{note}")
    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
