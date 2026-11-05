"""husk smoke test for the Nova Hydra delegate.

Renders the reference scenes in tests/scenes with husk and the staged Nova
package, then checks
  * that husk loads hdNova from the staged package (not an installed one),
  * the default beauty product against known pixel values,
  * that a crop render (dataWindowNDC) reproduces the full-frame pixels,
  * the AOVs of a multi-AOV render product against known values.

Uses only the Python standard library plus Houdini's husk, iinfo and
hoiiotool. Houdini runs with a temporary, empty preference directory and
without HOUDINI_PACKAGE_DIR/HSITE, so no user packages (and no installed
Nova) are loaded.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

FAILURES = []


def check(cond, message):
    print(("ok   " if cond else "FAIL ") + message)
    if not cond:
        FAILURES.append(message)


def tool(args, name):
    return os.path.join(args.hfs, "bin", name)


def run(cmd, env, cwd):
    proc = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout[-2000:])
        print(proc.stderr[-2000:])
    return proc


def husk(args, settings, scene, env, output=None):
    cmd = [tool(args, "husk.exe"), "--renderer", "Nova", "--settings", settings]
    if output:
        cmd += ["-o", output]
    cmd.append(os.path.join(args.scenes, scene))
    proc = run(cmd, env, args.out)
    check(proc.returncode == 0, f"husk {scene} {settings} exits with 0")


def data_window(args, image):
    out = subprocess.run([tool(args, "iinfo.exe"), image], capture_output=True, text=True).stdout
    m = re.search(r"Data Window:\s*\((\d+),(\d+)\)-\((\d+),(\d+)\)", out)
    return tuple(int(v) for v in m.groups()) if m else None


def is_constant(args, image):
    out = subprocess.run([tool(args, "hoiiotool.exe"), image, "--printstats"], capture_output=True, text=True).stdout
    return "Constant: Yes" in out


def pixel(args, image, subimage, x, y):
    out = subprocess.run([tool(args, "hoiiotool.exe"), image, "--subimage", str(subimage), "--cut", f"1x1+{x}+{y}",
                          "--printstats"], capture_output=True, text=True).stdout
    m = re.search(r"Stats Avg:\s*([-0-9.e ]+)", out)
    return [float(v) for v in m.group(1).split()] if m else []


def near(values, expected, tol):
    return len(values) >= len(expected) and all(abs(v - e) <= tol for v, e in zip(values, expected))


def isolated_env(args):
    env = dict(os.environ)
    prefs = tempfile.mkdtemp(prefix="nova_hprefs_")
    # Houdini ignores a preference directory without the __HVER__ placeholder.
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for var in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
        env.pop(var, None)
    env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    env["HOUDINI_PATH"] = args.stage.replace("\\", "/") + ";&"
    return env, prefs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True, help="staged package root (contains dso/usd_plugins)")
    parser.add_argument("--scenes", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    env, prefs = isolated_env(args)

    try:
        # 0. husk must load hdNova from the staged package.
        probe = dict(env, TF_DEBUG="PLUG_REGISTRATION")
        listing = run([tool(args, "husk.exe"), "--list-renderers"], probe, args.out)
        log = (listing.stdout + listing.stderr).replace("\\", "/").lower()
        registered = re.findall(r"registering shared library plugin 'hdnova' at '([^']+)'", log)
        stage = args.stage.replace("\\", "/").lower()
        check(len(registered) == 1 and registered[0].startswith(stage),
              f"hdNova is registered once, from the staged package: {registered}")
        check("hdnovarendererplugin (nova)" in log, "husk lists HdNovaRendererPlugin (Nova)")

        # 1. Default beauty product (LPE "C.*[LO]") and crop vs full frame.
        full = os.path.join(args.out, "full.exr")
        crop = os.path.join(args.out, "crop.exr")
        husk(args, "/Render/full", "basic_crop.usda", env, full)
        husk(args, "/Render/crop", "basic_crop.usda", env, crop)
        check(not is_constant(args, full), "full-frame beauty is not a constant image")
        full_ground = pixel(args, full, 0, 20, 410)
        check(near(full_ground, [0.364, 0.379, 0.394, 1.0], 0.02), f"full-frame ground ~ sun + sky: {full_ground}")
        window = data_window(args, crop)
        check(window == (160, 85, 447, 297), f"crop data window {window} == (160, 85, 447, 297)")
        if window:
            x0, y0, x1, y1 = window
            cut = f"{x1 - x0 + 1}x{y1 - y0 + 1}+{x0}+{y0}"
            diff = subprocess.run([tool(args, "hoiiotool.exe"), full, "--cut", cut, crop, "--cut", cut, "--diff"],
                                  capture_output=True, text=True).stdout
            m = re.search(r"Max error\s*=\s*([0-9.e+-]+)", diff)
            max_err = float(m.group(1)) if m else (0.0 if "PASS" in diff else 1.0)
            check(max_err == 0.0, f"crop matches full frame exactly (max error {max_err:g})")

        # 2. AOVs of a render product: C, depth, N, albedo, primId.
        husk(args, "/Render/aovs", "basic_aovs.usda", env)
        aovs = os.path.join(args.out, "aovs.exr")
        sky_c = pixel(args, aovs, 0, 160, 20)
        # The dome is camera-visible through karma:light:renderlightgeo; a
        # visible light is opaque, as in Karma.
        check(near(sky_c, [0.24, 0.27, 0.30, 1.0], 1e-3), f"sky color = dome radiance, alpha 1: {sky_c}")
        ground_c = pixel(args, aovs, 0, 10, 205)
        check(near(ground_c, [0.364, 0.379, 0.394, 1.0], 0.02), f"ground color ~ analytic sun + sky: {ground_c}")
        check(near(pixel(args, aovs, 1, 160, 20), [0.0], 1e-6), "sky depth = 0 (Karma convention)")
        ground_depth = pixel(args, aovs, 1, 160, 190)
        # Karma CPU renders this pixel's depth (Pz) as 5.314436 (pixel-filtered,
        # Nova writes the pixel centre, hence the tolerance).
        check(near(ground_depth, [5.3144], 0.02), f"ground depth = planar camera depth as in Karma: {ground_depth}")
        check(near(pixel(args, aovs, 2, 160, 190), [0.0, 1.0, 0.0], 1e-3), "ground normal = +Y")
        check(near(pixel(args, aovs, 3, 114, 98), [0.8, 0.25, 0.15], 1e-3), "sphere albedo = displayColor")
        check(near(pixel(args, aovs, 4, 160, 20), [1.0], 1e-6), "sky primId = -1 (0xFFFFFFFF)")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
