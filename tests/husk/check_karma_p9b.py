"""husk checks of nested dielectrics and light portals in Nova
(docs/design/M11-P9b.md).

The scenes in karma_p9b/ hold what Houdini's own LOPs author: a Karma
Render Settings LOP and Render Geometry Settings LOPs with Karma's
dielectricpriority, internalreflectenable, isportal and portaldomelights
over small geometry layers (karma_p9b/build_karma_p9b.py writes them with an
isolated hython). Nova renders them through husk; the values are compared
with Karma CPU's, measured once with isolated husk and stored below, as
check_karma_props.py does:

- nested dielectrics, at normal incidence through glass A (index 1.5,
  absorbing green) and B (index 2, absorbing blue): red is the interfaces'
  Fresnel loss, green and blue the lengths inside A and B; equal priorities
  nest nothing, a lower priority's surfaces inside a higher one are passed
  through (also uncounted by the refraction limit), the medium inside is the
  highest priority's; with internal reflection the reflectance keeps each
  surface's own index;
- the refracted direction through the overlap seen at 45 degrees (the image
  row of an emitter's edge): real interfaces bend by the media's ratio;
- shadows through the stack (fake caustics) do not depend on priorities and
  take the transmission colour per crossing;
- portals: a room lit by a dome through a window keeps its brightness with a
  portal in the window and loses most of its noise; the dome lights nothing
  but through its portals (a second window, the outside); the portal serves
  the domes its list names; it neither renders nor blocks other lights.

A check passes within 3 % of Karma's value (plus a small absolute margin for
dark values), or within the stated bounds.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_karma_p9b.py --hfs ... --stage ... --out ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "karma_p9b")

# Karma CPU 22.0.466, 256 samples Path Traced (8 for *_low), measured
# 2026-11-08 with isolated husk. Nested: the RGB mean of the image's centre.
KARMA_RGB = {
    "overlap_p00": (0.85333, 0.42666, 0.42666), "overlap_p01": (0.96065, 0.24019, 0.48032),
    "overlap_p10": (0.85314, 0.42657, 0.21329), "overlap_p01_ir": (0.82741, 0.20579, 0.41109),
    "overlap_p10_ir": (0.77267, 0.38411, 0.19036), "contain_p00": (0.85333, 0.42666, 0.42666),
    "contain_p01": (0.96000, 0.12000, 0.96000), "contain_p10": (0.85436, 0.21359, 0.42718),
    "contain_p21": (0.85436, 0.21359, 0.42718), "contain_p10_ir": (0.74957, 0.18495, 0.36897),
    # Refraction limits: false interfaces are not counted; past the limit a
    # thin-wall pass-through leaves or enters the medium and tints by the
    # transmission colour.
    "limit1_p01": (0.96000, 0.06000, 0.96000), "limit2_p00": (0.85452, 0.21363, 0.21363),
    "limit2_p10": (0.85483, 0.10685, 0.21371),
}
# Oblique: the emitter edge's offset along the camera's up axis (units).
KARMA_EDGE = {"oblique_p00": -0.11318, "oblique_p01": -0.31584, "oblique_p10": -0.20154}
# Shadow through the stack over the lit floor, per channel.
KARMA_SHADOW = {"shadow_p00": (0.61316, 0.15365, 0.15374), "shadow_p01": (0.61316, 0.15365, 0.15374)}
# Portals: the RGB mean of the whole image.
KARMA_MEAN = {
    "room": (0.01432, 0.01432, 0.01432), "room_portal": (0.01431, 0.01431, 0.01431),
    "room_quad": (0.0, 0.0, 0.0),  # the same quad without isportal blocks the window
    "room2_two": (0.02843, 0.0, 0.02840), "room2_two_portal": (0.01418, 0.0, 0.01417),
    "room2_two_red": (0.01418, 0.0, 0.02840), "room2_two_list": (0.02843, 0.0, 0.01417),
    "room2_two_name": (0.02843, 0.0, 0.02840),  # a name is no path: no dome served
    "outside": (0.30520, 0.30520, 0.30520), "outside_portal": (0.00155, 0.00155, 0.00155),
    "room_sun": (0.01593, 0.01593, 0.01593), "room_sun_portal": (0.01593, 0.01593, 0.01593),
}
# Portals at 8 samples: the RMS difference to the case's 256-sample image
# (room_portal for every one: the same light), Karma's for information.
KARMA_NOISE = {"room_low": 0.02338, "room_portal_low": 0.00207, "room_portal_env_low": 0.02338,
               "room_portal_portal_low": 0.00123}


def read(path, np, oiio):
    image = oiio.ImageInput.open(path)
    if not image:
        return None
    spec = image.spec()
    px = np.asarray(image.read_image(0, 0, 0, spec.nchannels, oiio.FLOAT)).reshape(spec.height, spec.width, -1)
    image.close()
    return px[..., :3]


def edge(px, np):
    """The emitter edge's offset (units along the camera's up axis) in an
    oblique image: 192 rows over 12 units, the half-level crossing of red."""
    col = px[:, 4:12, 0].mean(axis=1)
    hi = float(np.median(col[col > 0.5 * col.max()]))
    above = col > 0.5 * hi
    idx = np.where(above[:-1] != above[1:])[0]
    if len(idx) != 1:
        return None
    i = int(idx[0])
    f = (0.5 * hi - col[i]) / (col[i + 1] - col[i])
    row = i + 0.5 + f
    return 6.0 - 12.0 * row / px.shape[0]


def shadow_ratio(px):
    """Floor under the stack's shadow over the lit floor: x 4..7 at 10 pixels
    a unit, z -1.5..7.5 from the top row."""
    def reg(x0, x1, z0, z1):
        c0, c1 = int((x0 - 4) * 10), int((x1 - 4) * 10)
        r0, r1 = int((z0 + 1.5) * 10), int((z1 + 1.5) * 10)
        return px[r0:r1, c0:c1].reshape(-1, 3).mean(axis=0)
    return reg(4.6, 5.9, -1.0, 1.0) / reg(4.2, 6.8, 5.0, 7.0)


def near(got, want, rel=0.03, eps=2e-4):
    return all(abs(g - w) <= rel * abs(w) + eps for g, w in zip(got, want))


def fmt(v):
    return "(" + ", ".join("%.5f" % x for x in v) + ")"


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
    cases = sorted(set(KARMA_RGB) | set(KARMA_EDGE) | set(KARMA_SHADOW) | set(KARMA_MEAN) | set(KARMA_NOISE) |
                   {"room_portal"})
    if karma:
        cases = sorted(os.path.basename(p)[:-5] for p in os.listdir(HERE) if p.endswith(".usda") and not p.startswith("geo_"))
    images = {}
    for case in cases:
        exr = os.path.join(args.out, case + ".exr")
        proc = run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s",
                    "/Render/rendersettings", "-o", exr, os.path.join(HERE, case + ".usda")], env, args.out)
        check(proc.returncode == 0, f"husk {case}.usda exits with 0")
        px = read(exr, np, oiio)
        if px is None:
            check(False, f"{case}: no image")
            continue
        images[case] = px

    def report(table, name, value):
        if karma:
            print(f"     {table} {name}: {fmt(value) if hasattr(value, '__len__') else '%.5f' % value}")

    for case in sorted(images):
        px = images[case]
        h, w = px.shape[:2]
        if case.startswith(("overlap", "contain", "limit")):
            value = px[h // 4:3 * h // 4, w // 4:3 * w // 4].reshape(-1, 3).mean(axis=0)
            report("RGB", case, value)
            if not karma and case in KARMA_RGB:
                check(near(value, KARMA_RGB[case]), f"{case}: Nova {fmt(value)}, Karma {fmt(KARMA_RGB[case])}")
        elif case.startswith("oblique"):
            value = edge(px, np)
            report("EDGE", case, value if value is not None else float("nan"))
            if not karma and case in KARMA_EDGE:
                check(value is not None and abs(value - KARMA_EDGE[case]) <= 0.04,
                      f"{case}: edge at {value}, Karma {KARMA_EDGE[case]:.3f} (units, +-0.04)")
        elif case.startswith("shadow"):
            value = shadow_ratio(px)
            report("SHADOW", case, value)
            if not karma and case in KARMA_SHADOW:
                check(near(value, KARMA_SHADOW[case]),
                      f"{case}: shadow / lit {fmt(value)}, Karma {fmt(KARMA_SHADOW[case])}")
        else:
            value = px.reshape(-1, 3).mean(axis=0)
            report("MEAN", case, value)
            if not karma and case in KARMA_MEAN:
                check(near(value, KARMA_MEAN[case], 0.03, 3e-4),
                      f"{case}: mean {fmt(value)}, Karma {fmt(KARMA_MEAN[case])}")
    reference = images.get("room_portal")
    if reference is not None:
        for case in sorted(images):
            if not case.endswith("_low"):
                continue
            value = float(np.sqrt(((images[case] - reference) ** 2).mean()))
            report("NOISE", case, value)
    if not karma and reference is not None and "room_low" in images and "room_portal_low" in images:
        noise = {c: float(np.sqrt(((images[c] - reference) ** 2).mean())) for c in images if c.endswith("_low")}
        # Without a portal the window's light is found by BSDF rays alone;
        # with it Nova samples the window (Karma: x0.21).
        ratio = noise["room_portal_low"] / noise["room_low"]
        check(ratio < 0.4, f"portal noise x{ratio:.3f} of the room without one at 8 samples (Karma x"
                           f"{KARMA_NOISE['room_portal_low'] / KARMA_NOISE['room_low']:.3f})")
        for c in ("room_portal_env_low", "room_portal_portal_low"):
            if c in noise:
                print(f"     {c}: noise x{noise[c] / noise['room_low']:.3f} (Karma x"
                      f"{KARMA_NOISE[c] / KARMA_NOISE['room_low']:.3f})")
    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
