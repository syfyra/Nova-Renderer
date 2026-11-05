"""Builds the reference images in tests/reference/images with Karma CPU.

For every scene in manifest.json (or --only a,b):
  * type "karma": two independent Karma renders (seeds reference.seeds, each
    reference.spp_half samples) become images/<name>.exr (their mean, half
    float, or 32-bit float with reference.float) and images/<name>.sigma.exr (|A - B| / 2 per pixel, the standard
    error of the mean). Karma's own settings are checked from the EXR's
    husk:render_stats. Two more Karma renders at the test sample count must
    pass against the new reference (self check); their FLIP/relMSE are
    recorded as the noise floor.
  * type "constant" / "dome_irradiance": the analytic value is stored; with
    karma_check, Karma must reproduce it too (validates the scene).
  * reference.file: Karma renders this variant of the scene instead (the
    same image by construction, written in a form Karma renders exactly).
  * aovs: the scene's product has these AOV planes after the beauty; each
    gets images/<name>/<plane>.exr and .sigma.exr the same way, and its
    own self check (M6.3).
Writes images/<name>.json with the provenance (input hashes, Karma version,
sample counts, floors). Everything is built in --work and moved into
--refdir only after the self check passed, the JSON last; --spp-half (dry
runs) never touches --refdir unless --force is given. Needs a Karma license;
not part of ctest.

Usage:
  python make_references.py --hfs <HFS> --imgcmp <nova_imgcmp.exe> --refdir tests/reference
                            --work <scratch dir> [--only cornell_rect,...] [--spp-half N [--force]]
"""

import argparse
import datetime
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # also under python -I
import refcommon as rc  # noqa: E402


def check_karma_settings(hfs, exr, spp, size):
    stats = rc.render_stats(hfs, exr)
    rs = stats.get("render_settings", {})
    problems = []
    if rs.get("convergence_mode") != "Path Traced":
        problems.append(f"convergence_mode {rs.get('convergence_mode')}")
    if "uniform" not in str(rs.get("pixeloracle")):
        problems.append(f"pixeloracle {rs.get('pixeloracle')}")
    if rs.get("colorlimit", 0) < 1e8 or not (rs.get("colorlimitindlinked") or rs.get("colorlimitindirect", 0) >= 1e8):
        problems.append("color limit active")
    if rs.get("pathtracedsamples") != spp:
        problems.append(f"pathtracedsamples {rs.get('pathtracedsamples')} != {spp}")
    if rs.get("constrainmaxrough", False):
        problems.append("constrainmaxrough on")
    primary = stats.get("ray_counts", {}).get("primary")
    if primary != size[0] * size[1] * spp:
        problems.append(f"primary rays {primary} != {size[0]}x{size[1]}x{spp} (adaptive sampling?)")
    if problems:
        raise RuntimeError(f"{exr}: Karma settings differ from the reference recipe: {'; '.join(problems)}")
    return stats.get("karma_version", "?"), rs.get("pixelfilter")


def karma_pair(args, hfs_env, scene, seeds, spp, tag):
    out = []
    karma_file = scene["reference"].get("file", scene["file"])
    for seed in seeds:
        exr = os.path.join(args.work, f"{scene['name']}_{tag}_s{seed}.exr")
        rc.husk(args.hfs, hfs_env.env, "BRAY_HdKarma", os.path.join(args.refdir, karma_file), scene["settings"], exr,
                extra=["-p", str(spp), "--lock-random", str(seed)],
                log=os.path.join(args.work, f"{scene['name']}_{tag}_s{seed}.log"))
        out.append(exr)
    return out


def make_scene(args, hfs_env, scene):
    """Builds the reference in args.work. Returns its info and the files to
    move into the reference directory ([(built, destination)])."""
    name, ref = scene["name"], scene["reference"]
    info = {"scene": name, "created": datetime.date.today().isoformat(),
            "inputs": rc.reference_inputs(args.refdir, scene)}
    spp_half = args.spp_half or ref["spp_half"]
    files = []

    if ref["type"] == "karma":
        a, b = karma_pair(args, hfs_env, scene, ref["seeds"], spp_half, "ref")
        size = rc.image_size(args.hfs, a)
        for exr in (a, b):
            info["karma"], info["pixelfilter"] = check_karma_settings(args.hfs, exr, spp_half, size)
        image, sigma = os.path.join(args.work, f"{name}.exr"), os.path.join(args.work, f"{name}.sigma.exr")
        proc = rc.run([args.imgcmp, "--make-ref", a, b, "--out", image, "--out-sigma", sigma,
                       "--attrib", f"nova:reference={name}", "--attrib", f"nova:spp={2 * spp_half}",
                       *(["--float"] if ref.get("float") else [])], hfs_env.env)
        if proc.returncode != 0:
            raise RuntimeError(proc.stderr)
        info.update(spp=2 * spp_half, image=f"images/{name}.exr", sigma=f"images/{name}.sigma.exr")
        files = [(image, os.path.join(args.refdir, info["image"])), (sigma, os.path.join(args.refdir, info["sigma"]))]
        ref_args = ["--ref", image, "--ref-sigma", sigma]
        plane_refs = {}
        for plane in scene.get("aovs", []):
            pa = rc.extract_plane(args.hfs, hfs_env.env, a, plane, os.path.join(args.work, f"{name}_a_{plane}.exr"))
            pb = rc.extract_plane(args.hfs, hfs_env.env, b, plane, os.path.join(args.work, f"{name}_b_{plane}.exr"))
            p_image = os.path.join(args.work, f"{name}.{plane}.exr")
            p_sigma = os.path.join(args.work, f"{name}.{plane}.sigma.exr")
            proc = rc.run([args.imgcmp, "--make-ref", pa, pb, "--out", p_image, "--out-sigma", p_sigma,
                           "--attrib", f"nova:reference={name}/{plane}", "--attrib", f"nova:spp={2 * spp_half}",
                           *(["--float"] if ref.get("float") else [])], hfs_env.env)
            if proc.returncode != 0:
                raise RuntimeError(proc.stderr)
            rel_image, rel_sigma = f"images/{name}/{plane}.exr", f"images/{name}/{plane}.sigma.exr"
            info.setdefault("aovs", {})[plane] = {"image": rel_image, "sigma": rel_sigma}
            files += [(p_image, os.path.join(args.refdir, rel_image)), (p_sigma, os.path.join(args.refdir, rel_sigma))]
            plane_refs[plane] = ["--ref", p_image, "--ref-sigma", p_sigma]
    else:
        plane_refs = {}
        if ref["type"] == "constant":
            value = list(ref["value"])
        else:  # dome_irradiance: Lambert plane under the dome, direct light only
            e = rc.dome_irradiance(args.hfs, os.path.join(args.refdir, ref["texture"]), ref["intensity"])
            value = [ref["albedo"] / 3.141592653589793 * v for v in e] + [1.0]
        info["value"] = value
        ref_args = ["--ref-const", ",".join(f"{v:.7g}" for v in value)]
        if not ref.get("karma_check"):
            return info, files

    # Self check: Karma at the test sample count against the new reference.
    c, d = karma_pair(args, hfs_env, scene, ref["check_seeds"], scene["test"]["spp"], "check")
    code, result, err = rc.run_imgcmp(args.imgcmp, hfs_env.env, ref_args, c, d, scene["compare"],
                                      os.path.join(args.work, f"{name}_check.json"))
    if code != 0:
        raise RuntimeError(f"{name}: Karma at {scene['test']['spp']} spp fails against its own reference "
                           f"({err.strip() or result})")
    keys = ("pixels_failed", "worst_pixel") if result.get("mode") == "pixel" else (
        "flip_mean", "flip_test_pair", "relmse", "global_z")
    info["karma_floor"] = {k: result.get(k) for k in keys}
    for plane, plane_args in plane_refs.items():
        pc = rc.extract_plane(args.hfs, hfs_env.env, c, plane, os.path.join(args.work, f"{name}_c_{plane}.exr"))
        pd = rc.extract_plane(args.hfs, hfs_env.env, d, plane, os.path.join(args.work, f"{name}_d_{plane}.exr"))
        code, result, err = rc.run_imgcmp(args.imgcmp, hfs_env.env, plane_args, pc, pd, rc.plane_compare(scene, plane),
                                          os.path.join(args.work, f"{name}_check_{plane}.json"))
        if code != 0:
            raise RuntimeError(f"{name}: Karma's {plane} at {scene['test']['spp']} spp fails against its own "
                               f"reference ({err.strip() or result})")
        info["aovs"][plane]["karma_floor"] = {k: result.get(k) for k in keys}
    return info, files


def install(args, name, info, files):
    """Moves a validated reference into place: each file atomically, the
    JSON (which marks it current) last. An interrupted run can leave new
    images next to the old JSON; check_references then rejects the pair
    (input hash and spp mismatch) until the run is repeated."""
    for built, destination in files:
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        shutil.copyfile(built, destination + ".tmp")
        os.replace(destination + ".tmp", destination)
    path = os.path.join(args.refdir, "images", f"{name}.json")
    with open(path + ".tmp", "w", encoding="utf-8", newline="\n") as f:
        json.dump(info, f, indent=2)
        f.write("\n")
    os.replace(path + ".tmp", path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--imgcmp", required=True)
    parser.add_argument("--refdir", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--only", default="")
    parser.add_argument("--spp-half", type=int, default=0,
                        help="override reference.spp_half (dry runs: the result stays in --work)")
    parser.add_argument("--force", action="store_true", help="install a --spp-half run into --refdir anyway")
    args = parser.parse_args()
    os.makedirs(args.work, exist_ok=True)
    dry_run = bool(args.spp_half) and not args.force
    manifest = rc.load_manifest(args.refdir)
    names = [s["name"] for s in manifest["scenes"] if not args.only or s["name"] in args.only.split(",")]
    hfs_env = rc.IsolatedHoudini(args.hfs)
    failures = 0
    try:
        for name in names:
            scene = rc.scene_config(manifest, name)
            if scene.get("status") == "pending":
                print(f"skip {name}: pending ({scene.get('requires', '')})")
                continue
            try:
                info, files = make_scene(args, hfs_env, scene)
            except RuntimeError as e:
                print(f"FAIL {name}: {e}")
                failures += 1
                continue
            summary = json.dumps({k: info.get(k) for k in ('spp', 'value', 'karma_floor')})
            if dry_run:
                print(f"ok   {name} (dry run, kept in {args.work}): {summary}")
                continue
            install(args, name, info, files)
            print(f"ok   {name}: {summary}")
    finally:
        hfs_env.close()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
