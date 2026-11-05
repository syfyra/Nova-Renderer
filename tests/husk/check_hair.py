"""husk checks of curves and hair against Karma (docs/design/M9.md M9.1, M9.2).

hair_scenes.py writes the research's calibration scenes (docs/research/m9/
curves-hair.md 3); Nova renders a selection with husk and the measurements are
compared with Karma CPU's (section 3.2-3.5, 6):
- geometry: width = diameter (down to 1/10 pixel), Karma's width scale, round
  caps on linear curves and open cubic ends, the curve styles (round by
  default: normals ignored; ribbons facing the ray; oriented ribbons; linear
  curves cannot be oriented), points as spheres, ray:primid = the curve;
- Karma Hair on one fibre: a white fibre scatters all light with no second
  event, its R/T split follows the side of the tube normal, the colour
  controls (absorption, melanin, base colour), the diffuse lobe, and the
  lobes under six distant lights (TT within 2 %: Karma's TT is about 1 %
  above the model, research 8.3), the tilt's sign, azimuthal roughness;
- MaterialX chiang_hair_bsdf renders the spec's fibre (furnace 1), not
  Karma's lowering of it (0.06; a documented difference).

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_hair.py ...
"""

import argparse
import math
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

# Karma CPU (docs/research/m9/curves-hair.md 3.4-3.5): mean radiance over the fibre.
FURNACE = {
    "fur_kma_sa_a0": (0.6983, 0.4247, 0.2111),
    "fur_kma_def_a0": (0.6400, 0.4790, 0.2524),
    "fur_kma_m08_a0": (0.2679, 0.1455, 0.0813),
    "fur_kma_base_a0": (0.9973, 0.9747, 0.8722),
}
SIDE_SPLIT = {"fur_kma_s0_a0": 0.1276, "fur_kma_s0_a30": 0.1479, "fur_kma_s0_a60": 0.2833}  # RG of a white fibre
LIGHTS = {  # per light (LPE C.*<L.'lN'>), irradiance 1 each
    "dl_kma_sa_a0": {"l0": (0.0384, 0.0235, 0.0171), "l1": (0.0403, 0.0303, 0.0214), "l2": (1.1201, 0.6260, 0.2424),
                     "l3": (0.0351, 0.0207, 0.0146), "l4": (0.0351, 0.0207, 0.0146), "l5": (0.0059, 0.0042, 0.0035)},
    "dl_kma_sa_a30": {"l0": (0.0094, 0.0030, 0.0005), "l1": (0.0042, 0.0039, 0.0039), "l2": (1.1171, 0.6044, 0.2240),
                      "l3": (0.0063, 0.0020, 0.0003), "l4": (0.0138, 0.0045, 0.0009)},
    "dl_kma_rough_a0": {"l0": (0.0724, 0.0487, 0.0383), "l1": (0.2364, 0.1551, 0.0860), "l2": (2.0964, 1.1940, 0.4820),
                        "l3": (0.0420, 0.0237, 0.0158)},
    "dl_kma_tilt_a0": {"l3": (0.0328, 0.0179, 0.0116), "l4": (0.0344, 0.0218, 0.0164)},
    "dl_kma_diffuse_a0": {"l0": (0.1250, 0.1250, 0.1250), "l1": (0.0398, 0.0398, 0.0398), "l2": (0.0, 0.0, 0.0)},
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    here = os.path.dirname(os.path.abspath(__file__))
    scenes = os.path.join(args.out, "scenes")
    subprocess.run([sys.executable, "-I", os.path.join(here, "hair_scenes.py"), scenes], check=True,
                   capture_output=True)

    def load(path):
        planes = {}
        image = oiio.ImageInput.open(path)
        if not image:
            return planes
        sub = 0
        while image.seek_subimage(sub, 0):
            spec = image.spec()
            data = image.read_image(sub, 0, 0, spec.nchannels, oiio.FLOAT)
            planes[spec.getattribute("name")] = np.asarray(data, dtype=np.float64).reshape(spec.height, spec.width, -1)
            sub += 1
        image.close()
        return planes

    env, prefs = isolated_env(args)
    names = (["geo_width", "geo_caps", "geo_ribbon", "geo_ids", "fur_mx_s0_a0", "fur_kma_diffuse_a0"] +
             list(FURNACE) + list(SIDE_SPLIT) + list(LIGHTS))
    renders = {}
    for name in names:
        out = os.path.join(args.out, name + ".exr")
        proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "--headlight", "none", "-s", "/Render/settings",
                    "-o", out, os.path.join(scenes, name + ".usda")], env, args.out)
        check(proc.returncode == 0, f"husk {name}.usda exits with 0")
        renders[name] = load(out)

    def grid(plane, width, height):
        h, w = plane.shape[:2]
        x = (np.arange(w) + 0.5) / w * width - width / 2
        y = height / 2 - (np.arange(h) + 0.5) / h * height
        return np.meshgrid(x, y)

    def close(value, expected, rel, absolute=1e-4):
        return abs(value - expected) <= rel * abs(expected) + absolute

    # --- geometry (4 x 1 units at 800 x 200: 5 mm pixels)
    g = renders["geo_width"].get("C")
    if g is not None:
        x, y = grid(g, 4.0, 1.0)
        alpha = g[..., 3]
        pixel = 4.0 / g.shape[1]
        for x0, expected, label in ((-1.6, 0.2, "width 0.2"), (-1.1, 0.0025, "width 0.0025 (1/2 pixel)"),
                                    (-0.8, 0.0005, "width 0.0005 (1/10 pixel)"), (-0.4, 0.2, "pinned B-spline 0.2"),
                                    (1.0, 0.4, "width 0.2 with karma:object:width_scale 2")):
            band = np.abs(x - x0) < max(0.15, expected)
            width = (alpha * band).sum(axis=1).mean() * pixel
            check(close(width, expected, 0.02, 2e-5), f"geo_width {label}: projected width {width:.5f} (Karma {expected})")
        area = (alpha * ((np.abs(x - 0.3) < 0.15) & (np.abs(y) < 0.15))).sum() * pixel * pixel
        check(close(area, math.pi * 0.01, 0.01), f"geo_width: a point of width 0.2 covers {area:.5f} (pi 0.1^2)")
    c = renders["geo_caps"].get("C")
    if c is not None:
        x, y = grid(c, 0.5, 0.5)
        alpha = c[..., 3]
        pixel = 0.5 / c.shape[1]
        lin = (alpha * ((x < 0.0) & (y > 0.0))).sum() * pixel * pixel
        bsp = (alpha * ((x > 0.0) & (y > 0.0))).sum() * pixel * pixel
        check(close(lin, math.pi * 0.05 ** 2 / 2, 0.02), f"geo_caps: a linear curve ends in a round cap ({lin:.6f})")
        check(bsp < 1e-4, f"geo_caps: a cubic curve ends open ({bsp:.6f})")
    r = renders["geo_ribbon"].get("C")
    if r is not None:
        x, y = grid(r, 6.0, 0.5)
        alpha = r[..., 3]
        pixel = 6.0 / r.shape[1]
        for i, (expected, label) in enumerate(((0.2, "unset: round"), (0.2, "0: facing the ray"), (0.2, "1: round"),
                                               (0.1, "2: oriented"), (0.1, "3: USD's rule, oriented"),
                                               (0.2, "linear with style 2: facing the ray"))):
            x0 = -2.5 + i
            width = (alpha * (np.abs(x - x0) < 0.3)).sum(axis=1).mean() * pixel
            check(close(width, expected, 0.02), f"geo_ribbon style {label}: projected width {width:.4f} (Karma {expected})")
    ids = renders["geo_ids"].get("primid")
    if ids is not None:
        x, y = grid(ids, 4.0, 1.0)
        values = [ids[..., 0][(np.abs(x - x0) < 0.02) & (np.abs(y + 0.3) < 0.02)] for x0 in (-1.5, -1.0, -0.5)]
        check(all(len(v) and np.all(v == i) for i, v in enumerate(values)),
              f"geo_ids: ray:primid is the curve ({[float(np.median(v)) if len(v) else None for v in values]})")

    # --- one fibre (mean radiance over it: summed C / summed alpha)
    def fibre(planes, name="C"):
        alpha = planes["C"][..., 3].sum()
        return planes[name][..., :3].sum(axis=(0, 1)) / alpha if name in planes and alpha > 0 else None

    for name, karma in FURNACE.items():
        m = fibre(renders[name])
        check(m is not None and all(close(a, b, 0.005) for a, b in zip(m, karma)),
              f"{name}: furnace {None if m is None else np.round(m, 4).tolist()} (Karma {karma})")
    for name, rg in SIDE_SPLIT.items():
        m, side, indirect = fibre(renders[name]), fibre(renders[name], "RG"), fibre(renders[name], "indirect")
        check(m is not None and all(close(v, 1.0, 0.002) for v in m), f"{name}: a white fibre scatters all light ({m})")
        check(side is not None and close(side[0], rg, 0.015), f"{name}: R side {None if side is None else side[0]:.4f} "
                                                              f"(Karma {rg}: the side of the tube normal)")
        check(indirect is not None and indirect.max() < 1e-3, f"{name}: no second event on one fibre ({indirect})")
    d = fibre(renders["fur_kma_diffuse_a0"], "RD")
    check(d is not None and all(close(v, 0.5, 0.005) for v in d), f"fur_kma_diffuse_a0: diffuse 1 is Lambert 0.5 ({d})")
    mx = fibre(renders["fur_mx_s0_a0"])
    check(mx is not None and all(close(v, 1.0, 0.003) for v in mx),
          f"fur_mx_s0_a0: MaterialX chiang_hair_bsdf scatters all light ({mx}; Karma's lowering: 0.061)")
    for name, lights in LIGHTS.items():
        for light, karma in lights.items():
            m = fibre(renders[name], light)
            check(m is not None and all(close(a, b, 0.02, 2e-4) for a, b in zip(m, karma)),
                  f"{name} {light}: {None if m is None else np.round(m, 4).tolist()} (Karma {karma})")

    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
