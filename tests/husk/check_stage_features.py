"""husk checks of stage features that Nova ignored or approximated (docs/bug-audit-2026-11.md, section 16).

tests/scenes/concave.usda: concave polygons. Hydra's triangulation is a fan
from a face's first vertex, which covers what a concave face does not; a
displaced face was patched around its centre, with the same result. The
covered pixels of a U, a star and a displaced L must be their areas.

tests/scenes/passes.usda: render passes (husk --pass). The pass's
collections prune prims, hide them from the renderer or from the camera and
make them mattes; before, every pass rendered the whole scene.

tests/scenes/implicit.usda: cylinders, cones and capsules. They were the
ten-sided cage UsdImaging makes of them; seen along its axis each must cover
its disc.

tests/scenes/default_material.usda: geometry without a material is Karma's
plastic (section 17): 0.4 of the display colour and a Phong highlight, under
a dome and under a distant light, against the values Karma CPU renders.

tests/scenes/shadow_api.usda: UsdLux ShadowAPI (section 20): shadows off,
a shadow colour, a shadow distance and its falloff, against Karma CPU.

tests/scenes/display_opacity.usda: displayOpacity on geometry without a
material, and an opacity that is a geompropvalue itself (which Nova XPU
rendered opaque).

tests/scenes/quad_split.usda: polygons are split from their last vertex,
as in Karma.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_stage_features.py ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--scenes", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)

    def husk(renderer, scene, output, extra):
        if os.path.exists(output):
            os.remove(output)
        cmd = [tool(args, "husk.exe"), "--renderer", renderer, "-o", output, *extra, os.path.join(args.scenes, scene)]
        proc = run(cmd, env, args.out)
        check(proc.returncode == 0 and os.path.exists(output), f"husk {scene} {' '.join(extra)} ({renderer}) writes its image")
        if not os.path.exists(output):
            return None
        inp = oiio.ImageInput.open(output)
        px = inp.read_image(0, 0, 0, 4, oiio.FLOAT)
        inp.close()
        return px

    try:
        for renderer, label in (("Nova", "CPU"), ("Nova XPU", "XPU")):
            tag = label.lower()

            # 1. Concave polygons: 400 pixels per unit of area, the shapes in thirds of the image.
            px = husk(renderer, "concave.usda", os.path.join(args.out, f"concave_{tag}.exr"), ["--settings", "/Render/rs"])
            if px is not None:
                for name, columns, area in (("U (a fan covers 20)", slice(0, 120), 11.0), ("star (7.8)", slice(120, 220), 5.1727),
                                            ("displaced L (8.4 around its centre)", slice(220, 320), 7.0)):
                    covered = float(px[:, columns, 3].sum()) / 400.0
                    check(abs(covered - area) < 0.01 * area, f"{label}: the {name} covers its area {area:.3f}: {covered:.3f}")

            # 2. Cylinder, cone and capsule along their axes: discs, 1600 pixels per unit of area.
            px = husk(renderer, "implicit.usda", os.path.join(args.out, f"implicit_{tag}.exr"), ["--settings", "/Render/rs"])
            if px is not None:
                for name, columns, radius in (("cylinder", slice(0, 160), 1.0), ("cone", slice(160, 320), 1.2),
                                              ("capsule", slice(320, 480), 0.75)):
                    covered, disc = float(px[:, columns, 3].sum()) / 1600.0, float(np.pi) * radius * radius
                    check(abs(covered - disc) < 0.003 * disc,
                          f"{label}: the {name} covers the disc of its radius, {disc:.4f}: {covered:.4f} (ten sides: {0.9355 * disc:.4f})")

            # 3. Geometry without a material: the left plane has a display colour of 0.8, the right none.
            # What Karma CPU renders, and what the model says (diffuse 0.4 c, Phong 1/30 of exponent 5).
            for name, karma, model in (("dome", (0.3536, 0.4337), (0.4 * 0.8 + 1 / 30, 0.4 + 1 / 30)),
                                       ("zenith", (0.4199, 0.5000), (0.4 * 0.8 + 0.1, 0.4 + 0.1)),
                                       ("off", (0.3259, None), (0.32 * np.sin(np.pi / 3) + 0.1 * np.cos(np.pi / 6) ** 5, None)),
                                       ("dome_matte", (None, None), (0.8, 0.18))):
                px = husk(renderer, "default_material.usda", os.path.join(args.out, f"default_{name}_{tag}.exr"),
                          ["--pass", "/Render/" + name])
                if px is None:
                    continue
                for side, columns, k, m in (("display colour 0.8", slice(10, 70), karma[0], model[0]),
                                            ("no display colour", slice(90, 150), karma[1], model[1])):
                    if m is None:
                        continue
                    got = float(px[10:110, columns, :3].mean())
                    check(abs(got - m) < 0.004 and (k is None or abs(got - k) < 0.006),
                          f"{label} unbound, {name}, {side}: {got:.4f} (the model {m:.4f}" + (f", Karma {k:.4f})" if k else ")"))

            # 3b. UsdLux ShadowAPI (section 20): the ground in the card's shadow and in the open, per sun.
            # What Karma CPU renders in the strip, and the rule (the card is sqrt 2 along the shadow rays).
            lit = 0.5 * float(np.cos(np.pi / 4))
            t = (float(np.sqrt(2.0)) - 0.5) / 1.5  # the card's share of the falloff zone 0.5 to 2
            for name, karma, rule in (("plain", (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)),
                                      ("off", (0.3537, 0.3537, 0.3537), (lit, lit, lit)),
                                      ("tinted", (0.3536, 0.0884, 0.0), (lit, 0.25 * lit, 0.0)),
                                      ("near", (0.3535, 0.3535, 0.3535), (lit, lit, lit)),
                                      ("fade", (0.2155, 0.2155, 0.2155), (lit * t,) * 3),
                                      ("fade2", (0.2996, 0.2996, 0.2996), (lit * (1.0 - (1.0 - t) ** 2),) * 3)):
                px = husk(renderer, "shadow_api.usda", os.path.join(args.out, f"shadow_{name}_{tag}.exr"),
                          ["--pass", "/Render/" + name])
                if px is None:
                    continue
                strip = [float(px[45:75, 43:57, ch].mean()) for ch in range(3)]
                open_ = float(px[45:75, 110:150, :3].mean())
                check(abs(open_ - lit) < 0.004, f"{label} ShadowAPI {name}: the open ground {open_:.4f} ({lit:.4f})")
                check(all(abs(strip[ch] - rule[ch]) < 0.006 and abs(strip[ch] - karma[ch]) < 0.006 for ch in range(3)),
                      f"{label} ShadowAPI {name}: in the card's shadow ({strip[0]:.4f}, {strip[1]:.4f}, {strip[2]:.4f}), "
                      f"Karma ({karma[0]:.4f}, {karma[1]:.4f}, {karma[2]:.4f})")

            # 3c. Opacity from a primvar (section 20): displayOpacity on geometry without a material, and a
            # Standard Surface opacity connected straight to a geompropvalue. 20 pixels per unit; regions of
            # 12 x 12 pixels by their first row and column, with what Karma CPU renders there.
            px = husk(renderer, "display_opacity.usda", os.path.join(args.out, f"display_opacity_{tag}.exr"),
                      ["--settings", "/Render/rs"])
            if px is not None:
                for name, row, col, karma in (
                        ("no material: an opacity of 0 casts no shadow", 64, 24, (0.3535, 0.3535)),
                        ("no material: through an opacity of 0, the ground in the next face's shadow", 64, 44, (0.0, 0.0)),
                        ("no material: an opacity of 1 is Karma's plastic", 64, 64, (0.2440, 0.0460)),
                        ("no material: the shadow of an opacity of 1", 44, 24, (0.0, 0.0)),
                        ("no material: an opacity of 0.25 over the lit ground", 44, 64, (0.3261, 0.2766)),
                        ("geompropvalue opacity 0: the ground in the next face's shadow", 64, 124, (0.0, 0.0)),
                        ("geompropvalue opacity 1: the red surface", 64, 144, (0.5657, 0.0707)),
                        ("geompropvalue opacity 0: the lit ground", 44, 144, (0.3535, 0.3535)),
                        ("geompropvalue opacity 1: its shadow", 44, 104, (0.0, 0.0))):
                    got = [float(px[row:row + 12, col:col + 12, ch].mean()) for ch in range(2)]
                    check(all(abs(got[ch] - karma[ch]) < 0.006 for ch in range(2)),
                          f"{label} {name}: ({got[0]:.4f}, {got[1]:.4f}), Karma ({karma[0]:.4f}, {karma[1]:.4f})")

            # 3d. How polygons are split (section 20): a primvar that is 1 at a face's first vertex. Karma
            # fans from the last vertex, so the quad's triangle 1 2 3 and the pentagon's 1 2 4 are black;
            # a fan from the first vertex has 1/3 and more there.
            px = husk(renderer, "quad_split.usda", os.path.join(args.out, f"quad_split_{tag}.exr"),
                      ["--settings", "/Render/rs"])
            if px is not None:
                for name, row, col, karma in (("the quad's triangle 0 1 3", 73, 37, 0.3255), ("the quad's triangle 1 2 3", 47, 63, 0.0),
                                              ("the pentagon's triangle 0 1 4", 82, 150, 0.3665),
                                              ("the pentagon's triangle 1 2 4", 57, 158, 0.0)):
                    got = float(px[row - 2:row + 3, col - 2:col + 3, 0].mean())
                    check(abs(got - karma) < 0.01, f"{label} {name}: {got:.4f} (Karma {karma:.4f})")

            # 3e. Where a volume shader runs (section 20, V-4): a constant emission on a VDB box of 20 voxels
            # of 0.1, 100 pixels per unit. Karma CPU: 2.0996 between x = -1.05 and 1.05, the image's mean 0.7716.
            px = husk(renderer, "volume_mask.usda", os.path.join(args.out, f"volume_mask_{tag}.exr"),
                      ["--headlight", "none", "--settings", "/Render/rs"])
            if px is not None:
                row = px[140:160, :, 0].mean(axis=0)
                plateau = float(row[190:210].mean())
                inside = [i for i in range(len(row)) if row[i] >= 0.5 * plateau]
                lo, hi = (inside[0] / 100.0 - 2.0, (inside[-1] + 1) / 100.0 - 2.0) if inside else (0.0, 0.0)
                mean = float(px[:, :, 0].mean())
                check(abs(plateau - 2.1) < 0.02 and abs(lo + 1.05) < 0.015 and abs(hi - 1.05) < 0.015,
                      f"{label} a constant emission fills the voxels and half a voxel around them: {plateau:.4f} from "
                      f"{lo:.2f} to {hi:.2f} (Karma 2.0996 from -1.05 to 1.05; the cells of 8 voxels reach 1.40)")
                check(abs(mean - 0.7716) < 0.008, f"{label} its image's mean {mean:.4f} (Karma 0.7716)")

            # 4. Render passes: cards of 40 x 40 pixels at columns 20, 60 and 100, the wall's strip in rows 55 to 65.
            images = {}
            for name in ("whole", "prune", "render_visibility", "camera_visibility", "matte"):
                images[name] = husk(renderer, "passes.usda", os.path.join(args.out, f"pass_{name}_{tag}.exr"),
                                    ["--pass", "/Render/" + name])
            if any(image is None for image in images.values()):
                continue

            def alpha(name, columns=slice(None)):
                return float(images[name][:, columns, 3].sum())

            def card(name, first):  # the mean radiance inside a card
                return float(images[name][45:75, first + 5:first + 35, :3].mean())

            a, b, c = slice(20, 60), slice(60, 100), slice(100, 140)
            lit, unlit = 0.5 * (1.0 + 1.0 / np.pi), 0.5  # with the sun and without it
            check(abs(alpha("whole") - 5200.0) < 30.0, f"{label}: the whole scene covers 5200 pixels: {alpha('whole'):.0f}")
            check(abs(card("whole", 60) - lit) < 0.02, f"{label}: a card under the dome and the sun: {card('whole', 60):.3f} ({lit:.3f})")

            # prune: card a and the sun are gone; the wall shows where the card was.
            check(abs(alpha("prune") - 4000.0) < 30.0, f"{label} prune: 4000 pixels without card a: {alpha('prune'):.0f}")
            check(abs(alpha("prune", a) - 400.0) < 10.0, f"{label} prune: the wall alone where the card was: {alpha('prune', a):.0f}")
            check(abs(card("prune", 60) - unlit) < 0.02, f"{label} prune: no sun on card b: {card('prune', 60):.3f} ({unlit:.3f})")

            # renderVisibility: card c (in its group) and the sun are outside it.
            check(abs(alpha("render_visibility") - 4000.0) < 30.0,
                  f"{label} renderVisibility: 4000 pixels without card c: {alpha('render_visibility'):.0f}")
            check(abs(alpha("render_visibility", c) - 400.0) < 10.0,
                  f"{label} renderVisibility: the wall alone where the card was: {alpha('render_visibility', c):.0f}")
            check(abs(card("render_visibility", 60) - unlit) < 0.02,
                  f"{label} renderVisibility: a light outside it is off: {card('render_visibility', 60):.3f} ({unlit:.3f})")

            # cameraVisibility: card a is not seen, but it still shadows the wall behind it.
            check(abs(alpha("camera_visibility", a) - 400.0) < 10.0,
                  f"{label} cameraVisibility: the wall alone where card a was: {alpha('camera_visibility', a):.0f}")
            check(abs(card("camera_visibility", 60) - lit) < 0.02,
                  f"{label} cameraVisibility: the sun stays on card b: {card('camera_visibility', 60):.3f}")
            behind = float(images["camera_visibility"][57:63, 25:55, :3].mean())
            open_ = float(images["prune"][57:63, 25:55, :3].mean())
            check(behind < 0.75 * open_,
                  f"{label} cameraVisibility: the unseen card shadows the wall behind it: {behind:.3f} against {open_:.3f} without it")

            # matte: card b is a hole that hides the wall behind it.
            check(abs(alpha("matte") - 3600.0) < 30.0, f"{label} matte: 3600 pixels: {alpha('matte'):.0f}")
            check(alpha("matte", slice(65, 95)) < 1.0, f"{label} matte: no alpha inside the matte: {alpha('matte', slice(65, 95)):.2f}")
            inside = float(np.abs(images["matte"][:, 65:95, :3]).max())
            check(inside < 1e-4, f"{label} matte: no light inside the matte: {inside:.5f}")
            check(abs(card("matte", 20) - lit) < 0.02, f"{label} matte: the other cards as before: {card('matte', 20):.3f}")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
