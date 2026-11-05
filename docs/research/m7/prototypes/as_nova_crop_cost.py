"""M7 adaptive sampling prototype: Nova's cost per sample in image regions.

Renders crops of a reference scene (dataWindowNDC on the RenderSettings
prim) with nova_cli on one thread and reports seconds per pixel-sample, so
the emulator's sample counts can be turned into time for pixels that see
only the dome (camera ray misses) and pixels that see geometry.

    $HFS/python313/python.exe -I as_nova_crop_cost.py --cli <nova_cli.exe> --work <dir> --scene mtl_glass
        --crop sky:0,0.8,1,1 --crop glass:0.42,0.4,0.58,0.6 [--spp 256]
"""
import argparse
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tests", "reference", "tools"))
import refcommon as rc  # noqa: E402

LAYER = """#usda 1.0
(
    subLayers = [@{scene}@]{timing}
)

over "Render"
{{
    over "{prim}"
    {{
        int nova:samples = {spp}
        int nova:seed = 7
        float4 dataWindowNDC = ({x0}, {y0}, {x1}, {y1})
    }}
}}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--hfs", default=os.environ.get("HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466"))
    ap.add_argument("--scene", default="mtl_glass")
    ap.add_argument("--spp", type=int, default=256)
    ap.add_argument("--crop", action="append", default=[])
    ap.add_argument("--repeat", type=int, default=2)
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    refdir = os.path.join(REPO, "tests", "reference")
    sc = rc.scene_config(rc.load_manifest(refdir), args.scene)
    scene_file = os.path.join(refdir, sc["file"])
    prim = sc["settings"].rsplit("/", 1)[1]
    timing = "".join(f"\n    {k} = {v}" for k, v in rc.layer_timing(scene_file).items())
    with open(scene_file, encoding="utf-8") as f:
        res = re.search(r"resolution = \((\d+), (\d+)\)", f.read())
    w, h = int(res.group(1)), int(res.group(2))
    env = rc.IsolatedHoudini(args.hfs)
    try:
        for c in args.crop:
            name, box = c.split(":")
            x0, y0, x1, y1 = (float(v) for v in box.split(","))
            layer = os.path.join(args.work, f"{args.scene}_{name}.usda")
            with open(layer, "w", encoding="utf-8", newline="\n") as f:
                f.write(LAYER.format(scene=rc.usd_path(scene_file), timing=timing, prim=prim, spp=args.spp,
                                     x0=x0, y0=y0, x1=x1, y1=y1))
            px = round((x1 - x0) * w) * round((y1 - y0) * h)
            best = None
            for r in range(args.repeat):
                exr = os.path.join(args.work, f"{args.scene}_{name}.exr")
                proc = rc.run([args.cli, layer, "--settings", sc["settings"], "-o", exr, "--threads", "1"], env.env)
                m = re.search(r"converged in ([0-9.]+) s", proc.stdout)
                if not m:
                    raise RuntimeError(proc.stdout + proc.stderr)
                t = float(m.group(1))
                best = t if best is None else min(best, t)
            print(f"{args.scene} {name}: ~{px} px, {args.spp} spp, 1 thread: {best:.3f} s = "
                  f"{best / (px * args.spp) * 1e6:.2f} us per pixel-sample", flush=True)
    finally:
        env.close()


if __name__ == "__main__":
    main()
