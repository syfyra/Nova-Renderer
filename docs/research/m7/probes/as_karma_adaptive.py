"""M7 adaptive sampling probe 2: Karma CPU's pixel oracles measured.

Renders a reference scene with husk + Karma CPU (Path Traced convergence, as
the reference images) under the uniform oracle at several sample counts and
under the variance oracle at several thresholds, with Karma's per-pixel
sample count AOV (primarysamples = intrinsic ray:pixelsamples, max blending).
For every render it records the wall time, Karma's own render time and ray
counts (husk:render_stats in the EXR), the per-pixel sample count
distribution and the error against the scene's high-sample Karma reference
in tests/reference/images (RMSE and relMSE over RGB; the reference's own
variance, estimated from its .sigma.exr, is subtracted as well).

Houdini's python (numpy + OpenImageIO), isolated husk via
tests/reference/tools/refcommon.py:
    $HFS/python313/python.exe -I as_karma_adaptive.py --work <scratch dir>
        [--scene cornell_rect] [--uniform 16,32,...] [--variance 0.01,...]
        [--max 1024] [--minrays -1] [--seed 5] [--plane-stats]
Writes <work>/<scene>_karma_adaptive.json and prints a table.
"""
import argparse
import json
import os
import subprocess
import sys
import time

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
        string karma:global:pixeloracle = '{oracle}'
        rel products = [</Render/Products/adaptive>]
    }}
    over "Products"
    {{
        def RenderProduct "adaptive"
        {{
            token productName = "adaptive.exr"
            rel orderedVars = [</Render/Vars/C>, </Render/Vars/primarysamples>{extra_vars}]
        }}
    }}
    over "Vars"
    {{
        def RenderVar "primarysamples" (
            prepend apiSchemas = ["KarmaRenderVarAPI", "HuskRenderVarAPI"]
        )
        {{
            uniform token dataType = "int"
            token driver:parameters:aov:husk:format = "int"
            bool driver:parameters:aov:husk:multiSampled = 0
            string driver:parameters:aov:husk:name = "primarysamples"
            string driver:parameters:aov:husk:channel_prefix = "primarysamples"
            string driver:parameters:aov:karma:filter = '["minmax",{{"mode":"max"}}]'
            uniform string sourceName = "ray:pixelsamples"
            uniform token sourceType = "intrinsic"
        }}
{stat_vars}
    }}
}}
"""

STAT_VAR = """        def RenderVar "{name}" (
            prepend apiSchemas = ["KarmaRenderVarAPI", "HuskRenderVarAPI"]
        )
        {{
            uniform token dataType = "color3f"
            token driver:parameters:aov:husk:format = "color3f"
            string driver:parameters:aov:husk:name = "{name}"
            string driver:parameters:aov:husk:channel_prefix = "{name}"
            string driver:parameters:aov:karma:filter = '["stats",{{"mode":"{mode}"}}]'
            uniform string sourceName = "C.*[LO]"
            uniform token sourceType = "lpe"
        }}
"""
STATS = [("C_even", "even"), ("C_odd", "odd"), ("C_var", "var"), ("C_mse", "mse")]


def read_parts(path):
    """{part name: (channel names, float array HxWxC)} of a (multi-part) EXR."""
    out = {}
    inp = oiio.ImageInput.open(path)
    if not inp:
        raise RuntimeError(oiio.geterror())
    sub = 0
    while inp.seek_subimage(sub, 0):
        spec = inp.spec()
        name = spec.getattribute("name") or ("part%d" % sub)
        # Integer planes (primarysamples is uint32) are read as integers: a
        # float read would normalize them by 2^32 - 1.
        fmt = oiio.UINT if spec.format.basetype == oiio.UINT else oiio.FLOAT
        px = inp.read_image(sub, 0, 0, spec.nchannels, fmt)
        out[name] = (list(spec.channelnames), np.asarray(px, dtype=np.float64).reshape(spec.height, spec.width, -1))
        sub += 1
    inp.close()
    return out


def read_rgb(path):
    buf = oiio.ImageBuf(path)
    px = np.asarray(buf.get_pixels(oiio.FLOAT), dtype=np.float64)
    return px[..., :3]


def errors(img, ref, ref_var):
    d = img - ref
    mse = float(np.mean(d * d))
    rel = float(np.mean(d * d / (ref * ref + 1e-2)))
    rel_ref = float(np.mean(ref_var / (ref * ref + 1e-2)))
    return {"rmse": mse ** 0.5, "mse": mse, "mse_minus_ref": mse - float(np.mean(ref_var)),
            "relmse": rel, "relmse_minus_ref": rel - rel_ref}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--hfs", default=os.environ.get("HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466"))
    ap.add_argument("--scene", default="cornell_rect")
    ap.add_argument("--uniform", default="16,64,256,1024")
    ap.add_argument("--variance", default="0.01,0.003,0.001,0.0003")
    ap.add_argument("--max", type=int, default=1024)
    ap.add_argument("--minrays", type=int, default=-1)
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--plane-stats", action="store_true", help="also write C even/odd/var/mse planes")
    ap.add_argument("--tag", default="")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)

    refdir = os.path.join(REPO, "tests", "reference")
    manifest = rc.load_manifest(refdir)
    sc = rc.scene_config(manifest, args.scene)
    scene_file = os.path.join(refdir, sc["file"])
    settings = sc["settings"]
    prim = settings.rsplit("/", 1)[1]
    timing = "".join(f"\n    {k} = {v}" for k, v in rc.layer_timing(scene_file).items())
    ref = read_rgb(os.path.join(refdir, "images", args.scene + ".exr"))
    sigma = read_rgb(os.path.join(refdir, "images", args.scene + ".sigma.exr"))
    ref_var = sigma * sigma

    runs = []
    for spp in [int(s) for s in args.uniform.split(",") if s]:
        runs.append(("uniform", spp, '["uniform",{}]', None))
    for v in [float(s) for s in args.variance.split(",") if s]:
        runs.append(("variance", args.max, json.dumps(["variance", {"minrays": args.minrays, "plane": "C", "variance": v}]), v))

    env = rc.IsolatedHoudini(args.hfs)
    results = []
    try:
        for kind, spp, oracle, thr in runs:
            tag = f"{args.scene}{args.tag}_{kind}_{spp}" + (f"_v{thr:g}_m{args.minrays}" if thr is not None else "")
            layer = os.path.join(args.work, tag + ".usda")
            extra_vars = "".join(f", </Render/Vars/{n}>" for n, _ in STATS) if args.plane_stats else ""
            stat_vars = "".join(STAT_VAR.format(name=n, mode=m) for n, m in STATS) if args.plane_stats else ""
            with open(layer, "w", encoding="utf-8", newline="\n") as f:
                f.write(LAYER.format(scene=rc.usd_path(scene_file), timing=timing, prim=prim, oracle=oracle,
                                     extra_vars=extra_vars, stat_vars=stat_vars))
            exr = os.path.join(args.work, tag + ".exr")
            t0 = time.perf_counter()
            rc.husk(args.hfs, env.env, "BRAY_HdKarma", layer, settings, exr,
                    extra=["-p", str(spp), "--lock-random", str(args.seed)], log=os.path.join(args.work, tag + ".log"))
            wall = time.perf_counter() - t0
            stats = rc.render_stats(args.hfs, exr)
            parts = read_parts(exr)
            names = list(parts)
            beauty = parts[names[0]][1][..., :3]
            ps = None
            for n, (ch, px) in parts.items():
                if "primarysamples" in n or any("primarysamples" in c for c in ch):
                    ps = px[..., 0]
            row = {"kind": kind, "spp": spp, "threshold": thr, "minrays": args.minrays if thr is not None else None,
                   "oracle": oracle, "wall_s": wall, "exr": exr, "parts": names}
            row.update(errors(beauty, ref, ref_var))
            if ps is not None:
                row["samples"] = {"mean": float(ps.mean()), "min": float(ps.min()), "max": float(ps.max()),
                                  "p10": float(np.percentile(ps, 10)), "p50": float(np.percentile(ps, 50)),
                                  "p90": float(np.percentile(ps, 90)),
                                  "hist": {str(int(k)): int(c) for k, c in zip(*np.unique(ps, return_counts=True))}}
            rcount = stats.get("ray_counts", {})
            row["primary_rays"] = rcount.get("primary")
            # Karma's own wall clock of the render (husk start-up and scene
            # load excluded); the CPU time of its threads.
            row["render_s"] = stats.get("system_time", {}).get("wallclock")
            row["threads"] = stats.get("threads")
            row["karma_timings"] = stats.get("timings")
            row["ray_counts"] = rcount
            for k in ("render_time", "total_time", "ttime", "render_seconds"):
                if k in stats:
                    row["stats_" + k] = stats[k]
            row["stats_keys"] = sorted(stats.keys())
            row["timing"] = {k: v for k, v in stats.items() if "time" in k.lower() or "memory" in k.lower()}
            results.append(row)
            s = row.get("samples", {})
            print(f"{tag:48s} wall {wall:7.2f}s  prim/px {(row['primary_rays'] or 0) / ref.shape[0] / ref.shape[1]:8.1f}  "
                  f"spp mean {s.get('mean', 0):7.1f} [{s.get('min', 0):.0f}..{s.get('max', 0):.0f}]  "
                  f"rmse {row['rmse']:.5f}  relmse {row['relmse']:.6f} (-ref {row['relmse_minus_ref']:.6f})", flush=True)
    finally:
        env.close()
    out = os.path.join(args.work, f"{args.scene}{args.tag}_karma_adaptive.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=1)
    print("wrote", out)


if __name__ == "__main__":
    main()
