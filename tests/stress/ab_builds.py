"""Two nova_cli builds over the reference scenes (tests/reference/manifest.json): per scene the
relative difference of the means, the share of pixels that agree within 1e-3 and whether the
images are identical. For changes that move every image in its last bits.

  $HFS/python313/python.exe ab_builds.py --old <nova_cli.exe> --new <nova_cli.exe> --hfs <HFS> --repo <repo>
        --out <dir> [--spp 16] [--device cpu|xpu] [--only name]
"""
import argparse
import concurrent.futures as cf
import json
import os
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--hfs", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--spp", default="16")
    ap.add_argument("--device", default="cpu")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio
    os.makedirs(args.out, exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = os.path.join(args.hfs, "bin") + os.pathsep + env["PATH"]
    env.pop("PXR_PLUGINPATH_NAME", None)
    ref = os.path.join(args.repo, "tests", "reference")
    scenes = json.load(open(os.path.join(ref, "manifest.json")))["scenes"]

    def render(scene, which, cli):
        img = os.path.join(args.out, "%s_%s_%s.exr" % (scene["name"], args.device, which))
        if os.path.exists(img):
            os.remove(img)
        cmd = [cli, os.path.join(ref, scene["file"]), "--settings", scene["settings"], "-o", img, "--quiet", "--spp", args.spp,
               "--device", args.device, "--threads", "8", "--timeout", "600"]
        p = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=args.out)
        return img if os.path.exists(img) else "rc=%s %s" % (p.returncode, p.stderr.strip()[-300:])

    def load(path):
        inp = oiio.ImageInput.open(path)
        px = inp.read_image(0, 0, 0, min(inp.spec().nchannels, 4), oiio.FLOAT)
        inp.close()
        return px

    def one(scene):
        a, b = render(scene, "old", args.old), render(scene, "new", args.new)
        if not (a.endswith(".exr") and b.endswith(".exr")):
            return "%-22s FAILED old: %s | new: %s" % (scene["name"], a[-200:], b[-200:])
        pa, pb = load(a), load(b)
        if pa.shape != pb.shape:
            return "%-22s shape %s vs %s" % (scene["name"], pa.shape, pb.shape)
        ma, mb = float(pa[..., :3].mean()), float(pb[..., :3].mean())
        diff = np.abs(pa - pb)
        close = float((diff.max(axis=2) <= 1e-3 * np.maximum(1.0, np.abs(pa).max(axis=2))).mean())
        rel = abs(ma - mb) / max(abs(ma), 1e-6)
        flag = "" if rel < 2e-3 and close > 0.97 else "   <== look"
        return "%-22s mean %.6f -> %.6f (rel %.2e)  pixels within 1e-3: %.4f  identical: %s%s" % (
            scene["name"], ma, mb, rel, close, bool((pa == pb).all()), flag)

    todo = [s for s in scenes if not args.only or s["name"].startswith(args.only)]
    with cf.ThreadPoolExecutor(args.jobs if args.device == "cpu" else 1) as ex:
        for line in ex.map(one, todo):
            print(line, flush=True)


if __name__ == "__main__":
    main()
