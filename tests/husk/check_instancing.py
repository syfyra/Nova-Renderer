"""husk check of native Hydra instancing (tests/scenes/instancing.usda).

Renders the scene with Nova and checks the first-hit AOVs, whose pixel-centre
coverage is exact by construction of the scene:
  * the number of covered pixels (1512),
  * instanceId and albedo at probe pixels (quath and quatf rotations,
    scales, the prototype transform, invisibleIds, nested and native
    instancing),
  * the summed alpha of the beauty (filter-invariant area check).

Needs numpy and OpenImageIO, so run it with Houdini's Python (a plain
interpreter, no license): $HFS/python313/python.exe -I check_instancing.py ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, husk, isolated_env  # noqa: E402  (shared husk helpers)

RED, BLUE, YELLOW, GREEN, ORANGE = (0.8, 0.1, 0.1), (0.1, 0.1, 0.8), (0.8, 0.8, 0.1), (0.1, 0.8, 0.1), (0.8, 0.4, 0.1)

# EXR pixel (x, y), y down -> (instanceId, albedo); -1 = background.
PROBES = {
    (51, 56): (0, RED),       # PI Tri, USD index 0
    (76, 48): (1, RED),       # PI Tri, index 1, quath +90 deg
    (30, 88): (2, RED),       # PI Tri, index 3, scale (1, 3, 1), +90 deg
    (120, 60): (0, BLUE),     # PI Sq, index 2, scale 2
    (80, 100): (-1, None),    # PI index 4 is in invisibleIds
    (163, 166): (0, YELLOW),  # PIf Tri, quatf -90 deg (background if the rotation were lost or misread)
    (159, 119): (2, GREEN),   # nested dot k = 0 * 3 + 2
    (144, 134): (4, GREEN),   # nested dot k = 1 * 3 + 1
    (39, 159): (0, ORANGE),   # native instance NI_A
    (69, 159): (1, ORANGE),   # native instance NI_B, scale 1.4
    (100, 20): (-1, None),    # background
}
COVERED_PIXELS = 1512
# Half-float quaternions are not unit length; USD does not normalize them, so
# the rotated triangles shrink slightly: sum of areas 1511.83 px.
COVERED_AREA = 1511.83

# tests/scenes/instancing_colors.usda: EXR pixel (x, y) -> expected albedo.
R, G, B, Y, C, O = (1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0), (0, 1, 1), (0.8, 0.4, 0.1)
COLOR_PROBES = {
    (40, 40): R, (60, 40): G, (80, 40): B,                  # PointInstancer vertex displayColor
    (120, 40): O, (140, 40): O,                             # the prototype's own color wins
    (40, 80): Y, (60, 80): C,                               # native instances with constant colors
    (40, 120): R, (60, 120): R, (40, 150): B, (60, 150): B,  # outer level's colors
    (120, 120): G, (140, 120): Y, (120, 150): G, (140, 150): Y,  # inner level wins
}


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

    def read(path, subimage):
        image = oiio.ImageInput.open(path)
        image.seek_subimage(subimage, 0)
        integer = image.spec().format.basetype in (oiio.UINT, oiio.INT)
        data = image.read_image(oiio.UINT if integer else oiio.FLOAT)
        image.close()
        return data.view(np.int32) if integer else data

    env, prefs = isolated_env(args)
    try:
        output = os.path.join(args.out, "instancing.exr")
        husk(args, "/Render/settings", "instancing.usda", env, output)
        color, albedo, instance = read(output, 0), read(output, 1), read(output, 2)[..., 0]

        covered = int((instance >= 0).sum())
        check(covered == COVERED_PIXELS, f"{covered} pixels are covered by instances (expected {COVERED_PIXELS})")
        for (x, y), (k, rgb) in PROBES.items():
            got = int(instance[y, x])
            check(got == k, f"instanceId at ({x}, {y}) = {got}, expected {k}")
            if rgb:
                a = [round(float(v), 4) for v in albedo[y, x][:3]]
                check(all(abs(v - e) < 1e-3 for v, e in zip(a, rgb)), f"albedo at ({x}, {y}) = {a}, expected {rgb}")
        alpha = float(color[..., 3].sum())
        check(abs(alpha - COVERED_AREA) < 5.0, f"summed alpha {alpha:.2f} ~ covered area {COVERED_AREA}")

        # Instance-rate displayColor.
        output = os.path.join(args.out, "instancing_colors.exr")
        husk(args, "/Render/settings", "instancing_colors.usda", env, output)
        albedo = read(output, 1)
        for (x, y), rgb in COLOR_PROBES.items():
            a = [round(float(v), 4) for v in albedo[y, x][:3]]
            check(all(abs(v - e) < 1e-3 for v, e in zip(a, rgb)), f"instance color at ({x}, {y}) = {a}, expected {rgb}")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
