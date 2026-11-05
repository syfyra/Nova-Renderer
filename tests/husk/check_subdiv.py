"""husk check of subdivision surfaces (tests/scenes/subdiv.usda).

The center rows of an orthographic view of a Catmull-Clark unit cube must be
covered by the limit surface, 2 * 34/81 units = 209.877 px, at husk's default
complexity (refineLevel 8, subdivision level 3), and by the control hull,
250 px, at --complexity medium (refineLevel 2), as in Karma. A cube with all
edges infinitely sharp stays a cube with flat faces. A subdivision surface
with moving points is blurred and keeps its coverage.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_subdiv.py ...
"""

import argparse
import math
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

LIMIT_PX = 2.0 * 34.0 / 81.0 * 250.0  # 209.877
HULL_PX = 250.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--scenes", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import OpenImageIO as oiio

    def render(settings, name, extra=(), frame=1, run_env=None):
        image = os.path.join(args.out, name + ".exr")
        cmd = [tool(args, "husk.exe"), "--renderer", "Nova", "--settings", settings, "-o", image]
        if frame is not None:
            cmd += ["--frame", str(frame)]
        cmd += list(extra) + [os.path.join(args.scenes, "subdiv.usda")]
        proc = run(cmd, run_env or env, args.out)
        check(proc.returncode == 0, f"husk subdiv.usda {settings} {' '.join(extra)} exits with 0")
        return oiio.ImageBuf(image).get_pixels(oiio.FLOAT), proc.stdout + proc.stderr

    def coverage(pixels):
        """Alpha summed over the two center rows (px)."""
        return float(pixels[149:151, :, 3].sum()) / 2.0

    env, prefs = isolated_env(args)
    try:
        smooth, log = render("/Render/smooth", "smooth", run_env=dict(env, TF_DEBUG="HDNOVA_SUBDIV"))
        c = coverage(smooth)
        check(abs(c - LIMIT_PX) < 0.5, f"default complexity renders the limit surface: {c:.2f} px (expected {LIMIT_PX:.2f})")
        check("/World/smooth refineLevel 8 -> subdivision level 3" in log,
              "husk's default complexity (refineLevel 8) selects subdivision level 3")
        check("/World/implicitBall refineLevel" not in log,
              "UsdGeom spheres are analytic, not subdivided meshes (no subdivision decision for them)")

        hull, _ = render("/Render/smooth", "smooth_medium", ("--complexity", "medium"))
        c = coverage(hull)
        check(abs(c - HULL_PX) < 0.5, f"--complexity medium renders the control hull: {c:.2f} px (expected {HULL_PX:.2f})")

        creased, _ = render("/Render/creased", "creased")
        c = coverage(creased)
        check(abs(c - HULL_PX) < 0.5, f"infinitely sharp creases keep the cube: {c:.2f} px (expected {HULL_PX:.2f})")
        face = creased[100:200, 100:200, :3].mean(axis=(0, 1))
        expected = 0.5 / math.pi
        check(all(abs(float(v) - expected) < 1e-3 for v in face),
              f"creased front face is flat-shaded: {[round(float(v), 5) for v in face]} (expected {expected:.5f})")

        moving, _ = render("/Render/moving", "moving")
        c = coverage(moving)
        check(abs(c - LIMIT_PX) < 1.0, f"moving limit surface keeps its coverage: {c:.2f} px (expected {LIMIT_PX:.2f})")
        row = moving[149:151, :, 3].mean(axis=0)
        partial = int(((row > 0.02) & (row < 0.98)).sum())
        # The points move 0.25 units = 62.5 px during the shutter: two ramps of
        # about 62 px each instead of two partially covered pixels.
        check(110 <= partial <= 135, f"moving limit surface is motion blurred: {partial} partially covered px (~125)")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
