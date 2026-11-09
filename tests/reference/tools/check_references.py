"""Reference test of one scene (run by ctest, label "reference").

Renders the scene twice with Nova through husk, or with nova_cli if --cli is
given (two seeds, so the test's own variance is known), then compares with
nova_imgcmp against the Karma reference in tests/reference/images (or the
analytic value). Exit codes: 0 pass, 1 fail, 77 skipped (scene pending in
manifest.json).

Usage:
  python check_references.py --hfs <HFS> --stage <staged nova package> --imgcmp <nova_imgcmp.exe>
                             --refdir tests/reference --out <dir> --scene <name> [--spp N] [--cli <nova_cli.exe>]
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # also under python -I
import refcommon as rc  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--imgcmp", required=True)
    parser.add_argument("--refdir", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--spp", type=int, default=0, help="override test.spp (e.g. converged runs)")
    parser.add_argument("--cli", default="", help="render with this nova_cli instead of husk")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    scene = rc.scene_config(rc.load_manifest(args.refdir), args.scene)
    name = scene["name"]
    if scene.get("status") == "pending":
        print(f"{name}: pending ({scene.get('requires', 'not implemented yet')})")
        return rc.SKIP
    if args.cli and scene.get("cli", True) is False:
        # Houdini's imaging adapters (USD_HD.dll) are not in nova_cli: e.g. the
        # Karma Sky Atmosphere reaches only husk (docs/design/M11-P7.md).
        print(f"{name}: husk only")
        return rc.SKIP

    # The reference must have been made from the current scene files.
    info_path = os.path.join(args.refdir, "images", f"{name}.json")
    if not os.path.exists(info_path):
        print(f"FAIL {name}: no reference (run tools/make_references.py --only {name})")
        return 1
    with open(info_path, encoding="utf-8") as f:
        info = json.load(f)
    current = rc.reference_inputs(args.refdir, scene)
    if current != info["inputs"]:
        changed = sorted(k for k in set(current) | set(info["inputs"]) if current.get(k) != info["inputs"].get(k))
        print(f"FAIL {name}: reference is out of date, changed inputs {changed} "
              f"(run tools/make_references.py --only {name})")
        return 1

    if scene["reference"]["type"] == "karma":
        # The image must be the one the JSON describes, with the sample count
        # the manifest asks for (not a dry run).
        image = os.path.join(args.refdir, info["image"])
        spp = rc.exr_attribute(args.hfs, image, "nova:spp")
        if spp != str(info["spp"]) or info["spp"] < 2 * scene["reference"]["spp_half"]:
            print(f"FAIL {name}: reference image has nova:spp {spp}, its JSON {info['spp']}, the manifest "
                  f"{2 * scene['reference']['spp_half']} (run tools/make_references.py --only {name})")
            return 1
        ref_args = ["--ref", image, "--ref-sigma", os.path.join(args.refdir, info["sigma"])]
    else:
        ref_args = ["--ref-const", ",".join(f"{v:.7g}" for v in info["value"])]

    hfs_env = rc.IsolatedHoudini(args.hfs, args.stage)
    try:
        spp = args.spp or scene["test"]["spp"]
        renders = []
        for seed in scene["test"]["seeds"]:
            layer = rc.nova_override_layer(os.path.join(args.out, f"{name}_s{seed}.usda"),
                                           os.path.join(args.refdir, scene["file"]), scene["settings"], spp, seed)
            exr = os.path.join(args.out, f"{name}_s{seed}.exr")
            log = os.path.join(args.out, f"{name}_s{seed}.log")
            if args.cli:
                cmd = [args.cli, layer, "--settings", scene["settings"], "-o", exr, "--quiet"]
                proc = rc.run(cmd, hfs_env.env, log)
                if proc.returncode != 0 or not os.path.exists(exr):
                    raise RuntimeError(f"nova_cli failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stderr[-1500:]}")
            else:
                rc.husk(args.hfs, hfs_env.env, "Nova", layer, scene["settings"], exr, log=log)
            renders.append(exr)
        code, result, err = rc.run_imgcmp(args.imgcmp, hfs_env.env, ref_args, renders[0], renders[1],
                                          {k: v for k, v in scene["compare"].items() if k != "aovs"},
                                          os.path.join(args.out, f"{name}.json"),
                                          os.path.join(args.out, f"{name}_flip.png"))
        # AOV planes (M6.3): each against its own reference.
        plane_failures = []
        for plane in scene.get("aovs", []):
            ref = info["aovs"][plane]
            tests = [rc.extract_plane(args.hfs, hfs_env.env, exr, plane,
                                      os.path.join(args.out, f"{name}_s{seed}_{plane}.exr"))
                     for exr, seed in zip(renders, scene["test"]["seeds"])]
            p_code, p_result, p_err = rc.run_imgcmp(
                args.imgcmp, hfs_env.env,
                ["--ref", os.path.join(args.refdir, ref["image"]), "--ref-sigma", os.path.join(args.refdir, ref["sigma"])],
                tests[0], tests[1], rc.plane_compare(scene, plane), os.path.join(args.out, f"{name}_{plane}.json"))
            if p_code == 2 or not p_result:
                plane_failures.append(f"{plane}: nova_imgcmp error: {p_err.strip()}")
                continue
            worst = p_result["worst_block"]
            line = (f"{plane}: global rel {p_result['global_rel_diff']} z {p_result['global_z']}, blocks failed "
                    f"{p_result['blocks_failed']}/{p_result['block_tests']} (worst at {worst['x']},{worst['y']} "
                    f"ch {worst['channel']}: {worst['diff_over_allowed']:.2f} of allowed)")
            print(f"{'ok  ' if p_code == 0 else 'FAIL'} {name}/{line}")
            if p_code != 0:
                plane_failures.append(line)
    finally:
        hfs_env.close()

    if code == 2 or not result:
        print(f"FAIL {name}: nova_imgcmp error: {err.strip()}")
        return 1
    if result.get("mode") == "pixel":
        worst = result["worst_pixel"]
        print(f"{'ok  ' if code == 0 else 'FAIL'} {name}: {result['pixels_failed']} pixels outside "
              f"{result['pixel_tol']} (worst at {worst['x']},{worst['y']} ch {worst['channel']}: ref {worst['ref']}, "
              f"test {worst['test']}, {worst['diff_over_allowed']:.2f} of allowed)")
        return 0 if code == 0 else 1
    if plane_failures:
        code = 1
    worst = result["worst_block"]
    print(f"{'ok  ' if code == 0 else 'FAIL'} {name}: global rel {result['global_rel_diff']} z {result['global_z']}, "
          f"blocks failed {result['blocks_failed']}/{result['block_tests']} (worst at {worst['x']},{worst['y']} "
          f"ch {worst['channel']}: {worst['diff_over_allowed']:.2f} of allowed), FLIP {result['flip_mean']:.4f} "
          f"(test pair {result['flip_test_pair']:.4f}), ref noise ratio {result['ref_noise_ratio']:.2f}")
    if result["ref_noise_ratio"] > 1.0:
        print(f"note {name}: the reference is noisier than the test renders; more reference samples would "
              f"make the test stricter")
    return 0 if code == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
