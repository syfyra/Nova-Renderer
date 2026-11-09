"""husk checks of Karma's path guiding settings in Nova (docs/design/M11-P9c.md).

The scenes in karma_guiding/ hold what the Karma Render Settings LOP authors
(karma_guiding/build_karma_guiding.py writes them with an isolated hython)
over two indirect-heavy scenes: a room lit by the sun through a window, and
the same room filled with fog. Every case renders 32 Path Traced samples per
pixel with a uniform oracle and no colour limits, several times with
different seeds (an override layer sets karma:global:randomseed).

Guiding must change only the noise. Per case the script measures the image
mean over all seeds and the noise: the relative variance of 8 x 8 block
means across the seeds (mean of var / mean^2 over the blocks), and compares:

- the means with guiding off, on, and with each control, with Karma's means
  (Karma CPU, measured once and stored below) and with each other;
- the noise with guiding on against guiding off: Nova's must drop at least
  as stated (Karma's ratios stored below for reference);
- the controls: training samples (a frozen field after 8 samples), the
  training samples discarded (clearbuffers: the image holds 24 of the 32
  samples, read from a ray:pixelsamples var), components without the scene's
  (the image of guiding off, bit for bit), deterministic (two renders with
  one seed are identical; Nova's guiding is always deterministic);
- Nova XPU on the room and the fog: Nova CPU's means and noise drop.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_karma_guiding.py --hfs ... --stage ... --out ...
With --renderer BRAY_HdKarma (or BRAY_HdKarmaXPU) it measures Karma and
prints the values instead of checking.
"""

import argparse
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "karma_guiding")
BLOCK = 8

# Karma CPU 22.0.466 (isolated husk, 8 seeds, 2026-11-08): per case the image
# mean over the seeds, the block noise (median over the 8 x 8 blocks of the
# relative variance of their means across the seeds) and the pixel noise (the
# same per pixel). Karma XPU renders guiding on and off identically (it
# ignores guiding); Karma CPU without guiding_deterministic differs run to
# run, with it two renders are identical.
KARMA = {
    "room_off": (0.02659, 1.505e-2, 1.905),
    "room_on": (0.02660, 1.928e-2, 1.328),
    "room_train": (0.02658, 1.902e-2, 1.350),
    "room_clear": (0.02660, 2.777e-2, 1.906),
    "room_reflect": (0.02659, 1.505e-2, 1.905),  # bit-identical to room_off
    "room_det": (0.02660, 1.746e-2, 1.360),
    "fog_off": (0.01154, 8.142e-3, 0.5422),
    "fog_on": (0.01152, 7.075e-3, 0.4491),
    "fog_diffuse": (0.01154, 8.849e-3, 0.5594),
}

CASES = ["room_off", "room_on", "room_train", "room_clear", "room_reflect", "room_det", "fog_off", "fog_on",
         "fog_diffuse"]
XPU_CASES = ["room_off", "room_on", "fog_off", "fog_on"]


def seed_layer(case, seed, out, samples=0):
    path = os.path.join(out, f"{case}_seed{seed}.usda")
    extra = f"\n        int karma:global:pathtracedsamples = {samples}" if samples else ""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write('''#usda 1.0
(
    subLayers = [@%s@]
)
over "Render"
{
    over "rendersettings"
    {
        int karma:global:randomseed = %d%s
    }
}
''' % (os.path.join(HERE, case + ".usda").replace("\\", "/"), seed, extra))
    return path


def samples_layer(case, out):
    """The case with a ray:pixelsamples var in its product (as check_karma_props.py)."""
    path = os.path.join(out, case + "_samples.usda")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write('''#usda 1.0
(
    subLayers = [@%s@]
)
over "Render"
{
    over "Products"
    {
        over "renderproduct"
        {
            rel orderedVars = [</Render/Vars/beauty>, </Render/Vars/samples>]
        }
    }
    over "Vars"
    {
        def RenderVar "samples"
        {
            uniform token dataType = "float"
            uniform string sourceName = "ray:pixelsamples"
            uniform token sourceType = "raw"
            token driver:parameters:aov:format = "float"
            string driver:parameters:aov:name = "samples"
        }
    }
}
''' % os.path.join(HERE, case + ".usda").replace("\\", "/"))
    return path


def read(path, np, oiio, part=0):
    image = oiio.ImageInput.open(path)
    if not image or not image.seek_subimage(part, 0):
        return None
    spec = image.spec()
    px = np.asarray(image.read_image(part, 0, 0, spec.nchannels, oiio.FLOAT)).reshape(spec.height, spec.width, -1)
    image.close()
    return px[..., :3].mean(axis=2)


def stats(images, np):
    """Mean, block noise and pixel noise of a stack of luminance images."""
    stack = np.stack(images)  # seeds x h x w
    h, w = stack.shape[1] // BLOCK * BLOCK, stack.shape[2] // BLOCK * BLOCK
    blocks = stack[:, :h, :w].reshape(len(images), h // BLOCK, BLOCK, w // BLOCK, BLOCK).mean(axis=(2, 4))
    bmean = blocks.mean(axis=0)
    keep = bmean > 1e-3
    bvar = blocks.var(axis=0, ddof=1)
    pmean = stack.mean(axis=0)
    pkeep = pmean > 1e-3
    pvar = stack.var(axis=0, ddof=1)
    brel = bvar[keep] / bmean[keep] ** 2
    prel = pvar[pkeep] / pmean[pkeep] ** 2
    return {
        "mean": float(stack.mean()),
        # The median over the blocks (a firefly makes one block's variance
        # dominate a mean), and the mean for reference.
        "noise": float(np.median(brel)),
        "noisemean": float(brel.mean()),
        "pixnoise": float(np.median(prel)),
        # The standard error of the image mean.
        "error": float(stack.mean(axis=(1, 2)).std(ddof=1) / np.sqrt(len(images))),
    }


def checks(results, images):
    """Nova against Karma's behaviour (the values in KARMA)."""
    def r(case, key):
        return results[case][key]

    # Guiding changes only the noise: the means with guiding off and on agree
    # within their noise, and each case agrees with Karma's mean.
    for case, (mean, _, _) in KARMA.items():
        if case not in results:
            continue
        check(abs(r(case, "mean") - mean) <= 0.03 * mean,
              f"{case}: mean {r(case, 'mean'):.5f}, Karma {mean:.5f} (3 %)")
    for off, on in (("room_off", "room_on"), ("room_off", "room_train"), ("room_off", "room_clear"),
                    ("fog_off", "fog_on"), ("fog_off", "fog_diffuse")):
        if off in results and on in results:
            bound = 4.0 * (r(off, "error") ** 2 + r(on, "error") ** 2) ** 0.5 + 1e-5
            check(abs(r(off, "mean") - r(on, "mean")) <= bound,
                  f"{on}: mean {r(on, 'mean'):.5f} = {off}'s {r(off, 'mean'):.5f} within 4 sigma ({bound:.5f})")
    # The noise drops as in Karma (pixel noise; Karma x0.70 in the room, x0.83
    # in the fog).
    for off, on, bound in (("room_off", "room_on", 0.8), ("room_off", "room_train", 0.8), ("fog_off", "fog_on", 0.9)):
        if off in results and on in results:
            ratio = r(on, "pixnoise") / r(off, "pixnoise")
            karma = KARMA[on][2] / KARMA[off][2]
            check(ratio < bound, f"{on}: pixel noise x{ratio:.2f} of {off} (Karma x{karma:.2f}, bound {bound})")
    # Components: none of the room's listed: the image of guiding off, bit for
    # bit (Karma too); volumes left out: the fog's gain mostly goes.
    if "room_off" in images and "room_reflect" in images:
        same = all((a == b).all() for a, b in zip(images["room_off"], images["room_reflect"]))
        check(same, "room_reflect: identical to room_off (no listed component in the scene)")
    if "fog_on" in results and "fog_diffuse" in results:
        check(r("fog_diffuse", "pixnoise") > 1.05 * r("fog_on", "pixnoise"),
              f"fog_diffuse: pixel noise {r('fog_diffuse', 'pixnoise'):.3f} over fog_on's "
              f"{r('fog_on', 'pixnoise'):.3f} (volumes not guided)")
    # clearbuffers: the image keeps the samples after training, 24 of 32
    # (Karma: noise x1.41 of room_train's; Nova's training samples are the
    # noisy ones, so dropping them costs little noise: the samples plane
    # shows the count).
    if "room_clear" in results and "samples" in results["room_clear"]:
        got = results["room_clear"]["samples"]
        check(abs(got - 24.0) < 1e-3, f"room_clear: {got:.2f} samples per pixel in the image (32 less 8 training)")
    if "room_train" in results and "samples" in results["room_train"]:
        got = results["room_train"]["samples"]
        check(abs(got - 32.0) < 1e-3, f"room_train: {got:.2f} samples per pixel in the image (training kept)")
    # Deterministic: Nova always (Karma only with guiding_deterministic).
    for case in ("room_on", "room_det"):
        if case in results:
            check(r(case, "repeat") == 0.0, f"{case}: a second render is identical (|diff| {r(case, 'repeat'):.3e})")


def xpu_checks(args, env, cpu, np, oiio):
    """Nova XPU on the room and the fog, guiding off and on: the means of Nova
    CPU, the noise drop of Nova CPU (Karma XPU ignores guiding)."""
    xpu = {}
    for case in XPU_CASES:
        if case not in cpu:
            continue
        images = []
        for seed in range(args.seeds):
            exr = os.path.join(args.out, f"{case}_xpu_{seed}.exr")
            proc = run([tool(args, "husk.exe"), "--renderer", "HdNovaXpuRendererPlugin", "--headlight", "none", "-s",
                        "/Render/rendersettings", "-o", exr, seed_layer(case, seed, args.out, args.samples)], env,
                       args.out)
            lum = read(exr, np, oiio) if proc.returncode == 0 else None
            if lum is None:
                check(False, f"XPU {case} seed {seed}: no image")
                return
            images.append(lum)
        xpu[case] = stats(images, np)
        r = xpu[case]
        print(f"     XPU {case}: mean {r['mean']:.5f} noise {r['noise']:.3e} pixel noise {r['pixnoise']:.3e}")
        bound = 4.0 * (r["error"] ** 2 + cpu[case]["error"] ** 2) ** 0.5 + 1e-5
        check(abs(r["mean"] - cpu[case]["mean"]) <= bound,
              f"XPU {case}: mean {r['mean']:.5f} = CPU's {cpu[case]['mean']:.5f} within 4 sigma ({bound:.5f})")
    for off, on, bound in (("room_off", "room_on", 0.8), ("fog_off", "fog_on", 0.9)):
        if off in xpu and on in xpu:
            ratio = xpu[on]["pixnoise"] / xpu[off]["pixnoise"]
            cpu_ratio = cpu[on]["pixnoise"] / cpu[off]["pixnoise"]
            check(ratio < bound, f"XPU {on}: pixel noise x{ratio:.2f} of {off} (CPU x{cpu_ratio:.2f}, bound {bound})")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--renderer", default="Nova", help="BRAY_HdKarma or BRAY_HdKarmaXPU measures Karma")
    parser.add_argument("--seeds", type=int, default=8)
    parser.add_argument("--cases", default=",".join(CASES))
    parser.add_argument("--samples", type=int, default=0, help="override the scenes' 32 samples")
    parser.add_argument("--no-xpu", action="store_true", help="skip the Nova XPU renders")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)
    measure = args.renderer.startswith("BRAY")
    results, everything = {}, {}
    for case in args.cases.split(","):
        images, seconds = [], 0.0
        for seed in range(args.seeds):
            exr = os.path.join(args.out, f"{case}_{seed}.exr")
            start = time.time()
            proc = run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s",
                        "/Render/rendersettings", "-o", exr, seed_layer(case, seed, args.out, args.samples)], env,
                       args.out)
            seconds += time.time() - start
            check(proc.returncode == 0, f"husk {case} seed {seed} exits with 0")
            lum = read(exr, np, oiio)
            if lum is None:
                check(False, f"{case}: no image")
                break
            images.append(lum)
        if len(images) < 2:
            continue
        everything[case] = images
        results[case] = stats(images, np)
        results[case]["seconds"] = seconds / len(images)
        if case in ("room_train", "room_clear") and not measure:  # the samples the image holds
            exr = os.path.join(args.out, f"{case}_samples.exr")
            run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s",
                 "/Render/rendersettings", "-o", exr, samples_layer(case, args.out)], env, args.out)
            plane = read(exr, np, oiio, 1)
            if plane is not None:
                results[case]["samples"] = float(plane.mean())
        if case in ("room_on", "room_det"):  # a second render with seed 0: the same image?
            exr = os.path.join(args.out, f"{case}_again.exr")
            run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s",
                 "/Render/rendersettings", "-o", exr, seed_layer(case, 0, args.out, args.samples)], env, args.out)
            again = read(exr, np, oiio)
            results[case]["repeat"] = float(np.abs(again - images[0]).max()) if again is not None else -1.0
        r = results[case]
        print(f"     {case}: mean {r['mean']:.5f} noise {r['noise']:.3e} (mean {r['noisemean']:.3e}) "
              f"pixel noise {r['pixnoise']:.3e} "
              f"{r['seconds']:.2f} s" + (f" repeat {r['repeat']:.3e}" if "repeat" in r else ""))
    if measure:
        print(json.dumps(results, indent=1))
    else:
        checks(results, everything)
        if not args.no_xpu:
            xpu_checks(args, env, results, np, oiio)
    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
