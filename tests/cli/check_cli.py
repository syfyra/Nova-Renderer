"""nova_cli smoke test: renders tests/scenes with nova_cli (no Houdini
license, no husk) and checks the same pixel values as tests/husk/check_scenes.py,
plus crop-window placement and crop == full frame, and that render settings
are read like husk reads them (values measured with husk): overscan, products
without RenderVars, purposes, the deprecated instantaneousShutter, the
resolution without settings, the EXR part layout and the pixel aspect ratio.
Warnings are printed once, and linear .hdr textures need no OCIO config. The
cryptomatte sidecar of a bare product name goes next to the image.

Uses only the Python standard library and Houdini's iinfo/hoiiotool.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import textwrap

FAILURES = []


def check(cond, message):
    print(("ok   " if cond else "FAIL ") + message)
    if not cond:
        FAILURES.append(message)


def tool(args, name):
    return os.path.join(args.hfs, "bin", name)


def cli(args, scene, settings, output=None, extra=(), expect=0, env=None):
    cmd = [args.cli, scene if os.path.isabs(scene) else os.path.join(args.scenes, scene), "--quiet", *extra]
    if settings:
        cmd += ["--settings", settings]
    if output:
        cmd += ["-o", output]
    try:
        proc = subprocess.run(cmd, cwd=args.out, capture_output=True, text=True, timeout=60, env=env)
    except subprocess.TimeoutExpired:
        check(False, f"nova_cli {scene} {settings} finishes within 60 s")
        return None
    if proc.returncode != expect:
        print(proc.stdout[-2000:])
        print(proc.stderr[-2000:])
    check(proc.returncode == expect, f"nova_cli {scene} {settings} exits with {expect} ({proc.returncode})")
    return proc


def parts(args, image):
    """Per EXR part: name, channels, pixel type, PixelAspectRatio, origin and size."""
    out = subprocess.run([tool(args, "hoiiotool.exe"), "--info", "-v", "-a", image], capture_output=True,
                         text=True).stdout
    result = []
    blocks = re.split(r"\n\s*subimage\s+\d+:", out)[1:]
    if not blocks:  # a single part has no subimage headers
        blocks = [out.split(" : ", 1)[-1]]
    for block in blocks:
        head = re.match(r"\s*(\d+)\s*x\s*(\d+),\s*(\d+) channel,\s*(\w+)", block)
        info = {"size": (int(head.group(1)), int(head.group(2))), "type": head.group(4)} if head else {}
        m = re.search(r"channel list:\s*(.*)", block)
        info["channels"] = [c.strip() for c in m.group(1).split(",")] if m else []
        m = re.search(r'\bname:\s*"([^"]*)"', block)
        info["name"] = m.group(1) if m else None
        m = re.search(r"PixelAspectRatio:\s*([0-9.e+-]+)", block)
        info["par"] = float(m.group(1)) if m else None
        m = re.search(r"pixel data origin: x=(-?\d+), y=(-?\d+)", block)
        info["origin"] = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        m = re.search(r"full/display size:\s*(\d+)\s*x\s*(\d+)", block)
        info["full"] = (int(m.group(1)), int(m.group(2))) if m else info.get("size")
        result.append(info)
    return result


def max_diff(args, a, b, cut=None):
    cmd = [tool(args, "hoiiotool.exe"), a] + (["--cut", cut] if cut else []) + [b] + (["--cut", cut] if cut else [])
    out = subprocess.run(cmd + ["--diff"], capture_output=True, text=True).stdout
    m = re.search(r"Max error\s*=\s*([0-9.e+-]+)", out)
    return float(m.group(1)) if m else (0.0 if "PASS" in out else 1.0)


def data_window(args, image):
    out = subprocess.run([tool(args, "iinfo.exe"), image], capture_output=True, text=True).stdout
    m = re.search(r"Data Window:\s*\((\d+),(\d+)\)-\((\d+),(\d+)\)", out)
    return tuple(int(v) for v in m.groups()) if m else None


def pixel(args, image, subimage, x, y):
    out = subprocess.run([tool(args, "hoiiotool.exe"), image, "--subimage", str(subimage), "--cut", f"1x1+{x}+{y}",
                          "--printstats"], capture_output=True, text=True).stdout
    m = re.search(r"Stats Avg:\s*([-0-9.e ]+)", out)
    return [float(v) for v in m.group(1).split()] if m else []


def near(values, expected, tol):
    return len(values) >= len(expected) and all(abs(v - e) <= tol for v, e in zip(values, expected))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--scenes", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    # 1. Full frame and crop window (dataWindowNDC 0.25 0.3 0.7 0.8 at 640x427).
    full = os.path.join(args.out, "full.exr")
    crop = os.path.join(args.out, "crop.exr")
    cli(args, "basic_crop.usda", "/Render/full", full)
    cli(args, "basic_crop.usda", "/Render/crop", crop)
    ground = pixel(args, full, 0, 20, 410)
    check(near(ground, [0.364, 0.379, 0.394, 1.0], 0.02), f"full-frame ground ~ sun + sky: {ground}")
    window = data_window(args, crop)
    check(window == (160, 85, 447, 297), f"crop data window {window} == husk's (160, 85, 447, 297)")
    if window:
        x0, y0, x1, y1 = window
        cut = f"{x1 - x0 + 1}x{y1 - y0 + 1}+{x0}+{y0}"
        diff = subprocess.run([tool(args, "hoiiotool.exe"), full, "--cut", cut, crop, "--cut", cut, "--diff"],
                              capture_output=True, text=True).stdout
        m = re.search(r"Max error\s*=\s*([0-9.e+-]+)", diff)
        max_err = float(m.group(1)) if m else (0.0 if "PASS" in diff else 1.0)
        check(max_err == 0.0, f"crop matches full frame exactly (max error {max_err:g})")

    # 2. Multi-AOV product: one EXR part per RenderVar, husk's layout.
    cli(args, "basic_aovs.usda", "/Render/aovs")
    aovs = os.path.join(args.out, "aovs.exr")
    # The dome is camera-visible (karma:light:renderlightgeo) and then opaque, as in Karma.
    check(near(pixel(args, aovs, 0, 160, 20), [0.24, 0.27, 0.30, 1.0], 1e-3), "sky color = dome radiance, alpha 1")
    check(near(pixel(args, aovs, 1, 160, 20), [0.0], 1e-6), "sky depth = 0")
    check(near(pixel(args, aovs, 1, 160, 190), [5.3144], 0.02), "ground depth = planar camera depth")
    check(near(pixel(args, aovs, 2, 160, 190), [0.0, 1.0, 0.0], 1e-3), "ground normal = +Y")
    check(near(pixel(args, aovs, 3, 114, 98), [0.8, 0.25, 0.15], 1e-3), "sphere albedo = displayColor")
    check(near(pixel(args, aovs, 4, 160, 20), [1.0], 1e-6), "sky primId = -1 (0xFFFFFFFF)")

    # 3. Overscan: a data window beyond the display window, as husk writes it.
    full = os.path.join(args.out, "parity_full.exr")
    over = os.path.join(args.out, "parity_overscan.exr")
    cli(args, "cli_parity.usda", "/Render/full", full)
    cli(args, "cli_parity.usda", "/Render/overscan", over)
    info = parts(args, over)
    if info:
        check(info[0]["origin"] == (-24, 0) and info[0]["size"] == (156, 84) and info[0]["full"] == (120, 80),
              f"overscan data window origin {info[0]['origin']} size {info[0]['size']} display {info[0]['full']}"
              " == husk's (-24, 0) 156x84 in 120x80")
        check(max_diff(args, full, over, "120x80+0+0") == 0.0, "the overscan render matches the full frame inside it")

    # 4. A product without RenderVars is an error (husk exits 1), not a hang.
    cli(args, "cli_parity.usda", "/Render/novars", expect=2)

    # 5. EXR layout: "C" first with bare R,G,B,A in the aov:format type (half);
    #    without "C" the first color AOV is bare; adjustPixelAspectRatio at the
    #    final resolution.
    layout = os.path.join(args.out, "layout.exr")
    cli(args, "cli_parity.usda", "/Render/layout", layout)
    info = parts(args, layout)
    check([i["name"] for i in info] == ["C", "N"], f"layout parts {[i['name'] for i in info]} == ['C', 'N']")
    if info:
        check(info[0]["channels"] == ["R", "G", "B", "A"] and info[0]["type"] == "half",
              f"C has {info[0]['channels']} in {info[0]['type']} == R, G, B, A in half")
        check(info[-1]["channels"] == ["N.x", "N.y", "N.z"], f"N has {info[-1]['channels']}")
    beauty = os.path.join(args.out, "beauty3.exr")
    cli(args, "cli_parity.usda", "/Render/beauty3", beauty)
    info = parts(args, beauty)
    check([i["channels"] for i in info] == [["N.x", "N.y", "N.z"], ["R", "G", "B"]],
          f"beauty3 channels {[i['channels'] for i in info]}")
    formats = os.path.join(args.out, "formats.exr")
    cli(args, "cli_parity.usda", "/Render/formats", formats)
    info = parts(args, formats)
    expected = [("C", ["R", "G", "B", "A"]), ("depth", ["depth.Z"]), ("N", ["N.x", "N.y", "N.z"]),
                ("Nv", ["Nv.x", "Nv.y", "Nv.z"])]
    check([(i["name"], i["channels"]) for i in info] == expected,
          f"husk:format layout {[(i['name'], i['channels']) for i in info]} == {expected}")
    check(all(i["type"] == "half" for i in info), f"husk:format types {[i['type'] for i in info]} are all half")
    par = os.path.join(args.out, "par.exr")
    cli(args, "cli_parity.usda", "/Render/par", par, ("--res", "320x160"))
    info = parts(args, par)
    check(bool(info) and info[0]["par"] is not None and abs(info[0]["par"] - 0.75) < 1e-4,
          f"adjustPixelAspectRatio at 320x160: PixelAspectRatio {info[0]['par'] if info else None} == 0.75")

    # 6. Purposes: husk renders "render" whenever "default" is included.
    def visible(image):
        return "".join("#" if pixel(args, image, 0, x, 20)[3:4] == [1.0] else "." for x in (20, 60, 100, 140))
    for settings, expected in (("dflt", "##.."), ("proxyguide", "####"), ("renderonly", ".#.."),
                               ("proxyonly", "..#.")):
        image = os.path.join(args.out, f"purposes_{settings}.exr")
        cli(args, "cli_purposes.usda", f"/Render/{settings}", image)
        cards = visible(image)
        check(cards == expected, f"purposes {settings}: cards {cards} == {expected}")
    # --purposes replaces includedPurposes and adds only default geometry (as
    # husk --purpose geometry,<list>), never "render".
    for flag, expected in (("proxy", "#.#."), ("guide", "#..#"), ("render,proxy", "###.")):
        image = os.path.join(args.out, f"purposes_flag_{flag.replace(',', '_')}.exr")
        cli(args, "cli_purposes.usda", "/Render/dflt", image, ("--purposes", flag))
        cards = visible(image)
        check(cards == expected, f"--purposes {flag}: cards {cards} == {expected}")

    # 7. instantaneousShutter is not passed by husk: the image stays blurred.
    blurred = os.path.join(args.out, "instant_blurred.exr")
    instant = os.path.join(args.out, "instant.exr")
    cli(args, "motion.usda", "/Render/settings", blurred, ("--frame", "1", "--spp", "4"))
    cli(args, "cli_instant.usda", "/Render/settings", instant, ("--frame", "1", "--spp", "4"))
    check(max_diff(args, blurred, instant) == 0.0, "instantaneousShutter renders like husk (blurred)")

    # 8. Without a RenderSettings prim husk renders 1280x720.
    default = os.path.join(args.out, "default.exr")
    cli(args, "basic.usda", None, default, ("--spp", "1"))
    info = parts(args, default)
    check(bool(info) and info[0]["size"] == (1280, 720), f"default resolution {info[0]['size'] if info else None}")

    # 9. Diagnostics: a linear .hdr and a PNG tagged Gamma2.2 (OIIO's name
    #    for a gAMA chunk; decoded as sRGB, as Karma) need no OCIO config, and
    #    every warning appears once.
    hdr = os.path.join(args.out, "sky.hdr")
    subprocess.run([tool(args, "hoiiotool.exe"), "--pattern", "constant:color=0.8,0.4,0.1", "16x8", "3", "-d", "float",
                    "-o", hdr], capture_output=True)
    subprocess.run([tool(args, "hoiiotool.exe"), "--pattern", "constant:color=0.8,0.4,0.1", "16x8", "3", "-d", "uint8",
                    "--attrib", "oiio:ColorSpace", "Gamma2.2", "-o", os.path.join(args.out, "gamma.png")],
                   capture_output=True)
    dome = textwrap.dedent("""\
        #usda 1.0
        def Xform "World"
        {
            def DomeLight "sky"
            {
                asset inputs:texture:file = @./TEXTURE@
            }
            def Camera "cam"
            {
                double3 xformOp:translate = (0, 0, 5)
                uniform token[] xformOpOrder = ["xformOp:translate"]
            }
        }
        def RenderSettings "rs"
        {
            rel camera = </World/cam>
            int2 resolution = (8, 8)
            int nova:samples = 1
        }
        """)
    no_ocio = {k: v for k, v in os.environ.items() if k != "OCIO"}
    for texture, needle in (("sky.hdr", None), ("gamma.png", None), ("missing.exr", "cannot open texture")):
        scene = os.path.join(args.out, f"warn_{texture.replace('.', '_')}.usda")
        with open(scene, "w") as f:
            f.write(dome.replace("TEXTURE", texture))
        proc = cli(args, scene, "/rs", os.path.join(args.out, "warn.exr"), env=no_ocio)
        if proc is None:
            continue
        if needle is None:
            check("Warning" not in proc.stderr,
                  f"{texture} without OCIO gives no warning: {proc.stderr.strip()[:200]}")
        else:
            check(proc.stderr.count(needle) == 1, f"the missing texture is reported once: {proc.stderr.strip()[:300]}")

    # 10. A bare product name: the cryptomatte sidecar goes next to the image
    #     (the working directory), not to the drive's root.
    sidecar = os.path.join(args.out, "crypto_materials.json")
    if os.path.exists(sidecar):
        os.remove(sidecar)
    proc = cli(args, "crypto.usda", "/Render/s", "crypto_bare.exr", ("--spp", "1"))
    if proc is not None:
        check(os.path.exists(os.path.join(args.out, "crypto_bare.exr")), "-o crypto_bare.exr renders into the cwd")
        manifest = {}
        if os.path.exists(sidecar):
            with open(sidecar, encoding="utf-8") as f:
                manifest = json.load(f)
        check("/World/mtl/red" in manifest, f"the sidecar of a bare product is in the cwd: {sorted(manifest)}")
        check("cannot write" not in proc.stderr, f"the sidecar is written: {proc.stderr.strip()[:300]}")

    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
