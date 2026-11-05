"""M7 adaptive sampling prototype, step 1: Nova sample ladders.

Nova's samples are a pure function of (image pixel, sample index, seed,
stream) (core/math/sobol.h), and Renderer::renderPass adds sample index
film.samples to every pixel, so a render with N samples per pixel holds
exactly the first N samples of any longer render with the same seed. A
render ladder N = 2^k (k = kmin..kmax) therefore gives, for every pixel, the
running mean after each power of two: what a pixel would show if an
adaptive Nova stopped it at that checkpoint. as_adaptive_emulator.py replays
stopping rules on these ladders.

Renders with an existing nova_cli (not rebuilt; copy it to a scratch
directory first so concurrent builds cannot replace it) and writes
<work>/<scene>/s<seed>_n<N>.exr plus truth renders t<seed>_n<N>.exr and a
times.json (nova_cli's 'converged in' seconds).

    $HFS/python313/python.exe -I as_nova_ladder.py --cli <nova_cli.exe> --work <dir>
        --scene cornell_rect [--seeds 1] [--kmin 3] [--kmax 10] [--truth 4096] [--truth-seeds 1001,1002]
"""
import argparse
import json
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tests", "reference", "tools"))
import refcommon as rc  # noqa: E402


def render(cli, env, scene_file, settings, spp, seed, out_dir, tag):
    exr = os.path.join(out_dir, f"{tag}.exr")
    if os.path.exists(exr):
        return exr, None
    layer = rc.nova_override_layer(os.path.join(out_dir, f"{tag}.usda"), scene_file, settings, spp, seed)
    proc = rc.run([cli, layer, "--settings", settings, "-o", exr], env, os.path.join(out_dir, f"{tag}.log"))
    if proc.returncode != 0 or not os.path.exists(exr):
        raise RuntimeError(f"nova_cli failed ({proc.returncode}) {tag}: {proc.stderr[-800:]}")
    m = re.search(r"converged in ([0-9.]+) s", proc.stdout)
    return exr, float(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--hfs", default=os.environ.get("HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466"))
    ap.add_argument("--scene", default="cornell_rect")
    ap.add_argument("--seeds", default="1")
    ap.add_argument("--kmin", type=int, default=3)
    ap.add_argument("--kmax", type=int, default=10)
    ap.add_argument("--truth", type=int, default=4096)
    ap.add_argument("--truth-seeds", default="1001,1002")
    ap.add_argument("--extra", default="", help="more sample counts, e.g. 12,24,48 (sqrt(2)-spaced checkpoints)")
    args = ap.parse_args()

    refdir = os.path.join(REPO, "tests", "reference")
    sc = rc.scene_config(rc.load_manifest(refdir), args.scene)
    scene_file = os.path.join(refdir, sc["file"])
    out_dir = os.path.join(args.work, args.scene)
    os.makedirs(out_dir, exist_ok=True)
    times_path = os.path.join(out_dir, "times.json")
    times = json.load(open(times_path)) if os.path.exists(times_path) else {}
    env = rc.IsolatedHoudini(args.hfs)
    try:
        jobs = [(1 << k, int(s), f"s{s}_n{1 << k}") for s in args.seeds.split(",") for k in
                range(args.kmin, args.kmax + 1)]
        jobs += [(int(n), int(s), f"s{s}_n{int(n)}") for s in args.seeds.split(",") for n in args.extra.split(",")
                 if n]
        jobs += [(args.truth, int(s), f"t{s}_n{args.truth}") for s in args.truth_seeds.split(",") if s]
        for spp, seed, tag in jobs:
            exr, t = render(args.cli, env.env, scene_file, sc["settings"], spp, seed, out_dir, tag)
            if t is not None:
                times[tag] = t
                print(f"{args.scene} {tag}: {t:.3f} s", flush=True)
                with open(times_path, "w") as f:
                    json.dump(times, f, indent=1)
    finally:
        env.close()


if __name__ == "__main__":
    main()
