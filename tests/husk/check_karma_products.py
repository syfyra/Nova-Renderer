"""husk checks of Karma's Render Products, Standard Render Vars and
Cryptomatte details against Karma CPU (docs/design/M11-P6.md).

The scenes in tests/husk/karma_products were built once in an isolated hython
with the actual LOPs (karma_products_scenes.py): Karma Standard Render Vars
2.0, Additional Render Vars, Karma Render Products 2.0, Karma Cryptomatte and
the Karma Render Settings LOP's deep camera map and checkpoint products. Nova
renders them through husk; the values are compared with Karma CPU's, measured
once with isolated husk (Houdini 22.0.466, 64 samples, box pixel filter):
- per-var sample filters (karma:filter) at full pixels exactly, at edges
  within the sampling noise of 64 samples: ubox, accumulate, min/max (by
  magnitude), zmin/zmax, var/mse, even/odd on depth, the beauty and
  primvars; Karma's idcover without a PrimId var averages;
- the raw sources ray:mask, time, near, far, P, D, contrib, dPdz, hituv,
  hitN_facing, level, hitstack, pixelsamples and the holdout prefixes;
- per-instance primvar cryptomattes: the instancer's, the prototype's own
  value, nested instancers and native instances name each instance as Karma;
- deep camera maps: a deep EXR with Karma's channels, flattening to the flat
  beauty, the depth range of Karma's;
- checkpoints: a render stopped by husk's time limit leaves a checkpoint, the
  next render resumes from it and equals an uninterrupted render, and the
  finished render removes it.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_karma_products.py --hfs ... --stage ... --out ...
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

SCENES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "karma_products")
COLS = (9, 15, 20, 30, 35)  # row 10 of vars.usda: back face 0/1 edge, front, front/back edge, back/empty edge, ramp

# Karma CPU at row 10, columns COLS (first channel), and the tolerance at the
# columns where the samples see two surfaces (9: two faces, 20, 30, 35) and
# so depend on the sample positions; column 15 (one face) must agree within
# 1e-3 (or the third value).
KARMA = {
    "depth": ([10, 9, 9, 10, 9.8012], 0.02),             # zmin (Karma's Standard Render Vars)
    "z_ubox": ([10, 9, 9.3125, 2.9688, 9.9], 0.4),
    "z_acc": ([640, 576, 596, 190, 633.6], 25),
    "z_min": ([10, 9, 9, 0, 9.8012], 0.02),
    "z_max": ([10, 9, 10, 10, 9.9991], 0.02),
    "z_zmin": ([10, 9, 9, 10, 9.8012], 0.02),
    "z_zmax": ([10, 9, 10, 10, 9.9991], 0.02),
    "z_var": ([0, 0, 0.21825, 21.205, 0.0033722], 2.0),
    "z_mse": ([0, 0, 0.0034102, 0.33133, 5.2691e-05], 0.04),
    # Half the samples each: at the 30 % edge which half gets more hits is the
    # sampler's (Karma 6 and 13 of 32, Nova others); their mean is ubox's (below).
    "z_even": ([10, 9, 9.2813, 1.875, 9.8927], 4.0),
    "z_odd": ([10, 9, 9.3437, 4.0625, 9.9073], 4.0),
    "C_min": ([0.1592, 0.1592, 0.1592, 0, 0], 0.01),
    "pcol_ubox": ([0.3047, 0, 0, 0, 0], 0.05),           # red channel
    "pcol_max": ([0, 0, 0, 0, 0], 0.01),
    "pcol_min": ([0.5, 0, 0, 0, 0], 0.01),
    "pcol_zmin": ([0.5, 0, 0, 0, 0], 0.01),
    "ps_ubox": ([-1.4375, 2, 1.6875, 0.29687, 0], 0.1),
    "ps_max": ([-3, 2, 2, 1, 0], 0.01),
    "ps_min": ([1, 2, 1, 0, 0], 0.01),
    "ps_zmin": ([-3, 2, 2, 1, 0], 0.01),
    "mask": ([1, 1, 1, 0.29687, 1], 0.04),
    "time": ([0.5009, 0.49931, 0.4993, 0.17656, 0.49873], 0.07, 0.01),  # sample times: the mean of 64
    "near": ([0.5, 0.5, 0.5, 0.14844, 0.5], 0.02),
    "far": ([50, 50, 50, 14.844, 50], 2.0),
    "rayorigin": ([-1.05, -0.45, 0.04999, 0.3013, 1.55], 0.02),
    "contrib": ([1, 1, 1, 0.2969, 1], 0.04),
    "dPdz": ([0, 0, 0, 0, 0], 0.0),
    "hituv": ([0.5945, 0.08773, 0.8143, 0.2947, 0.1111], 0.04),
    "level": ([0, 0, 0, 0, 0], 0.0),
    "hitstack": ([1, 2, 1.6875, 0.29687, 3], 0.1),
    "primarysamples": ([64, 64, 64, 63, 64], 2),
    "P_holdouts": ([0, 0, 0, 0, 1.55], 0.02),
    "P_noholdouts": ([-1.05, -0.45, 0.04999, 0.3013, 0], 0.02),
    "element": ([0.39063, 0, 0.3125, 0.29687, 0], 0.04),  # idcover without PrimId: ubox (Karma's error)
    "primid": ([0.39063, 0, 0.3125, 0.29687, 0], 0.04),
}
# Vector planes: the channel the table holds (others checked separately).
CHANNEL = {"rayorigin": 0, "hituv": 0, "P_holdouts": 0, "P_noholdouts": 0, "pcol_ubox": 0, "pcol_max": 0,
           "pcol_min": 0, "pcol_zmin": 0, "contrib": 0}

# Karma's cryptomatte names at instance pixels of crypto_instances.usda.
KARMA_CRYPTO = {
    "CryptoPrimvar_instname": {(20, 20): "proto_pebble", (48, 20): "i1", (76, 20): "proto_pebble", (104, 20): "i3",
                               (127, 20): "innerX", (137, 20): "innerY", (40, 60): "native_A", (68, 60): "native_B",
                               (96, 60): "plain_mesh"},
    "CryptoPrimvar_piece": {(20, 20): "10", (48, 20): "11", (76, 20): "12", (104, 20): "13", (40, 60): "21",
                            (68, 60): None, (96, 60): None},
}


def parts(oiio, np, path):
    inp = oiio.ImageInput.open(path)
    out = {}
    i = 0
    while inp.seek_subimage(i, 0):
        s = inp.spec()
        out[s.getattribute("name") or str(i)] = np.asarray(inp.read_image(i, 0, 0, s.nchannels, oiio.FLOAT)).reshape(
            s.height, s.width, s.nchannels)
        i += 1
    attrs = {a.name: a.value for a in oiio.ImageInput.open(path).spec().extra_attribs}
    return out, attrs


def check_vars(args, env, oiio, np):
    out = os.path.join(args.out, "vars.exr")
    proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "-s", "/Render/s", "-o", out,
                os.path.join(SCENES, "vars.usda")], env, args.out)
    check(proc.returncode == 0, "husk vars.usda exits with 0")
    check("idcover needs a render var named PrimId" in proc.stderr + proc.stdout,
          "idcover without a PrimId var warns, as Karma reports it")
    if not os.path.exists(out):
        return
    planes, _ = parts(oiio, np, out)
    for name, entry in KARMA.items():
        karma, tol = entry[0], entry[1]
        if name not in planes:
            check(False, f"{name}: plane present")
            continue
        ch = CHANNEL.get(name, 0)
        nova = [float(planes[name][10, c, ch]) for c in COLS]
        exact = entry[2] if len(entry) > 2 else 1e-3 + 1e-4 * max(abs(v) for v in karma)
        ok = all(abs(n - k) <= (exact if c == 15 else max(tol, exact)) for n, k, c in zip(nova, karma, COLS))
        check(ok, f"{name}: Nova {['%.5g' % v for v in nova]} vs Karma {karma}")
    # Even and odd samples average to the mean.
    for c in COLS:
        even, odd, box = (float(planes[k][10, c, 0]) for k in ("z_even", "z_odd", "z_ubox"))
        check(abs((even + odd) / 2 - box) < 1e-3, f"column {c}: stats even and odd average to ubox")
    # Vector details: min/max keep the larger/smaller vector, raydir down -Z.
    check(abs(planes["pcol_max"][10, 9, 2] - 1.0) < 1e-4, "pcol max at the face edge is (0, 0, 1), the longer vector")
    check(abs(planes["raydir"][10, 9, 2] + 1.0) < 1e-5, "ray:D of the orthographic camera is (0, 0, -1)")
    check(abs(planes["N_facing"][10, 35, 0] + 0.8944) < 2e-3, "ray:hitN_facing of the ramp faces the camera")
    # The defaults of the other filters: full pixels equal the first-hit planes.
    check(abs(planes["P"][10, 15, 2] - 1.0) < 1e-5 and abs(planes["P"][10, 20, 2] - 0.6875) < 0.04,
          "P (ubox) at the front and at its edge")


def check_persp(args, env, oiio, np):
    out = os.path.join(args.out, "vars_persp.exr")
    proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "-s", "/Render/s", "-o", out,
                os.path.join(SCENES, "vars_persp.usda")], env, args.out)
    check(proc.returncode == 0, "husk vars_persp.usda exits with 0")
    if not os.path.exists(out):
        return
    planes, _ = parts(oiio, np, out)
    # Karma: near and far along the ray (planes / cos), the ray from the eye.
    c = 20
    d = planes["raydir"][10, c]
    cosine = -float(d[2]) / max(1e-9, float(np.linalg.norm(d)))
    check(abs(planes["near"][10, c, 0] - 0.5 / cosine) < 1e-3, "perspective ray:near = near / cos")
    check(abs(planes["far"][10, c, 0] - 50.0 / cosine) < 0.05, "perspective ray:far = far / cos")
    check(np.allclose(planes["rayorigin"][10, c, :3], [0, 0, 10], atol=1e-4), "perspective ray:P is the eye")


def murmur3_32(data):
    c1, c2, h = 0xCC9E2D51, 0x1B873593, 0
    n = len(data) // 4
    for i in range(n):
        k = int.from_bytes(data[4 * i:4 * i + 4], "little")
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        k = (k * c2) & 0xFFFFFFFF
        h ^= k
        h = ((h << 13) | (h >> 19)) & 0xFFFFFFFF
        h = (h * 5 + 0xE6546B64) & 0xFFFFFFFF
    tail, k = data[4 * n:], 0
    if len(tail) >= 3:
        k ^= tail[2] << 16
    if len(tail) >= 2:
        k ^= tail[1] << 8
    if tail:
        k ^= tail[0]
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        h ^= (k * c2) & 0xFFFFFFFF
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    return h ^ (h >> 16)


def check_crypto(args, env, oiio, np):
    out = os.path.join(args.out, "crypto_instances.exr")
    proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "-s", "/Render/s", "-o", out,
                os.path.join(SCENES, "crypto_instances.usda")], env, args.out)
    check(proc.returncode == 0, "husk crypto_instances.usda exits with 0")
    if not os.path.exists(out):
        return
    planes, attrs = parts(oiio, np, out)
    for layer, expected in KARMA_CRYPTO.items():
        key = "cryptomatte/%07x" % (murmur3_32(layer.encode()) >> 4)
        manifest = json.loads(attrs.get(key + "/manifest", "{}") or "{}")
        names = {int(v, 16): k for k, v in manifest.items()}
        for (x, y), want in expected.items():
            best, cov = None, 0.0
            k = 0
            while "%s%02d" % (layer, k) in planes:
                px = planes["%s%02d" % (layer, k)][y, x]
                for j in (0, 2):
                    if px[j + 1] > cov:
                        cov = float(px[j + 1])
                        best = names.get(struct.unpack("<I", struct.pack("<f", float(px[j])))[0], "?")
                k += 1
            check(best == want, f"{layer} at ({x},{y}): Nova {best} vs Karma {want}")


def check_deep(args, env, oiio, np):
    folder = os.path.join(args.out, "deep")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    shutil.copy(os.path.join(SCENES, "deep.usda"), folder)
    proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "deep.usda"], env, folder)
    check(proc.returncode == 0, "husk deep.usda exits with 0")
    deep_path = os.path.join(folder, "deep_deep.exr")
    check(os.path.exists(deep_path), "Nova writes the karma:deep product")
    check(not os.path.exists(os.path.join(folder, "deep.checkpoint")), "a finished render removes its checkpoint")
    if not os.path.exists(deep_path):
        return
    info = subprocess.run([tool(args, "hoiiotool.exe"), "--info", "-v", deep_path], env=env, capture_output=True,
                          text=True).stdout
    check(" deep " in info, "hoiiotool reads the file as deep")
    inp = oiio.ImageInput.open(deep_path)
    spec = inp.spec()
    names = list(spec.channelnames)
    check(names == ["R", "G", "B", "A", "RA", "GA", "BA", "Z", "depth", "ZBack"],
          f"Karma's deep channels (dcmofsize 3, the depth var): {names}")
    dd = inp.read_native_deep_image(0, 0)
    flat, _ = parts(oiio, np, os.path.join(folder, "deep.exr"))
    beauty = flat[list(flat)[0]]
    zi, ai = names.index("Z"), names.index("A")
    worst, zs = 0.0, {}
    for p in range(dd.pixels):
        n = dd.samples(p)
        order = sorted(range(n), key=lambda s: dd.deep_value(p, zi, s))
        acc, T = np.zeros(4), 1.0
        for s in order:
            a = dd.deep_value(p, ai, s)
            acc += T * np.array([dd.deep_value(p, c, s) for c in (0, 1, 2, ai)])
            T *= 1.0 - a
        y, x = divmod(p, spec.width)
        worst = max(worst, float(np.abs(acc - beauty[y, x, :4]).max()))
        if n:
            zs[(x, y)] = (min(dd.deep_value(p, zi, s) for s in range(n)), max(dd.deep_value(p, zi, s) for s in range(n)))
    check(worst < 2e-3, f"the deep image flattens to the beauty (worst {worst:.2g}; half beauty)")
    # Karma's depth ranges: back 10, behind the 50 % sheet 9..10, the ramp 9.6..9.75 at column 36.
    for (x, lo, hi) in ((5, 10, 10), (12, 9, 10), (25, 9, 10), (36, 9.6024, 9.798)):
        z = zs.get((x, 10))
        check(z is not None and abs(z[0] - lo) < 0.03 and abs(z[1] - hi) < 0.03,
              f"deep depth range at column {x}: {z} vs Karma ({lo}, {hi})")


def windows(spec):
    """(data window, display window) as (x, y, width, height) each."""
    return ((spec.x, spec.y, spec.width, spec.height),
            (spec.full_x, spec.full_y, spec.full_width, spec.full_height))


def check_deep_windows(args, env, oiio, np):
    """A cropped and an overscan render (dataWindowNDC): the deep file's data
    and display windows are the flat product's (husk's), as Karma writes its
    deep files (measured with Karma CPU: the same windows as its flat image)."""
    with open(os.path.join(SCENES, "deep.usda"), encoding="utf-8") as f:
        text = f.read()
    # 40 x 20 pixels: x 10..39 and the upper half; then 4 / 2 pixels around.
    for name, ndc, want in (("crop", "(0.25, 0.5, 1, 1)", (10, 0, 30, 10)),
                            ("overscan", "(-0.1, -0.1, 1.1, 1.1)", (-4, -2, 48, 24))):
        folder = os.path.join(args.out, "deep_" + name)
        shutil.rmtree(folder, ignore_errors=True)
        os.makedirs(folder)
        scene = text.replace("uniform float4 dataWindowNDC = (0, 0, 1, 1)", "uniform float4 dataWindowNDC = " + ndc)
        check(scene != text, f"deep.usda authors dataWindowNDC ({name})")
        with open(os.path.join(folder, "deep.usda"), "w", encoding="utf-8", newline="\n") as f:
            f.write(scene)
        proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "deep.usda"], env, folder)
        check(proc.returncode == 0, f"husk deep.usda ({name}) exits with 0")
        deep_path, flat_path = os.path.join(folder, "deep_deep.exr"), os.path.join(folder, "deep.exr")
        if not (os.path.exists(deep_path) and os.path.exists(flat_path)):
            check(False, f"both products written ({name})")
            continue
        deep = windows(oiio.ImageInput.open(deep_path).spec())
        flat = windows(oiio.ImageInput.open(flat_path).spec())
        check(deep == flat, f"deep windows {deep} are the flat product's {flat} ({name})")
        check(flat[0] == want and flat[1] == (0, 0, 40, 20),
              f"husk's windows {flat}: data {want}, display (0, 0, 40, 20) ({name})")


def check_checkpoint(args, env, oiio, np):
    folder = os.path.join(args.out, "checkpoint")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    # deep.usda with many samples and a large image, so husk's time limit stops it.
    with open(os.path.join(SCENES, "deep.usda"), encoding="utf-8") as f:
        text = f.read()
    text = text.replace("uniform int2 resolution = (40, 20)", "uniform int2 resolution = (400, 200)")
    # Without the 50 % opacity sheet: stochastic opacity hashes the scene's
    # mesh ids, which follow Hydra's parallel sync order, so such renders
    # differ between husk runs (main before P6 as well).
    text = text.replace("color3f inputs:opacity = (0.5, 0.5, 0.5)", "")
    for key in ("karma:global:pathtracedsamples", "karma:global:samplesperpixel", "nova:samples"):
        text = text.replace(f"{key} = 16", f"{key} = 1024")
    with open(os.path.join(folder, "deep.usda"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    cmd = [tool(args, "husk.exe"), "--renderer", "Nova", "deep.usda"]
    stopped = run(cmd + ["--timelimit", "4"], env, folder)
    ckpt = os.path.join(folder, "deep.checkpoint")
    check(os.path.exists(ckpt), "husk's time limit leaves a checkpoint")
    resumed = run(cmd, env, folder)
    check("Resuming from checkpoint" in resumed.stderr + resumed.stdout, "the next render resumes from it")
    check(not os.path.exists(ckpt), "the finished render removes the checkpoint")
    if os.path.exists(os.path.join(folder, "deep.exr")):
        os.replace(os.path.join(folder, "deep.exr"), os.path.join(folder, "resumed.exr"))
    run(cmd, env, folder)  # uninterrupted (no checkpoint left)
    if not (os.path.exists(os.path.join(folder, "resumed.exr")) and os.path.exists(os.path.join(folder, "deep.exr"))):
        check(False, "both renders wrote their image")
        return
    a, _ = parts(oiio, np, os.path.join(folder, "resumed.exr"))
    b, _ = parts(oiio, np, os.path.join(folder, "deep.exr"))
    same = all(np.array_equal(a[k], b[k]) for k in a)
    check(same, "the resumed render equals the uninterrupted one")
    del stopped


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    args.stage = os.path.abspath(args.stage)
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np  # noqa: E402
    import OpenImageIO as oiio  # noqa: E402
    env, prefs = isolated_env(args)
    try:
        check_vars(args, env, oiio, np)
        check_persp(args, env, oiio, np)
        check_crypto(args, env, oiio, np)
        check_deep(args, env, oiio, np)
        check_deep_windows(args, env, oiio, np)
        check_checkpoint(args, env, oiio, np)
    finally:
        shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
