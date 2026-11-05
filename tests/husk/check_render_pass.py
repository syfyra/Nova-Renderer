"""husk checks of the render pass: framing, restarts and the depth camera.

tests/scenes/framing_offset.usda: aspect ratio conform policies with a film
back offset. Karma scales the offset with the conformed aperture (measured
with husk --renderer BRAY_HdKarma); Nova must place the marker's centroid at
the same pixel for expandAperture and cropAperture, and at the unconformed
position when the aspects match.
tests/scenes/resync.usda: the camera shutter and nova:subdivLevel change in
the same frame; the subdivided cube must be rebuilt once, at the new level
(TF_DEBUG=HDNOVA_SUBDIV reports every build).
tests/scenes/resync_budget.usda: a smaller subdivision budget in frame 2
rebuilds only the mesh whose tessellation rate changes.
tests/scenes/depth_shutter.usda: the depth AOV comes from the camera the
first-hit rays start from, also when the shutter excludes the frame.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_render_pass.py ...
"""

import argparse
import os
import re
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

    def husk(settings, scene, env, output, extra=()):
        cmd = [tool(args, "husk.exe"), "--renderer", "Nova", "--settings", settings, "-o", output, *extra,
               os.path.join(args.scenes, scene)]
        proc = run(cmd, env, args.out)
        check(proc.returncode == 0, f"husk {scene} {settings} exits with 0")
        return proc

    def read(path, subimage=0):
        image = oiio.ImageInput.open(path)
        image.seek_subimage(subimage, 0)
        data = image.read_image(oiio.FLOAT)
        image.close()
        return data  # rows top-down

    env, prefs = isolated_env(args)
    try:
        # --- Conform policies with a film back offset: marker centroid (x right, y down).
        for settings, expected in (("control", (300.0, 225.0)), ("expand", (200.0, 200.0)),
                                   ("crop", (233.333, 225.0)), ("wide", (250.0, 112.5))):
            out = os.path.join(args.out, f"framing_{settings}.exr")
            husk(f"/Render/{settings}", "framing_offset.usda", env, out)
            alpha = read(out)[..., 3]
            ys, xs = np.indices(alpha.shape)
            total = float(alpha.sum())
            centroid = (float((alpha * (xs + 0.5)).sum()) / total, float((alpha * (ys + 0.5)).sum()) / total)
            worst = max(abs(c - e) for c, e in zip(centroid, expected))
            check(worst < 0.25, f"{settings}: the marker lands where Karma puts it: "
                                f"({centroid[0]:.2f}, {centroid[1]:.2f}) vs {expected}")

        # --- Shutter and subdivision level change together at frame 2.
        debug_env = dict(env, TF_DEBUG="HDNOVA_SUBDIV")
        proc = husk("/Render/settings", "resync.usda", debug_env, os.path.join(args.out, "resync.$F.exr"),
                    ("--frame", "1", "--frame-count", "2"))
        builds = [int(r) for r in re.findall(r"/World/sub geometry built \(subdivision rate (\d+)\)",
                                             proc.stdout + proc.stderr)]
        check(builds == [0, 4], f"the cube is built once per frame, at the current level: rates {builds} == [0, 4]")

        # --- A budget change rebuilds only the meshes whose rate changes.
        proc = husk("/Render/settings", "resync_budget.usda", debug_env,
                    os.path.join(args.out, "resync_budget.$F.exr"), ("--frame", "1", "--frame-count", "2"))
        builds = {}
        for path, rate in re.findall(r"(/World/\w+) geometry built \(subdivision rate (\d+)\)", proc.stdout + proc.stderr):
            builds.setdefault(path, []).append(int(rate))
        expected = {"/World/small": [4], "/World/big": [8, 4], "/World/poly": [0]}
        check(builds == expected, f"budget change: builds {builds} == {expected}")

        # --- Depth from the camera of the first-hit rays.
        for settings, expected in (("late", 9.75), ("centred", 10.0)):
            out = os.path.join(args.out, f"depth_{settings}.exr")
            husk(f"/Render/{settings}", "depth_shutter.usda", env, out, ("--frame", "1"))
            depth = float(read(out, 1)[16, 16, 0])
            check(abs(depth - expected) < 1e-3, f"{settings} shutter: depth {depth:.4f} == {expected}")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
