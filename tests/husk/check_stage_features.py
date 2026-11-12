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

            # 3. Render passes: cards of 40 x 40 pixels at columns 20, 60 and 100, the wall's strip in rows 55 to 65.
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
