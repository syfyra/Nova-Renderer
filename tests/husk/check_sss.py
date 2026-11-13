"""husk checks of random-walk subsurface scattering against Karma (docs/design/M9.md M9.3).

sss_scenes.py writes the research's calibration scenes (docs/research/m9/sss.md
section 4); Nova renders a selection with husk and the means are compared with
Karma CPU's measured values (section 4.2-4.6): white furnaces of thick and
small spheres (albedo inversion, anisotropy, the 256-segment cap, chromatic
mean free paths, OpenPBR, a mix with diffuse, SSS limit 0), slabs lit from
behind (transmission, anisotropy, oblique light, thin walls, an open mesh),
the lit side's angular profile, light bled past a terminator, and the room
whose walls count SSS events (limits and light path expressions).

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_sss.py ...
"""

import argparse
import json
import math
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

# Karma CPU's means (docs/research/m9/sss.md 4.2-4.6), red / green / blue.
FURNACE = {
    "f_a1": (0.2028, 0.5019, 0.8001),
    "f_a2": (0.0519, 0.9118, 0.9202),  # the cap darkens 0.95 / 0.99
    "f_gp5": (0.2006, 0.5016, 0.7966),
    "f_gm5": (0.2094, 0.5047, 0.8015),
    "f_gp9": (0.2021, 0.5044, 0.7207),
    "f_s01": (0.2335, 0.5730, 0.9006),
    "f_s1": (0.6197, 0.8913, 0.9876),
    "f_chroma": (0.9618, 0.9250, 0.8812),
    "f_opbr": (0.2028, 0.5019, 0.8001),
    "f_lim0": (0.2028, 0.5019, 0.8001),  # past the SSS limit the exit still gathers the dome
    "f_mix": (0.5004, 0.5004, 0.5004),
}
SLABS = {
    "slab_d0p05": (0.1534, 0.1731, 0.1757),
    "slab_d0p1": (0.0877, 0.1187, 0.1234),
    "slab_d0p4": (0.0043, 0.0311, 0.0442),
    "slab_chroma": (0.1731, 0.1187, 0.0690),
    "slab_gp5": (0.1484, 0.1720, 0.1749),
    "slab_a60": (0.0438, 0.0593, 0.0617),
    "slab_thin": (0.1592, 0.2546, 0.3024),
    "slab_refl": (0.5014 / math.pi, 0.7983 / math.pi, 0.9089 / math.pi),
    "open_quad": (0.1596, 0.2541, 0.2894),
}
# Light bled past the terminator of a sphere lit from +x (mfp 0.2), at x.
TERMINATOR = {-0.54: (0.0041, 0.0113, 0.0131), -0.13: (0.0125, 0.0253, 0.0282), 0.15: (0.0386, 0.0596, 0.0639)}
# The lit side's radiance at mu = 0.95 (exit and entry both Lambertian).
HEAD = (0.0614, 0.1521, 0.2429)
# A camera inside a room of emissive SSS walls: one SSS event per walk.
ROOM = {
    "room_l0": {"C": (1.0, 1.0, 1.0), "td_O": (0.0, 0.0, 0.0)},
    "room_l1": {"C": (1.2514, 1.4998, 1.7466), "td_O": (0.2514, 0.4998, 0.7466), "td_td_O": (0.0, 0.0, 0.0)},
    "room_l2": {"C": (1.3146, 1.7495, 2.3032), "td_td_O": (0.0632, 0.2496, 0.5567), "rd_all": (0.0, 0.0, 0.0)},
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
    scenes_dir = os.path.join(args.out, "scenes")
    subprocess.run([sys.executable, "-I", os.path.join(here, "sss_scenes.py"), scenes_dir], check=True,
                   capture_output=True)
    meta = json.load(open(os.path.join(scenes_dir, "scenes.json"), encoding="utf-8"))

    def load(path):
        planes = {}
        image = oiio.ImageInput.open(path)
        if not image:
            return planes
        sub = 0
        while image.seek_subimage(sub, 0):
            spec = image.spec()
            data = image.read_image(sub, 0, 0, spec.nchannels, oiio.FLOAT)
            planes[spec.getattribute("name")] = np.asarray(data, dtype=np.float64).reshape(
                spec.height, spec.width, -1)[:, :, :3]
            sub += 1
        image.close()
        return planes

    env, prefs = isolated_env(args)
    renders = {}
    names = list(FURNACE) + list(SLABS) + list(ROOM) + ["term_s0p2", "head"]
    for name in names:
        out = os.path.join(args.out, name + ".exr")
        # No headlight: the rooms have no light, and the one husk then adds casts no shadows (its
        # ShadowAPI): it lights the walls from outside the closed room, in Karma as in Nova since 0.2.1
        # (room_l0 1.308, 1.613, 1.918 in both). The values here are the rooms' own light.
        proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "--headlight", "none", "-s", "/Render/s", "-o", out,
                    os.path.join(scenes_dir, name + ".usda")], env, args.out)
        check(proc.returncode == 0, f"husk {name}.usda exits with 0")
        renders[name] = load(out)

    def coords(name, plane):
        m = meta[name]
        h, w = plane.shape[:2]
        width = m.get("width", 1.0)
        height = width * h / w
        x = (np.arange(w) + 0.5) / w * width - width / 2
        y = height / 2 - (np.arange(h) + 0.5) / h * height
        return np.meshgrid(x, y)

    def mean_se(values):
        values = np.asarray(values)
        return values.mean(axis=0), values.std(axis=0, ddof=1) / math.sqrt(max(len(values), 1))

    def compare(label, nova, se, karma, rel=0.01):
        ok = all(abs(n - k) <= 4.0 * s + rel * k + 2e-4 for n, s, k in zip(nova, se, karma))
        check(ok, f"{label}: Nova ({', '.join('%.4f' % v for v in nova)}) vs Karma "
                  f"({', '.join('%.4f' % v for v in karma)})")

    def disc(name, plane):
        x, y = coords(name, plane)
        r = meta[name]["inner"] * meta[name]["radius"]
        return plane[x * x + y * y < r * r]

    for name, karma in FURNACE.items():
        c = renders[name].get("C")
        if c is None:
            check(False, f"{name}: beauty missing")
            continue
        mean, se = mean_se(disc(name, c))
        compare(f"{name} furnace", mean, se, karma)
        if name in ("f_a1", "f_chroma"):
            sss, _ = mean_se(disc(name, renders[name]["sss"]))
            rd, _ = mean_se(disc(name, renders[name]["rd_all"]))
            check(np.allclose(sss, mean, rtol=1e-4, atol=1e-6) and np.all(rd == 0.0),
                  f"{name}: all of it is C<...'sss'>.*, none C<RD>.* (one TD event per walk)")
    for name, karma in SLABS.items():
        c = renders[name].get("C")
        if c is None:
            check(False, f"{name}: beauty missing")
            continue
        mean, se = mean_se(c.reshape(-1, 3))
        compare(f"{name} slab", mean, se, karma)

    c = renders["head"].get("C")
    if c is not None:
        x, y = coords("head", c)
        r2 = x * x + y * y
        mu = np.sqrt(np.clip(1.0 - r2, 0.0, 1.0))
        mean, se = mean_se(c[(r2 < 0.98) & (mu >= 0.9)])
        compare("head: radiance at mu 0.9-1 (Lambertian entry and exit)", mean, se, HEAD, rel=0.015)
    c = renders["term_s0p2"].get("C")
    if c is not None:
        x, _ = coords("term_s0p2", c)
        for x0, karma in TERMINATOR.items():
            col = int(np.argmin(np.abs(x[0] - x0)))
            mean, se = mean_se(c[:, col])
            compare(f"term_s0p2: light bled to x = {x0}", mean, se, karma, rel=0.05)
    for name, expected in ROOM.items():
        for var, karma in expected.items():
            p = renders[name].get(var)
            if p is None:
                check(False, f"{name}: {var} missing")
                continue
            mean, se = mean_se(p.reshape(-1, 3))
            compare(f"{name} {var}", mean, se, karma)

    try:
        import shutil
        shutil.rmtree(prefs, ignore_errors=True)
    except Exception:
        pass
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
