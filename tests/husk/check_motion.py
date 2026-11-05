"""husk check of motion blur and depth of field through Hydra.

tests/scenes/motion.usda: transform blur (translate samples) and deformation
blur (point samples with bracketing samples outside the shutter) must give
the analytic trapezoid coverage, and disableMotionBlur a sharp image.
tests/scenes/motion_instancer.usda: the same for moving instances (positions,
instancer and prototype transforms, nested levels), plus rotation blur of
slerped orientations against its analytic radial coverage.
tests/scenes/motion_topology.usda: particles dying and meshes changing their
point count within the shutter must render sharp at their frame values;
motion_topology_late.usda the same for a shutter that excludes the frame.
tests/scenes/dof.usda: a defocused edge must follow the edge-spread function
of the circle of confusion (rho = 22.22 px), an edge on the focus plane and
disableDepthOfField must stay sharp.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_motion.py ...
"""

import argparse
import math
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)


def husk_frame(args, settings, scene, env, output, frame=None):
    cmd = [tool(args, "husk.exe"), "--renderer", "Nova", "--settings", settings, "-o", output]
    if frame is not None:
        cmd += ["--frame", str(frame)]
    cmd.append(os.path.join(args.scenes, scene))
    proc = run(cmd, env, args.out)
    check(proc.returncode == 0, f"husk {scene} {settings} exits with 0")


def trapezoid(x):
    """Coverage of a 2-unit quad whose left edge sweeps [-2.5, 2.5]."""
    lo, hi = max(x - 2.0, -2.5), min(x, 2.5)
    return max(0.0, hi - lo) / 5.0


def spin_coverage(r):
    """Time coverage at distance r of a 2 x 2 square turning 90 degrees."""
    if r <= 1.0:
        return 1.0
    if r >= math.sqrt(2.0):
        return 0.0
    return 4.0 / math.pi * math.asin(1.0 / r) - 1.0


def disk_esf(d):
    """Edge-spread function of a uniform disk, d = signed distance / radius."""
    c = max(-1.0, min(1.0, d))
    return 0.5 + (math.asin(c) + c * math.sqrt(1.0 - c * c)) / math.pi


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

    def alpha(path):
        image = oiio.ImageInput.open(path)
        data = image.read_image(oiio.FLOAT)
        image.close()
        return data[..., 3]  # rows top-down

    env, prefs = isolated_env(args)
    try:
        # --- Motion blur at frame 1, shutter [-0.25, 0.25]
        out = os.path.join(args.out, "motion.exr")
        husk_frame(args, "/Render/settings", "motion.usda", env, out, frame=1)
        a = alpha(out)
        for name, rows in (("transform", range(12, 28)), ("deformation", range(52, 68))):
            profile = a[list(rows)].mean(axis=0)
            worst = max(abs(float(profile[c]) - trapezoid(-20.0 + (c + 0.5) / 10.0)) for c in range(400))
            check(worst < 0.03, f"{name} blur follows the swept coverage (max deviation {worst:.4f})")
            area = float(profile.sum()) / 10.0
            check(abs(area - 2.0) < 0.05, f"{name} blur conserves the quad's width ({area:.3f} units of 2)")

        out = os.path.join(args.out, "nomotion.exr")
        husk_frame(args, "/Render/nomotion", "motion.usda", env, out, frame=1)
        a = alpha(out)
        sharp = a[list(range(12, 28)) + list(range(52, 68))].mean(axis=0)
        expected = [1.0 if 0.0 < -20.0 + (c + 0.5) / 10.0 < 2.0 else 0.0 for c in range(400)]
        worst = max(abs(float(sharp[c]) - expected[c]) for c in range(400))
        check(worst < 1e-6, f"disableMotionBlur renders the frame sharp (max deviation {worst:.2g})")

        # --- Instancer motion at frame 1, shutter [-0.5, 0.5] (motion_instancer.usda)
        out = os.path.join(args.out, "motion_instancer.exr")
        husk_frame(args, "/Render/settings", "motion_instancer.usda", env, out, frame=1)
        a = alpha(out)
        def px(c):
            return -20.0 + (c + 0.5) / 10.0
        def py(r):
            return 6.0 - (r + 0.5) / 10.0
        for name, rows, cols, shift in (("instance positions", range(12, 28), range(0, 400), 0.0),
                                        ("instancer transform", range(52, 68), range(140, 300), 0.0),
                                        ("nested instancers", range(92, 108), range(0, 400), 0.0),
                                        ("prototype transform", range(52, 68), range(20, 160), 12.0)):
            profile = a[list(rows)].mean(axis=0)
            worst = max(abs(float(profile[c]) - trapezoid(px(c) + shift)) for c in cols)
            check(worst < 0.03, f"{name} blur follows the swept coverage (max deviation {worst:.4f})")
            area = float(profile[list(cols)].sum()) / 10.0
            check(abs(area - 2.0) < 0.05, f"{name} blur conserves the quad's width ({area:.3f} units of 2)")

        # Rotation blur: ring averages around (12, 0) against c(r), with the
        # expectation averaged over 4 x 4 points per pixel.
        bins = {}
        area = 0.0
        for r in range(30, 90):
            for c in range(290, 350):
                x, y = px(c) - 12.0, py(r)
                dist = math.hypot(x, y)
                if dist > 1.6:
                    continue
                expect = sum(spin_coverage(math.hypot(x + (i + 0.5) / 40.0 - 0.05, y + (j + 0.5) / 40.0 - 0.05))
                             for i in range(4) for j in range(4)) / 16.0
                b = bins.setdefault(int(dist * 10.0), [0.0, 0.0, 0])
                b[0] += float(a[r, c])
                b[1] += expect
                b[2] += 1
                area += float(a[r, c])
        worst = max(abs(s - e) / n for s, e, n in bins.values())
        check(worst < 0.02, f"rotation blur follows the swept coverage c(r) (max ring deviation {worst:.4f})")
        check(abs(area / 100.0 - 4.0) < 0.05, f"rotation blur conserves the square's area ({area / 100.0:.3f} of 4)")

        out = os.path.join(args.out, "nomotion_instancer.exr")
        husk_frame(args, "/Render/nomotion", "motion_instancer.usda", env, out, frame=1)
        a = alpha(out)
        boxes = ((0, 2, 3, 5), (0, 2, -1, 1), (0, 2, -5, -3), (-12, -10, -1, 1), (11, 13, -1, 1))
        worst = 0.0
        for r in range(120):
            for c in range(400):
                inside = any(x0 < px(c) < x1 and y0 < py(r) < y1 for x0, x1, y0, y1 in boxes)
                worst = max(worst, abs(float(a[r, c]) - (1.0 if inside else 0.0)))
        check(worst < 1e-6, f"disableMotionBlur renders the instances sharp at the frame (max deviation {worst:.2g})")

        # --- Array lengths changing within the shutter (motion_topology.usda):
        # dying particles and meshes changing topology hold their frame value.
        rows = (((-10, 0, 10), 3.5), ((-10, 0, 10), 0.5), ((-10, 10), -2.5), ((-10,), -5.5))
        boxes = [(x0, x0 + 2, y0, y0 + 2) for xs, y0 in rows for x0 in xs]
        for settings in ("fwd", "ctr"):
            out = os.path.join(args.out, f"motion_topology_{settings}.exr")
            husk_frame(args, f"/Render/{settings}", "motion_topology.usda", env, out, frame=1)
            a = alpha(out)
            worst = 0.0
            for r in range(120):
                for c in range(400):
                    inside = any(x0 < px(c) < x1 and y0 < py(r) < y1 for x0, x1, y0, y1 in boxes)
                    worst = max(worst, abs(float(a[r, c]) - (1.0 if inside else 0.0)))
            check(worst < 1e-6, f"changing array lengths hold the frame, shutter {settings} (max deviation {worst:.2g})")
        # A shutter that excludes the frame (motion_topology_late.usda): the
        # samples inside it agree with each other but not with the frame's
        # topology; the meshes hold their frame points, as in Karma.
        out = os.path.join(args.out, "motion_topology_late.exr")
        husk_frame(args, "/Render/late", "motion_topology_late.usda", env, out, frame=1)
        a = alpha(out)
        boxes = [(x0, x0 + 2, y0, y0 + 2) for xs, y0 in (((-10, 10), 0.5), ((-10,), -2.5)) for x0 in xs]
        worst = 0.0
        for r in range(120):
            for c in range(400):
                inside = any(x0 < px(c) < x1 and y0 < py(r) < y1 for x0, x1, y0, y1 in boxes)
                worst = max(worst, abs(float(a[r, c]) - (1.0 if inside else 0.0)))
        check(worst < 1e-6, f"topology changing outside the shutter holds the frame (max deviation {worst:.2g})")

        # --- Depth of field
        out = os.path.join(args.out, "dof.exr")
        husk_frame(args, "/Render/settings", "dof.usda", env, out)
        a = alpha(out)
        rho = 25.0 * 10.0 * 256.0 / (2.0 * 2.0 * 20.0 * 10.0 * 3.6)
        far = a[7:48].mean(axis=0)
        worst = max(abs(float(far[c]) - disk_esf((c + 0.5 - 128.0) / rho)) for c in range(90, 166))
        check(worst < 0.02, f"defocused edge follows the circle of confusion, rho {rho:.2f} px (max deviation {worst:.4f})")
        near = a[121:127].mean(axis=0)
        check(float(near[127]) == 0.0 and float(near[128]) == 1.0,
              f"edge on the focus plane stays sharp ({float(near[127]):.3f}, {float(near[128]):.3f})")

        out = os.path.join(args.out, "nodof.exr")
        husk_frame(args, "/Render/nodof", "dof.usda", env, out)
        a = alpha(out)
        far = a[7:48].mean(axis=0)
        check(float(far[127]) == 0.0 and float(far[128]) == 1.0,
              f"disableDepthOfField renders a pinhole image ({float(far[127]):.3f}, {float(far[128]):.3f})")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
