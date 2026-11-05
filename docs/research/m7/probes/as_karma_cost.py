"""M7 adaptive sampling probe 3: a per-pixel cost map from Karma.

Nova has no per-pixel time AOV, so the adaptive emulator weights samples by
Karma's: a uniform-oracle, Path Traced render of the reference scene with
the cputime AOV (intrinsic stat:cputime, accumulate blending) and the ray
limits of the scene, divided by the sample count = CPU seconds per sample
per pixel. Karma's paths follow the same limits and lights, so the map is a
proxy for where Nova spends its time (cheap background, expensive glass).
Measured: Karma's cputime is constant over each 32x32 bucket (the bucket's
time spread over its pixels), so the map only resolves buckets; its unit is
not seconds per pixel (relative values only).

    $HFS/python313/python.exe -I as_karma_cost.py --work <dir> --scene mtl_glass [--spp 128]
Writes <work>/<scene>_cost.exr (one channel, seconds per sample).
"""
import argparse
import os
import sys

HFS_DEFAULT = os.environ.get("HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466")
os.add_dll_directory(os.path.join(HFS_DEFAULT, "bin"))
import numpy as np  # noqa: E402
import OpenImageIO as oiio  # noqa: E402

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
        token karma:global:convergence_mode = "Path Traced"
        string karma:global:pixeloracle = '["uniform",{{}}]'
        rel products = [</Render/Products/cost>]
    }}
    over "Products"
    {{
        def RenderProduct "cost"
        {{
            token productName = "cost.exr"
            rel orderedVars = [</Render/Vars/C>, </Render/Vars/cputime>]
        }}
    }}
    over "Vars"
    {{
        def RenderVar "cputime" (
            prepend apiSchemas = ["KarmaRenderVarAPI", "HuskRenderVarAPI"]
        )
        {{
            uniform token dataType = "float"
            token driver:parameters:aov:husk:format = "float"
            bool driver:parameters:aov:husk:multiSampled = 0
            string driver:parameters:aov:husk:name = "cputime"
            string driver:parameters:aov:husk:channel_prefix = "cputime"
            string driver:parameters:aov:karma:filter = '["accumulate",{{}}]'
            uniform string sourceName = "stat:cputime"
            uniform token sourceType = "intrinsic"
        }}
    }}
}}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--hfs", default=HFS_DEFAULT)
    ap.add_argument("--scene", default="mtl_glass")
    ap.add_argument("--spp", type=int, default=128)
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    refdir = os.path.join(REPO, "tests", "reference")
    sc = rc.scene_config(rc.load_manifest(refdir), args.scene)
    scene_file = os.path.join(refdir, sc["file"])
    prim = sc["settings"].rsplit("/", 1)[1]
    timing = "".join(f"\n    {k} = {v}" for k, v in rc.layer_timing(scene_file).items())
    layer = os.path.join(args.work, f"{args.scene}_cost.usda")
    with open(layer, "w", encoding="utf-8", newline="\n") as f:
        f.write(LAYER.format(scene=rc.usd_path(scene_file), timing=timing, prim=prim))
    exr = os.path.join(args.work, f"{args.scene}_cost_karma.exr")
    env = rc.IsolatedHoudini(args.hfs)
    try:
        rc.husk(args.hfs, env.env, "BRAY_HdKarma", layer, sc["settings"], exr, extra=["-p", str(args.spp)],
                log=os.path.join(args.work, f"{args.scene}_cost.log"))
    finally:
        env.close()
    inp = oiio.ImageInput.open(exr)
    cost = None
    sub = 0
    while inp.seek_subimage(sub, 0):
        spec = inp.spec()
        if "cputime" in (spec.getattribute("name") or ""):
            cost = np.asarray(inp.read_image(sub, 0, 0, 1, oiio.FLOAT), dtype=np.float32)[..., 0] / args.spp
        sub += 1
    inp.close()
    out = os.path.join(args.work, f"{args.scene}_cost.exr")
    spec = oiio.ImageSpec(cost.shape[1], cost.shape[0], 1, oiio.FLOAT)
    o = oiio.ImageOutput.create(out)
    o.open(out, spec)
    o.write_image(np.ascontiguousarray(cost[..., None]))
    o.close()
    q = np.percentile(cost, [1, 10, 50, 90, 99])
    print(f"{args.scene}: cost per sample (s) mean {cost.mean():.3e}, percentiles 1/10/50/90/99 "
          + " ".join(f"{v:.2e}" for v in q) + f", max/median {cost.max() / max(q[2], 1e-12):.1f}")


if __name__ == "__main__":
    main()
