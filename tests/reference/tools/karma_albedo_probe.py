"""Karma albedo probe (docs/design/M3.md, M3.12; docs/research/m3/test-strategy.md
section 6, "Re-measuring"): renders the probe materials of
tests/data/albedo_ref.json with Karma CPU and measures Karma's directional
albedos against the oracle (tests/reference/tools/gen_albedo_ref.py). The
measurement is the one nova_tests makes for Nova (tests/test_material_furnace.cpp):
an exact unit sphere (a one-point Points prim) seen orthographically in a
white dome of radiance 1, pixel centers, mean of Karma - oracle in ten mu bins.

Writes tests/reference/karma_albedo.json: per probe the bins, the disk means
and the largest relative bin difference. Karma's material models differ from
the MaterialX reference in places (docs/research/m3/surface-models.md); the
reference scenes' tolerances (manifest.json "reason") rest on these
differences. Re-run on a Houdini or Karma version change, before
regenerating references, and recheck the tolerances.

Needs a Karma license; not part of ctest. Run with Houdini's Python:
  "$HFS/python313/python.exe" tests/reference/tools/karma_albedo_probe.py --hfs "$HFS"
                                [--spp 256] [--only ss_qon_r10,...] [--work <dir>]
"""
import argparse
import json
import math
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # also under python -I
import refcommon as rc  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
ORACLE = os.path.join(ROOT, "tests", "data", "albedo_ref.json")
OUT = os.path.join(ROOT, "tests", "reference", "karma_albedo.json")

SS = "ND_standard_surface_surfaceshader"
OP = "ND_open_pbr_surface_surfaceshader"
COPPER, WARM, WHITE = (0.95, 0.64, 0.54), (0.8, 0.5, 0.2), (1.0, 1.0, 1.0)


def ss(**inputs):
    return SS, dict({"base": 1.0, "specular_IOR": 1.5}, **inputs)


def op(**inputs):
    return OP, dict({"base_weight": 1.0}, **inputs)


# The probes of albedo_ref.json, as tests/test_material_furnace.cpp builds them.
PROBES = {
    "ss_lambert": ss(base_color=WARM, specular=0.0),
    "ss_qon_r05": ss(base_color=WHITE, specular=0.0, diffuse_roughness=0.5),
    "ss_qon_r10": ss(base_color=WHITE, specular=0.0, diffuse_roughness=1.0),
    "pbr_eon_r05": op(base_color=WARM, specular_weight=0.0, base_diffuse_roughness=0.5),
    "pbr_eon_r10": op(base_color=WARM, specular_weight=0.0, base_diffuse_roughness=1.0),
    "ss_metal_r02": ss(metalness=1.0, base_color=COPPER, specular=1.0, specular_color=WHITE, specular_roughness=0.2),
    "ss_metal_r05": ss(metalness=1.0, base_color=COPPER, specular=1.0, specular_color=WHITE, specular_roughness=0.5),
    "ss_metal_r08": ss(metalness=1.0, base_color=COPPER, specular=1.0, specular_color=WHITE, specular_roughness=0.8),
    "ss_metal_edge_r05": ss(metalness=1.0, base_color=(0.9, 0.9, 0.9), specular=1.0, specular_color=(1.0, 0.6, 0.3),
                            specular_roughness=0.5),
    "ss_metal_white_r10": ss(metalness=1.0, base_color=WHITE, specular=1.0, specular_color=WHITE,
                             specular_roughness=1.0),
    "pbr_metal_r03": op(base_metalness=1.0, base_color=COPPER, specular_weight=1.0, specular_color=(1.0, 0.9, 0.75),
                        specular_roughness=0.3),
    "pbr_metal_r07": op(base_metalness=1.0, base_color=COPPER, specular_weight=1.0, specular_color=(1.0, 0.9, 0.75),
                        specular_roughness=0.7),
    "ss_dielectric_r02": ss(base_color=(0.0, 0.0, 0.0), specular=1.0, specular_roughness=0.2),
    "ss_dielectric_r05": ss(base_color=(0.0, 0.0, 0.0), specular=1.0, specular_roughness=0.5),
    "ss_dielectric_r08": ss(base_color=(0.0, 0.0, 0.0), specular=1.0, specular_roughness=0.8),
}

RES = 128
WIDTH = 2.2  # scene units across the image; the sphere has radius 1
BINS = 10


def value(v):
    if isinstance(v, tuple):
        return "color3f", "(%g, %g, %g)" % v
    return "float", "%g" % v


def scene(shader_id, inputs):
    body = "\n".join("            %s inputs:%s = %s" % (value(v)[0], k, value(v)[1]) for k, v in inputs.items())
    return f"""#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "World"
{{
    def Points "ball" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {{
        point3f[] points = [(0, 0, 0)]
        float[] widths = [2]
        rel material:binding = </World/Looks/probe>
    }}
    def DomeLight "sky"
    {{
        float inputs:intensity = 1
        color3f inputs:color = (1, 1, 1)
    }}
    def Camera "cam"
    {{
        token projection = "orthographic"
        float horizontalAperture = {WIDTH * 10:g}
        float verticalAperture = {WIDTH * 10:g}
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
    def Scope "Looks"
    {{
        def Material "probe"
        {{
            token outputs:mtlx:surface.connect = </World/Looks/probe/surface.outputs:out>
            def Shader "surface"
            {{
                uniform token info:id = "{shader_id}"
    {body.replace(chr(10), chr(10) + '    ')}
                token outputs:out
            }}
        }}
    }}
}}

def Scope "Render"
{{
    def RenderSettings "probe"
    {{
        rel camera = </World/cam>
        int2 resolution = ({RES}, {RES})
        rel products = [</Render/Products/beauty>]
        token karma:global:convergence_mode = "Path Traced"
        string karma:global:pixeloracle = '["uniform",{{}}]'
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1e-06
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
        bool karma:global:constrainmaxrough = 0
        float karma:object:diffuselimit = 4
        float karma:object:reflectlimit = 4
        float karma:object:refractlimit = 4
    }}
    def Scope "Products"
    {{
        def RenderProduct "beauty"
        {{
            token productName = "beauty.exr"
            rel orderedVars = [</Render/Vars/C>]
        }}
    }}
    def Scope "Vars"
    {{
        def RenderVar "C"
        {{
            uniform token dataType = "color4f"
            uniform string sourceName = "C.*[LO]"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "color4f"
            string driver:parameters:aov:name = "C"
        }}
    }}
}}
"""


def oracle_at(E, mu):
    """Linear interpolation on the oracle's u = (2 / pi) asin(sqrt(mu)) grid."""
    grid = len(E) - 1
    t = math.asin(math.sqrt(mu)) * (2.0 / math.pi) * grid
    j = min(int(t), grid - 1)
    f = t - j
    return [E[j][c] + f * (E[j + 1][c] - E[j][c]) for c in range(3)]


def profile(pixels, E):
    """Per mu bin: Karma - oracle (RGB), Karma's and the oracle's means."""
    acc = [[[0.0] * 3 for _ in range(3)] for _ in range(BINS)]
    count = [0] * BINS
    for y in range(RES):
        for x in range(RES):
            u = ((x + 0.5) / RES - 0.5) * WIDTH
            v = ((y + 0.5) / RES - 0.5) * WIDTH
            d2 = u * u + v * v
            if d2 >= 1.0:
                continue
            mu = math.sqrt(1.0 - d2)
            b = min(int(mu * BINS), BINS - 1)
            ref = oracle_at(E, mu)
            for c in range(3):
                k = float(pixels[y][x][c])
                acc[b][0][c] += k - ref[c]
                acc[b][1][c] += k
                acc[b][2][c] += ref[c]
            count[b] += 1
    out = []
    for b in range(BINS):
        n = max(count[b], 1)
        out.append({"mu": [b / BINS, (b + 1) / BINS], "pixels": count[b],
                    "karma_minus_oracle": [round(acc[b][0][c] / n, 5) for c in range(3)],
                    "karma": [round(acc[b][1][c] / n, 5) for c in range(3)],
                    "oracle": [round(acc[b][2][c] / n, 5) for c in range(3)]})
    return out


def read_pixels(hfs, exr):
    os.add_dll_directory(os.path.join(hfs, "bin"))
    import OpenImageIO as oiio
    buf = oiio.ImageBuf(exr)
    if buf.has_error:
        raise RuntimeError(buf.geterror())
    return buf.get_pixels(oiio.FLOAT)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--spp", type=int, default=256)
    parser.add_argument("--only", default="")
    parser.add_argument("--work", default="")
    args = parser.parse_args()
    with open(ORACLE, encoding="utf-8") as f:
        oracle = json.load(f)
    names = [n for n in PROBES if not args.only or n in args.only.split(",")]
    work = args.work or tempfile.mkdtemp(prefix="nova_albedo_probe_")
    os.makedirs(work, exist_ok=True)
    version = re.search(r"Houdini (\d+\.\d+\.\d+)", args.hfs)
    results = {}
    if args.only and os.path.exists(OUT):
        with open(OUT, encoding="utf-8") as f:
            results = json.load(f).get("probes", {})
    hfs_env = rc.IsolatedHoudini(args.hfs)
    try:
        for name in names:
            shader_id, inputs = PROBES[name]
            layer = os.path.join(work, name + ".usda")
            with open(layer, "w", encoding="utf-8", newline="\n") as f:
                f.write(scene(shader_id, inputs))
            exr = os.path.join(work, name + ".exr")
            rc.husk(args.hfs, hfs_env.env, "BRAY_HdKarma", layer, "/Render/probe", exr,
                    extra=("-p", str(args.spp), "--lock-random", "1"), log=os.path.join(work, name + ".log"))
            bins = profile(read_pixels(args.hfs, exr), oracle["probes"][name]["E"])
            mass = [b["pixels"] for b in bins]
            disk_k = [sum(b["karma"][c] * m for b, m in zip(bins, mass)) / sum(mass) for c in range(3)]
            disk_o = [sum(b["oracle"][c] * m for b, m in zip(bins, mass)) / sum(mass) for c in range(3)]
            worst = max(((b["karma_minus_oracle"][c] / max(b["oracle"][c], 1e-3), b["mu"][0], c)
                         for b in bins for c in range(3)), key=lambda w: abs(w[0]))
            results[name] = {
                "material": oracle["probes"][name]["material"],
                "disk_karma": [round(v, 5) for v in disk_k],
                "disk_oracle": [round(v, 5) for v in disk_o],
                "disk_rel": [round(k / max(o, 1e-6) - 1.0, 4) for k, o in zip(disk_k, disk_o)],
                "worst_bin_rel": round(worst[0], 4), "worst_bin_mu": worst[1], "worst_bin_channel": worst[2],
                "bins": bins,
            }
            print(f"{name:20s} disk Karma/oracle - 1: "
                  + " ".join(f"{v:+.4f}" for v in results[name]["disk_rel"])
                  + f"   worst bin {worst[0]:+.4f} at mu {worst[1]:.1f} channel {worst[2]}", flush=True)
    finally:
        hfs_env.close()
        if not args.work:
            shutil.rmtree(work, ignore_errors=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"generator": "tests/reference/tools/karma_albedo_probe.py",
                   "houdini": version.group(1) if version else args.hfs,
                   "spp": args.spp, "resolution": RES,
                   "note": "Karma CPU vs tests/data/albedo_ref.json; *_rel = Karma / oracle - 1",
                   "probes": {n: results[n] for n in PROBES if n in results}}, f, indent=1)
        f.write("\n")
    print("wrote", OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
