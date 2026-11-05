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
tests/scenes/motion_velocity.usda: velocity and acceleration blur of meshes,
curves, points and instances (Nova's and Karma's settings, inherited and on
the render settings) against their swept coverage, on the CPU and Nova XPU.
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


def expected_alpha(rows, cols, inside, times=64, sub=4):
    """Box-filtered coverage per pixel of motion_velocity.usda's frame
    (10 px per unit, x from -20, y from 12 down): the mean over sub x sub
    positions per pixel and `times` shutter times u in (-1, 1) of
    inside(x, y, u) (numpy arrays)."""
    import numpy as np
    r = np.asarray(rows, dtype=float)[:, None, None]
    c = np.asarray(cols, dtype=float)[None, :, None]
    s = (np.arange(sub * sub, dtype=float) % sub + 0.5) / sub
    t = (np.arange(sub * sub, dtype=float) // sub + 0.5) / sub
    x = -20.0 + (c + s[None, None, :]) / 10.0
    y = 12.0 - (r + t[None, None, :]) / 10.0
    total = np.zeros((len(rows), len(cols)))
    for k in range(times):
        u = -1.0 + 2.0 * (k + 0.5) / times
        total += inside(x, y, u).mean(axis=2)
    return total / times


def accel_edge(u, samples=8):
    """The left edge of the accelerated quad, 2.5 u + 2.5 u^2, interpolated
    linearly between `samples` uniform shutter samples (acceleration blur)."""
    import numpy as np
    knots = np.linspace(-1.0, 1.0, samples)
    return float(np.interp(u, knots, 2.5 * knots + 2.5 * knots ** 2))


def check_velocity(args, env, alpha):
    """Velocity and acceleration blur (tests/scenes/motion_velocity.usda,
    docs/design/M8.md D19) against the analytic coverage, on the CPU and Nova XPU."""
    import numpy as np

    def quad(x0, y0, edge):
        return lambda x, y, u: (x >= x0 + edge(u)) & (x <= x0 + 2.0 + edge(u)) & (y >= y0) & (y <= y0 + 2.0)

    def sweep(dist):
        return lambda u: dist * u

    still = lambda u: 0.0

    def spin(cx, cy, degrees):
        def inside(x, y, u):
            a = math.radians(degrees * u)
            dx, dy = x - cx, y - cy
            xr, yr = dx * math.cos(a) + dy * math.sin(a), -dx * math.sin(a) + dy * math.cos(a)
            return (np.abs(xr) <= 1.0) & (np.abs(yr) <= 1.0)
        return inside

    # (name, rows, cols, inside) per object; rows avoid the ends of the curve.
    def objects(default_velocity, frame=False):
        left = range(0, 150)
        right = range(150, 400)
        return [
            ("no setting", range(10, 30), left, quad(-16, 9, sweep(2.5) if default_velocity else still)),
            ("nova:object:velocityBlur", range(10, 30), right, quad(0, 9, sweep(2.5))),
            ("inherited karma:object:vblur", range(50, 70), right, quad(0, 5, sweep(2.5))),
            ("Nova's 0 over Karma's 1", range(50, 70), left, quad(-16, 5, still)),
            # (the 8 samples miss the frame: its own position there)
            ("acceleration blur, 8 samples", range(90, 110), right, quad(0, 1, still if frame else accel_edge)),
            ("velocity blur ignores accelerations", range(90, 110), left, quad(-16, 1, sweep(2.5))),
            ("velocities over time samples", range(130, 150), right, quad(0, -3, sweep(5.0))),
            ("points", range(130, 150), left,
             lambda x, y, u: (x - (-15.0 + 2.5 * u)) ** 2 + (y + 2.0) ** 2 <= 1.0),
            ("curves", range(175, 185), right, lambda x, y, u: np.abs(x - (1.0 + 2.5 * u)) <= 0.5),
            ("instance velocities", range(170, 190), left, quad(-16, -7, sweep(2.5))),
            ("instance angular velocities", range(195, 236), right, spin(12.0, -10.0, 45.0)),
            ("instances without a setting", range(195, 236), left,
             spin(-12.0, -10.0, 45.0 if default_velocity else 0.0)),
        ]

    def compare(a, label, default_velocity=False):
        for name, rows, cols, inside in objects(default_velocity):
            rows, cols = list(rows), list(cols)
            want = expected_alpha(rows, cols, inside)
            got = a[np.ix_(rows, cols)]
            worst = float(np.abs(got.mean(axis=0) - want.mean(axis=0)).max())
            check(worst < 0.03, f"{label}: {name} follows the swept coverage (max column deviation {worst:.4f})")
            ratio = float(got.sum() / max(want.sum(), 1e-9))
            check(abs(ratio - 1.0) < 0.02, f"{label}: {name} conserves its area ({ratio:.4f})")

    for renderer in ("Nova", "HdNovaXpuRendererPlugin"):
        tag = "cpu" if renderer == "Nova" else "xpu"
        out = os.path.join(args.out, f"motion_velocity_{tag}.exr")
        cmd = [tool(args, "husk.exe"), "--renderer", renderer, "--settings", "/Render/settings", "-o", out,
               "--frame", "1", os.path.join(args.scenes, "motion_velocity.usda")]
        proc = run(cmd, env, args.out)
        check(proc.returncode == 0, f"husk motion_velocity.usda ({tag}) exits with 0")
        compare(alpha(out), f"velocity blur ({tag})")

    out = os.path.join(args.out, "motion_velocity_karma_default.exr")
    husk_frame(args, "/Render/karmaDefault", "motion_velocity.usda", env, out, frame=1)
    compare(alpha(out), "Karma's render-settings vblur", default_velocity=True)

    out = os.path.join(args.out, "motion_velocity_nomotion.exr")
    husk_frame(args, "/Render/nomotion", "motion_velocity.usda", env, out, frame=1)
    a = alpha(out)
    # Edges on pixel boundaries: a pixel sample exactly on an edge may fall
    # either way (one of 16), blur would leave whole columns partial.
    worst, total, count = 0.0, 0.0, 0
    for name, rows, cols, inside in objects(False, frame=True):
        if name == "points":
            continue  # the only edge that does not follow pixel boundaries
        rows, cols = list(rows), list(cols)
        want = expected_alpha(rows, cols, lambda x, y, u, f=inside: f(x, y, 0.0), times=1)
        diff = np.abs(a[np.ix_(rows, cols)] - want)
        worst = max(worst, float(diff.max()))
        total += float(diff.sum())
        count += diff.size
    check(worst <= 1.0 / 16.0 + 1e-6 and total / count < 1e-4,
          f"disableMotionBlur renders velocities sharp at the frame (max deviation {worst:.3g}, mean {total / count:.2g})")


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

        check_velocity(args, env, alpha)

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
