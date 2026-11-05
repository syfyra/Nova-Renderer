"""Probes how a renderer maps a latlong dome texture to directions.

A texture whose color encodes its own coordinates (R = u, G = v) is put on a
dome light; a narrow camera at the origin looks in a few directions and the
image average gives (u, v) of that direction. UsdLux (OpenEXR convention)
expects u = 0.5 - atan2(x, z) / (2 pi) and v = acos(y) / pi. Karma only shows
the dome to the camera with karma:light:renderlightgeo.

Usage:
    python probe_dome_mapping.py --hfs <Houdini dir> --stage <staged nova package> --out <dir>
        [--renderer BRAY_HdKarma|Nova]
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

# Camera rotations (rotateXYZ) that look along each direction: a camera
# looks down its local -Z.
DIRECTIONS = {
    "+Z": ((0, 180, 0), (0, 0, 1)),
    "-Z": ((0, 0, 0), (0, 0, -1)),
    "+X": ((0, -90, 0), (1, 0, 0)),
    "-X": ((0, 90, 0), (-1, 0, 0)),
    "+Y": ((90, 0, 0), (0, 1, 0)),
    "-Y": ((-90, 0, 0), (0, -1, 0)),
    "+X+Z": ((0, -135, 0), (1, 0, 1)),
    "+Y45-Z": ((45, 0, 0), (0, 1, -1)),
}

SCENE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "World"
{{
    def DomeLight "dome"
    {{
        asset inputs:texture:file = @{texture}@
        token inputs:texture:format = "latlong"
        float inputs:intensity = 1
        bool inputs:karma:light:renderlightgeo = 1
    }}
{cameras}
}}

def Scope "Render"
{{
{settings}
}}
"""

CAMERA = """    def Camera "{name}"
    {{
        float focalLength = 500
        float horizontalAperture = 2
        float verticalAperture = 2
        float2 clippingRange = (0.1, 1000)
        float3 xformOp:rotateXYZ = ({r[0]}, {r[1]}, {r[2]})
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }}
"""

SETTINGS = """    def RenderSettings "{name}"
    {{
        rel camera = </World/{camera}>
        int2 resolution = (8, 8)
        int nova:samples = 16
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = 16
    }}
"""


def expected_uv(d):
    x, y, z = d
    n = math.sqrt(x * x + y * y + z * z)
    x, y, z = x / n, y / n, z / n
    return (0.5 - math.atan2(x, z) / (2 * math.pi)) % 1.0, math.acos(max(-1.0, min(1.0, y))) / math.pi


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--renderer", default="BRAY_HdKarma")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    bin_dir = os.path.join(args.hfs, "bin")

    # 256 x 128 texture: R = u, G = v at the texel centers.
    texture = os.path.join(args.out, "uvprobe.exr").replace("\\", "/")
    w, h = 256, 128
    u0, u1 = 0.5 / w, 1.0 - 0.5 / w
    v0, v1 = 0.5 / h, 1.0 - 0.5 / h
    subprocess.run([os.path.join(bin_dir, "hoiiotool.exe"), "--pattern",
                    f"fill:topleft={u0},{v0},0:topright={u1},{v0},0:bottomleft={u0},{v1},0:bottomright={u1},{v1},0",
                    f"{w}x{h}", "3", "-d", "float", "-o", texture], check=True)

    cameras = "".join(CAMERA.format(name=f"cam{i}", r=r) for i, (r, _) in enumerate(DIRECTIONS.values()))
    settings = "".join(SETTINGS.format(name=f"s{i}", camera=f"cam{i}") for i in range(len(DIRECTIONS)))
    scene = os.path.join(args.out, "dome_probe.usda")
    with open(scene, "w", encoding="utf-8") as f:
        f.write(SCENE.format(texture=texture, cameras=cameras, settings=settings))

    env = dict(os.environ)
    prefs = tempfile.mkdtemp(prefix="nova_hprefs_")
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for var in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
        env.pop(var, None)
    env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    env["HOUDINI_PATH"] = args.stage.replace("\\", "/") + ";&"
    try:
        print(f"{'dir':8s} {'expected (u, v)':>18s} {'rendered (u, v)':>18s}")
        for i, (name, (_, d)) in enumerate(DIRECTIONS.items()):
            image = os.path.join(args.out, f"probe_{args.renderer}_{i}.exr")
            proc = subprocess.run([os.path.join(bin_dir, "husk.exe"), "--renderer", args.renderer, "--settings",
                                   f"/Render/s{i}", "-o", image, scene], env=env, capture_output=True, text=True)
            if proc.returncode != 0:
                print(name, "render failed:", proc.stderr[-800:])
                continue
            stats = subprocess.run([os.path.join(bin_dir, "hoiiotool.exe"), image, "--printstats"],
                                   capture_output=True, text=True).stdout
            m = re.search(r"Stats Avg:\s*([-0-9.e ]+)", stats)
            got = [float(v) for v in m.group(1).split()][:2] if m else None
            eu, ev = expected_uv(d)
            print(f"{name:8s} ({eu:7.4f}, {ev:7.4f})  " + (f"({got[0]:7.4f}, {got[1]:7.4f})" if got else "-"))
    finally:
        shutil.rmtree(prefs, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
