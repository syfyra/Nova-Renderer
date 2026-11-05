"""husk checks of Karma's render vars (docs/design/M6.md).

tests/scenes/karma_aovs.usda is the scene of docs/research/m6/karma-aovs.md:
Karma Render Settings with every AOV toggle and Karma Standard Render Vars,
as Karma's LOPs author them (lights tagged "key" and "fill", a sphere with
the object tag "metal"). Nova renders it with husk into one multi-part EXR.

M6.2: the light path expressions. Components that partition the beauty sum
to it (as in Karma, docs/research/m6/probes/karma_partition.txt), combined =
direct + indirect, light groups and tags select their lights and objects,
the coat is part of the glossy reflection. M6.3: unoccluded; exceeds the
beauty, shadow; is its occluded fraction, CDA is the diffuse albedo and
!C<TG>*B the alpha through glass (Karma's measured relations). M6.4:
tests/scenes/utility_aovs.usda's utility planes against its geometry.
M6.5: tests/scenes/crypto.usda's cryptomattes: names, hashes, metadata.

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_aovs.py ...
"""

import argparse
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--scenes", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--cli", default="", help="nova_cli: the cryptomatte checks also for its EXR writer")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    def load(path):
        planes = {}
        image = oiio.ImageInput.open(path)
        sub = 0
        while image.seek_subimage(sub, 0):
            spec = image.spec()
            data = image.read_image(sub, 0, 0, spec.nchannels, oiio.FLOAT)
            planes[spec.getattribute("name")] = np.asarray(data, dtype=np.float64).reshape(
                spec.height, spec.width, -1)
            sub += 1
        image.close()
        return planes

    env, prefs = isolated_env(args)
    try:
        out = os.path.join(args.out, "karma_aovs.exr")
        cmd = [tool(args, "husk.exe"), "--renderer", "Nova", "-s", "/Render/krs", "-o", out,
               os.path.join(args.scenes, "karma_aovs.usda")]
        proc = run(cmd, env, args.out)
        check(proc.returncode == 0, "husk karma_aovs.usda exits with 0")
        p = load(out)

        def rgb(name):
            return p[name][..., :3]

        C = rgb("C")
        ref = float(C.mean())
        check(ref > 0.05, f"the beauty is lit (mean {ref:.4f})")

        def same(title, a, b, tolerance=0.002):
            # half-float planes: compare the mean difference to the mean beauty
            d = float(np.abs(a - b).mean()) / ref
            check(d < tolerance, f"{title}: mean|diff| {100 * d:.3f} % of the beauty")

        first_events = ["visiblelights", "combineddiffuse", "combinedglossyreflection", "glossytransmission", "sss",
                        "combinedvolume", "combinedemission"]
        same("C = CL + C<RD>.*L + C<RG>.*L + C<TG>.*L + C<TD>.*L + CV.*L + C.*O", C,
             sum(rgb(n) for n in first_events))
        for combined, direct, indirect in (("combineddiffuse", "directdiffuse", "indirectdiffuse"),
                                           ("combinedglossyreflection", "directglossyreflection",
                                            "indirectglossyreflection"),
                                           ("combinedemission", "directemission", "indirectemission")):
            same(f"{combined} = {direct} + {indirect}", rgb(combined), rgb(direct) + rgb(indirect))
        same("C.*O (split per tag) = C.*O (combined)", rgb("C_emission"), rgb("combinedemission"))
        groups = rgb("combineddiffuse_key") + rgb("combineddiffuse_fill")
        excess = float((groups - rgb("combineddiffuse")).max())
        check(excess < 2e-3, f"the key and fill groups are part of C<RD>.*L (max excess {excess:.2g})")
        for name in ("C_key", "C_fill", "combineddiffuse", "combinedglossyreflection", "glossytransmission", "sss",
                     "combinedemission", "coat"):
            check(float(rgb(name).mean()) > 0.0, f"{name} is lit")
        excess = float((rgb("coat") - rgb("combinedglossyreflection")).max())
        check(excess < 2e-3, f"the coat is part of the glossy reflection (max excess {excess:.2g})")
        for name in ("combinedvolume", "visiblelights"):
            check(float(np.abs(rgb(name)).max()) == 0.0, f"{name} is empty (no volumes, no visible lights)")

        # M6.3: the prefixes and the A and B events.
        U = rgb("beautyunshadowed")
        check(float(U.mean()) > float(C.mean()), f"beautyunshadowed {U.mean():.4f} > C {C.mean():.4f}")
        lit = U > 1e-3
        derived = float((1.0 - C[lit] / U[lit]).mean())
        shadow = float(rgb("shadow")[lit].mean())
        check(abs(shadow - derived) < 0.02, f"shadow is the occluded fraction: {shadow:.4f} vs 1 - C/U {derived:.4f}")
        same("albedo (CDA) = albedodiffuse (CDA)", rgb("albedo"), rgb("albedodiffuse"))
        alpha = p["C"][..., 3]
        through = p["alphatransmission"][..., 0]
        check(abs(float(through.mean()) - float(alpha.mean())) < 0.03,
              f"!C<TG>*B {through.mean():.4f} is the alpha seen through glass (alpha {alpha.mean():.4f})")
        check(float((through - alpha).max()) < 2e-3, "!C<TG>*B never exceeds the alpha (half floats)")

        # M6.4: utility planes of tests/scenes/utility_aovs.usda, checked
        # against the geometry (Karma's conventions, measured).
        out = os.path.join(args.out, "utility_aovs.exr")
        cmd = [tool(args, "husk.exe"), "--renderer", "Nova", "-s", "/Render/s", "-o", out,
               os.path.join(args.scenes, "utility_aovs.usda")]
        proc = run(cmd, env, args.out)
        check(proc.returncode == 0, "husk utility_aovs.usda exits with 0")
        u = load(out)
        P, alpha = u["P"], u["C"][..., 3]
        eye = np.array([0.0, 0.0, 6.0])
        floor = (np.abs(P[..., 2]) < 1e-4) & (alpha > 0.999) & (P[..., 0] > -2.95) & (P[..., 0] < -0.05)             & (P[..., 1] > -2.45) & (P[..., 1] < -0.55)
        check(int(floor.sum()) > 500, f"the 3-face mesh covers {int(floor.sum())} pixels")
        x, y = P[..., 0][floor], P[..., 1][floor]
        face = np.where(x < -1.5, 0, np.where(y < -2.5 + (x + 1.5) * 4.0 / 3.0, 1, 2))
        for plane in ("primid", "element"):
            got = u[plane][..., 0][floor]
            bad = int((np.abs(got - face) > 1e-6).sum())
            check(bad == 0, f"{plane} (int var, husk:format float) is the face index: {bad} pixels differ")
        check(float(np.abs(u["P_camera"][floor] - (P[floor] - eye)).max()) < 1e-4, "P_camera = P - eye")
        dist = np.linalg.norm(P[floor] - eye, axis=1)
        check(float(np.abs(u["hitdist"][..., 0][floor] - dist).max()) < 1e-3, "hitdist is the distance from the eye")
        check(float(np.abs(u["FN"][..., 0][floor] - 6.0 / dist).max()) < 1e-3, "the facing ratio is |N . I|")
        st = u["UV"][floor]
        check(float(np.abs(st[:, 0] - (x + 3.0) / 3.0).max()) < 1e-3 and float(np.abs(st[:, 1] - (y + 2.5) / 2.0).max()) < 1e-3,
              "the st primvar AOV interpolates the authored st")
        mover = (np.abs(P[..., 2] + 1.0) < 1e-4) & (alpha > 0.999)
        check(int(mover.sum()) > 100, f"the mover covers {int(mover.sum())} pixels")
        mv, vel = u["mv"][mover], u["vel"][mover]
        check(float(np.abs(mv[:, :2] - 25.0).max()) < 0.01, f"motion vectors are 25 px right and up per shutter "
                                                           f"(max error {float(np.abs(mv[:, :2] - 25.0).max()):.4f})")
        check(float(np.abs(vel - np.array([1.0, 1.0, 0.0])).max()) < 1e-4, "velocity is the shutter's world motion")
        lid = (np.abs(P[..., 2] - 0.5) < 1e-4) & (alpha > 0.999)
        check(float(u["N"][..., 2][lid].max()) < -0.999, "N keeps the lid's authored orientation (down)")
        check(float(np.abs(u["basecolor"][floor] - np.array([0.8, 0.2, 0.1])).max()) < 1e-6, "basecolor is base_color")
        ao_free, ao_lid = float(u["AO"][..., 0][floor & (P[..., 0] > -1.5)].mean()), float(
            u["AO"][..., 0][floor & (P[..., 0] < -2.62) & (P[..., 0] > -2.98)].mean())
        check(ao_free > 0.97 and ao_lid < 0.8, f"AO: open floor {ao_free:.3f}, under the lid {ao_lid:.3f}")

        # M6.5: cryptomattes of tests/scenes/crypto.usda: Karma's names,
        # hashes, layers and metadata (docs/research/m6/cryptomatte.md).
        writers = [("husk", [tool(args, "husk.exe"), "--renderer", "Nova", "-s", "/Render/s"])]
        if args.cli:
            writers.append(("nova_cli", [args.cli, "--settings", "/Render/s", "--quiet"]))
        for writer, prefix in writers:
            # One kind of separator: husk puts the sidecar next to the product,
            # taking the directory up to the last "/".
            folder = os.path.join(args.out, writer)
            os.makedirs(folder, exist_ok=True)
            out = os.path.join(folder, "crypto.exr").replace("\\", "/")
            sidecar = os.path.join(folder, "crypto_materials.json")
            if os.path.exists(sidecar):
                os.remove(sidecar)
            proc = run(prefix + ["-o", out, os.path.join(args.scenes, "crypto.usda")], env, folder)
            check(proc.returncode == 0, f"{writer}: crypto.usda renders")
            image = oiio.ImageInput.open(out)
            header = {a.name: a.value for a in image.spec().extra_attribs if a.name.startswith("cryptomatte/")}
            image.close()
            c = load(out)
            alpha = c["C"][..., 3]
            expected = {
                "CryptoPrimitives": {"/World/plain", "/World/faces", "/World/unbound", "/World/inst/protos/pebble",
                                     "/World/inst/protos/rock", "/World/refA/ball"},
                "CryptoMaterials": {"/World/mtl/red", "/World/mtl/blue", "defaultshader"},
                "CryptoAsset": {"alpha", "beta"},
                "CryptoPiece": {"42", "-3"},
            }
            ranks = {"CryptoPrimitives": 6, "CryptoMaterials": 6, "CryptoAsset": 3, "CryptoPiece": 2}
            for layer, names in expected.items():
                keys = [k.split("/")[1] for k, v in header.items() if k.endswith("/name") and v == layer]
                check(len(keys) == 1, f"{writer} {layer}: one metadata block ({keys})")
                if not keys:
                    continue
                key = keys[0]
                if f"cryptomatte/{key}/manif_file" in header:
                    check(header[f"cryptomatte/{key}/manif_file"] == "crypto_materials.json" and os.path.exists(sidecar),
                          f"{layer}: the manifest is in the sidecar husk wrote")
                    with open(sidecar, encoding="utf-8") as f:
                        manifest = json.load(f)
                else:
                    manifest = json.loads(header[f"cryptomatte/{key}/manifest"])
                check(set(manifest) == names, f"{layer}: manifest names {sorted(manifest)} == {sorted(names)}")
                check(header[f"cryptomatte/{key}/hash"] == "MurmurHash3_32" and
                      header[f"cryptomatte/{key}/conversion"] == "uint32_to_float32", f"{layer}: hash and conversion")
                planes = [c[f"{layer}{k:02d}"] for k in range((ranks[layer] + 1) // 2)]
                check(f"{layer}{len(planes):02d}" not in c, f"{layer}: {len(planes)} rank layers for rank {ranks[layer]}")
                ids, cover = [], np.zeros(alpha.shape)
                for plane in planes:
                    for pair in range(2):
                        cov = plane[..., 2 * pair + 1]
                        cover += cov
                        bits = plane[..., 2 * pair].astype(np.float32).view(np.uint32)[cov > 0]
                        ids.extend(int(b) for b in np.unique(bits))
                known = {int(v, 16) for v in manifest.values()}
                check(set(ids) <= known, f"{layer}: every id with coverage is in the manifest")
                if layer in ("CryptoPrimitives", "CryptoMaterials"): # every visible surface has a name
                    check(float(np.abs(cover - alpha).max()) < 1e-5, f"{layer}: the coverage sums to the alpha")
            check(json.loads(header[[k for k, v in header.items() if v == "CryptoPrimitives"][0].replace("/name", "/manifest")])
                  ["/World/inst/protos/rock"] == "8082046e", "the exponent fixup (rock: 8002046e -> 8082046e)")
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
