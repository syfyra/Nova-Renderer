"""Renders every scene of a directory with Karma CPU (isolated Houdini env)
and writes the radial albedo profile + disk mean of each render.
Usage: python -I run_karma.py <scenedir> <spp> [names,...]"""
import json, os, shutil, subprocess, sys, tempfile, time

HFS = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
os.add_dll_directory(os.path.join(HFS, "bin"))
import numpy as np  # noqa: E402
import OpenImageIO as oiio  # noqa: E402


def isolated_env():
    prefs = tempfile.mkdtemp(prefix="nova_hprefs_")
    env = dict(os.environ)
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for var in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
        env.pop(var, None)
    env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    return env, prefs


def render(env, scene, out, spp, seed=1):
    cmd = [os.path.join(HFS, "bin", "husk.exe"), "--renderer", "BRAY_HdKarma", "--headlight", "none", "-s",
           "/Render/ref", "-o", out, "-p", str(spp), "--lock-random", str(seed), scene]
    t0 = time.time()
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    dt = time.time() - t0
    if proc.returncode != 0 or not os.path.exists(out):
        print("FAILED", scene, proc.returncode, proc.stderr[-1500:], proc.stdout[-500:])
        return None, dt
    return out, dt


def profile(exr, nbins=10):
    px = oiio.ImageBuf(exr).get_pixels(oiio.FLOAT)  # (h, w, 4)
    h, w = px.shape[:2]
    # orthographic: 2.2 units across the image, sphere radius 1
    ys, xs = np.mgrid[0:h, 0:w]
    u = ((xs + 0.5) / w - 0.5) * 2.2
    v = ((ys + 0.5) / h - 0.5) * 2.2
    r = np.sqrt(u * u + v * v)
    inside = r < 0.97
    mu = np.sqrt(np.clip(1 - r * r, 0, 1))
    rgb = px[..., :3]
    mean = rgb[inside].mean(axis=0)
    # disk mean with weights making it the cosine-weighted average over mu (uniform area = 2 mu dmu)
    bins = []
    edges = np.linspace(0.25, 1.0, nbins + 1)
    for a, b in zip(edges[:-1], edges[1:]):
        m = inside & (mu >= a) & (mu < b)
        if m.any():
            bins.append([float(0.5 * (a + b))] + [float(c) for c in rgb[m].mean(axis=0)])
    alpha_in = float(px[..., 3][inside].mean())
    alpha_out = float(px[..., 3][r > 1.05].mean())
    bg = float(rgb[r > 1.05].mean())
    return {"mean": [float(c) for c in mean], "profile": bins, "alpha_in": alpha_in, "alpha_out": alpha_out,
            "bg": bg, "radius_px_check": float(inside.sum())}


def main():
    sdir, spp = sys.argv[1], int(sys.argv[2])
    names = sys.argv[3].split(",") if len(sys.argv) > 3 else sorted(
        f[:-5] for f in os.listdir(sdir) if f.endswith(".usda") and f not in ("common.usda", "sphere.usda"))
    env, prefs = isolated_env()
    results = {}
    rpath = os.path.join(sdir, f"results_{spp}.json")
    if os.path.exists(rpath):
        results = json.load(open(rpath))
    try:
        for n in names:
            exr, dt = render(env, os.path.join(sdir, n + ".usda"), os.path.join(sdir, n + ".exr"), spp)
            if not exr:
                continue
            res = profile(exr)
            res["seconds"] = round(dt, 2)
            results[n] = res
            print(f"{n:22s} mean {res['mean'][0]:.4f} {res['mean'][1]:.4f} {res['mean'][2]:.4f}  "
                  f"alpha {res['alpha_in']:.3f}  {dt:.1f}s", flush=True)
            json.dump(results, open(rpath, "w"), indent=1)
    finally:
        shutil.rmtree(prefs, ignore_errors=True)


if __name__ == "__main__":
    main()
