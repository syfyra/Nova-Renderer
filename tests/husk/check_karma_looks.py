"""husk checks of Karma's outlines and look-dev overrides in Nova
(docs/design/M11-P9a.md).

The scenes in karma_looks/ hold what Houdini's own LOPs author: a Karma
Render Settings LOP (karmarendersettings) with its outline parameters or
Simplified Shading, the Karma properties it does not show (override_lighting,
head_*, autoheadlight) and a Render Geometry Settings LOP's outline radius,
over small geometry layers (karma_looks/build_karma_looks.py writes them with
an isolated hython). Nova renders them through husk; the values are compared
with Karma CPU's, measured once with isolated husk at the scenes' sampling
(64 Path Traced samples, uniform oracle) and stored below, as
check_karma_props.py does:

- edge: contour lines of an emissive card over a backdrop: the line's mass
  on each side (the outline AOV's alpha summed across the edge, the mean
  over rows), the beauty next to the edge, with the default stencil, fit,
  intensity, colour, three stencils, the random pattern, depth blend, apply
  off and the default radius;
- step: depth lines of a step in one object (and the crease the plane test
  finds on its far side) and crease lines of a convex 45 degree fold, in
  their line-type AOVs;
- look: the lighting overrides (Emissive Objects, Headlight, Dome Light,
  autoheadlight) and the simplified shading (occlusion on, off, limited,
  depth cue fog) over cards turned to the camera, a floor, a wall and an
  emissive panel.

A check passes within 3 % of Karma's value (plus a small absolute margin),
or within the stated tolerance.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_karma_looks.py --hfs ... --stage ... --out ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "karma_looks")

P = 36.0 / 50.0 / 320.0  # tan per pixel at the image centre


def px(x, y, z):
    """Image column and row of a world point (the camera at the origin)."""
    return 160 + x / (-z) / P, 120 - y / (-z) / P


def box(c, r, h=2):
    return slice(int(r) - h, int(r) + h + 1), slice(int(c) - h, int(c) + h + 1)


# Look-dev regions (rows, columns): the cards' centres, the floor, a low
# spot of the wall, the panel.
LOOK = {
    "q0": box(*px(-2.4, 0, -8)), "q30": box(*px(-1.2, 0, -8)), "q45": box(*px(0, 0, -8)),
    "q60": box(*px(1.2, 0, -8)), "floor": box(*px(0.6, -2, -10)), "wall": box(*px(1.8, -1.9, -12), 1),
    "panel": box(*px(-1.25, 2.0, -9)),
}

# (case, measure, Karma's value[, relative tolerance]). Karma CPU 22.0.466,
# 64 samples, measured 2026-11-08 (docs/design/M11-P9a.md).
KARMA = [
    # Contour lines (the line's mass: R / pi on each side, R = 8.9 pixels).
    ("edge_base", "line_front", 2.83698), ("edge_base", "line_back", 2.84160),
    ("edge_base", "beauty_front", 0.46983), ("edge_base", "beauty_back", 0.24876),
    ("edge_base", "thickness", 0.02000, 0.01),
    ("edge_fit", "line_front", 3.08112), ("edge_fit", "line_back", 3.08883),
    ("edge_fit", "beauty_front", 0.47060), ("edge_fit", "beauty_back", 0.25051),
    ("edge_intensity", "line_front", 1.41849), ("edge_intensity", "line_back", 1.42080),
    ("edge_intensity", "beauty_front", 0.43488), ("edge_intensity", "beauty_back", 0.17441),
    ("edge_color", "line_front", 2.83698), ("edge_color", "line_back", 2.84160),
    ("edge_color", "beauty_front", 0.38249), ("edge_color", "beauty_back", 0.16127),
    # Three rings: 0.81 of one ring's mass; the debug thickness the mean radius.
    ("edge_stencils", "line_front", 2.29739), ("edge_stencils", "line_back", 2.29811),
    ("edge_stencils", "beauty_front", 0.45409), ("edge_stencils", "beauty_back", 0.21513),
    ("edge_stencils", "thickness", 0.01619, 0.01),
    # The random pattern: radii uniform in [0, pi/2 R), mass R / 4.
    ("edge_random", "line_front", 2.22419), ("edge_random", "line_back", 2.22751),
    ("edge_random", "beauty_front", 0.45055), ("edge_random", "beauty_back", 0.20773),
    # Depth blend 1 at radius 0.2: R / (1 + t), t = 5 and 10.
    ("edge_blend", "line_front", 4.71081), ("edge_blend", "line_back", 2.57699),
    ("edge_blend", "beauty_front", 0.50029), ("edge_blend", "beauty_back", 0.23218),
    ("edge_blend", "thickness", 0.03268, 0.02),
    ("edge_noapply", "line_front", 2.83698), ("edge_noapply", "beauty_front", 0.39991),
    ("edge_noapply", "beauty_back", 0.10003),
    # The default radius 0.002: 0.9 pixels.
    ("edge_default", "line_front", 0.28522, 0.05), ("edge_default", "line_back", 0.28271, 0.05),
    ("edge_default", "beauty_front", 0.40066), ("edge_default", "thickness", 0.00200, 0.01),
    # A step in one object: depth lines on both sides, the far side's
    # crease (plane test), no contour.
    ("step_lines", "depth_step", 5.70318), ("step_lines", "crease_step_far", 2.77896),
    ("step_lines", "contour_step", 0.0),
    # A convex 45 degree fold: Karma flags fewer points near the fold than
    # every point across it (docs/design/M11-P9a.md 2): Nova's crease line
    # is denser, +21 %.
    ("step_lines", "crease_fold", 4.66959, 0.25), ("step_lines", "line_fold", 4.66959, 0.25),
    # Lights as on the stage, overridden: emissive objects only, the
    # headlight (= autoheadlight without lights), a white dome.
    ("look_base", "q0", 0.28790), ("look_base", "q30", 0.19559), ("look_base", "q45", 0.13961),
    ("look_base", "q60", 0.08712), ("look_base", "floor", 0.36239), ("look_base", "wall", 0.31383),
    ("look_emissive", "q0", 0.0), ("look_emissive", "q45", 0.0), ("look_emissive", "floor", 0.00825, 0.25),
    ("look_emissive", "wall", 0.00208, 0.25), ("look_emissive", "panel", 2.0),
    ("look_headlight", "q0", 0.38196), ("look_headlight", "q30", 0.33402), ("look_headlight", "q45", 0.27926),
    ("look_headlight", "q60", 0.21134), ("look_headlight", "floor", 0.11904), ("look_headlight", "wall", 0.38404),
    ("look_auto", "q0", 0.38196), ("look_auto", "q45", 0.27926), ("look_auto", "floor", 0.11904),
    ("look_auto", "wall", 0.38404),
    ("look_dome", "q0", 0.48588), ("look_dome", "q30", 0.44989), ("look_dome", "q45", 0.41351),
    ("look_dome", "q60", 0.39497), ("look_dome", "floor", 0.43690), ("look_dome", "wall", 0.39761),
    # Simplified shading: display colour x facing ratio x occlusion (+ the
    # panel's emission), without occlusion, occluders within 5, fog.
    ("look_simple", "q0", 0.29239), ("look_simple", "q30", 0.25472), ("look_simple", "q45", 0.17373),
    ("look_simple", "q60", 0.07913), ("look_simple", "floor", 0.05209), ("look_simple", "wall", 0.23578),
    ("look_simple", "panel", 2.25146),
    ("look_simple_noao", "q0", 0.47883), ("look_simple_noao", "q30", 0.46523), ("look_simple_noao", "q45", 0.35316),
    ("look_simple_noao", "q60", 0.18317), ("look_simple_noao", "floor", 0.09748), ("look_simple_noao", "wall", 0.48852),
    ("look_simple_noao", "panel", 2.35510),
    ("look_simple_aodist", "q0", 0.35017), ("look_simple_aodist", "q30", 0.30734), ("look_simple_aodist", "q45", 0.21609),
    ("look_simple_aodist", "q60", 0.10822), ("look_simple_aodist", "floor", 0.06723),
    ("look_simple_aodist", "wall", 0.24509),
    ("look_simple_fog", "q0", 0.27750), ("look_simple_fog", "q30", 0.24356), ("look_simple_fog", "q45", 0.17225),
    ("look_simple_fog", "q60", 0.08912), ("look_simple_fog", "floor", 0.08525), ("look_simple_fog", "wall", 0.20851),
    ("look_simple_fog", "panel", 2.24820),
]


def measures(case, layers, np):
    """The case's values: line masses, beauty means, region means."""
    out = {}
    C = layers.get("C")
    if C is None:
        return out
    rgb = C[..., :3].mean(axis=2)

    def mass(layer, rows, c0, c1):
        a = layers.get(layer)
        return None if a is None else float(a[rows, c0:c1, 3].mean(axis=0).sum())

    if case.startswith("edge"):
        rows = slice(60, 180)
        out["line_front"] = mass("outline", rows, 130, 160)
        out["line_back"] = mass("outline", rows, 160, 190)
        out["beauty_front"] = float(rgb[rows, 150:159].mean())
        out["beauty_back"] = float(rgb[rows, 161:170].mean())
        t = layers.get("export_outline_thickness")
        if t is not None:
            out["thickness"] = float(t[rows, 40:100, 0].mean())
    elif case.startswith("step"):
        top, bottom = slice(30, 100), slice(140, 210)
        out["depth_step"] = mass("outline_depth", top, 130, 190)
        out["crease_step_far"] = mass("outline_crease", top, 160, 190)
        out["contour_step"] = mass("outline_contour", top, 130, 190)
        out["crease_fold"] = mass("outline_crease", bottom, 130, 190)
        out["line_fold"] = mass("outline", bottom, 130, 190)
    else:
        for name, (r, c) in LOOK.items():
            out[name] = float(rgb[r, c].mean())
    return {k: v for k, v in out.items() if v is not None}


def read(path, np, oiio):
    image = oiio.ImageInput.open(path)
    if not image:
        return {}
    layers = {}
    part = 0
    while image.seek_subimage(part, 0):
        spec = image.spec()
        px_ = np.asarray(image.read_image(part, 0, 0, spec.nchannels, oiio.FLOAT)).reshape(spec.height, spec.width, -1)
        name = spec.getattribute("name") or "C"
        groups = {}
        for i, ch in enumerate(spec.channelnames):
            groups.setdefault(ch.rsplit(".", 1)[0] if "." in ch else name, []).append(i)
        for prefix, idx in groups.items():
            layers[prefix] = px_[..., idx]
        part += 1
    image.close()
    return layers


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--renderer", default="Nova", help="BRAY_HdKarma re-measures Karma")
    parser.add_argument("--cases", nargs="*", help="only these cases")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)
    karma = args.renderer.startswith("BRAY")
    cases = sorted(f[:-5] for f in os.listdir(HERE) if f.endswith(".usda") and not f.startswith("geo_"))
    if args.cases:
        cases = [c for c in cases if c in args.cases]
    values = {}
    for case in cases:
        exr = os.path.join(args.out, case + ".exr")
        if os.path.exists(exr):
            os.remove(exr)
        proc = run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s",
                    "/Render/rendersettings", "-o", exr, os.path.join(HERE, case + ".usda")], env, args.out)
        check(proc.returncode == 0, f"husk {case}.usda exits with 0")
        layers = read(exr, np, oiio)
        if not layers:
            check(False, f"{case}: no image")
            continue
        for name, v in measures(case, layers, np).items():
            values[(case, name)] = v
    if karma:
        for (case, name), v in sorted(values.items()):
            print(f'    ("{case}", "{name}", {v:.5f}),')
    for entry in KARMA:
        case, name, value = entry[:3]
        tolerance = entry[3] if len(entry) > 3 else 0.03
        got = values.get((case, name))
        if got is None or karma:
            continue
        ok = abs(got - value) <= tolerance * abs(value) + 2e-3
        check(ok, f"{case} {name}: Nova {got:.5f}, Karma {value:.5f}")
    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
