"""Karma probe of ND_blackbody (docs/design/M8.md D14): Karma's blackbody has
MaterialX's chromaticity but a brightness that grows roughly like T^4
(docs/research/m8/houdini-volumes.md 4.4), unlike MaterialX's luminance-1
reference. Renders 257 unlit rows, row i emitting blackbody(T_i) with T_i
log-uniform from 500 K to 40000 K, point-sampled by an orthographic camera,
and writes
  tests/data/karma_blackbody.json   every measured value (the odd rows test
                                    Nova's interpolation), and
  core/volume/blackbody_table.h     the even rows as Nova's table: Rec.709
                                    luminance and luminance-normalized RGB.
Needs a Karma license; not part of ctest. Run with Houdini's Python:
    "$HFS/python313/python.exe" tests/karma/probe_blackbody.py
"""
import json
import math
import os
import subprocess
import sys
import tempfile

HFS = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
os.add_dll_directory(os.path.join(HFS, "bin"))
import OpenImageIO as oiio

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = tempfile.mkdtemp(prefix="nova_blackbody_")
GOLDEN = os.path.join(ROOT, "tests", "data", "karma_blackbody.json")
TABLE = os.path.join(ROOT, "core", "volume", "blackbody_table.h")
T_MIN, T_MAX, COUNT = 500.0, 40000.0, 257
W = 2
TEMPS = [T_MIN * (T_MAX / T_MIN) ** (i / (COUNT - 1)) for i in range(COUNT)]
REC709_Y = (0.2126, 0.7152, 0.0722)


def scene():
    rows, mats = [], []
    n = len(TEMPS)
    for k, t in enumerate(TEMPS):
        y = -k / W
        name = f"t{k:03d}"
        rows.append(f"""
    def Mesh "{name}" (prepend apiSchemas = ["MaterialBindingAPI"])
    {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, {y - 0.5 / W}, 0), (1, {y - 0.5 / W}, 0), (1, {y + 0.5 / W}, 0), (0, {y + 0.5 / W}, 0)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </Looks/{name}>
    }}""")
        mats.append(f"""
    def Material "{name}"
    {{
        token outputs:kma:surface.connect = </Looks/{name}/s.outputs:out>
        def Shader "s"
        {{
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color.connect = </Looks/{name}/bb.outputs:out>
            token outputs:out
        }}
        def Shader "bb"
        {{
            uniform token info:id = "ND_blackbody"
            float inputs:temperature = {t!r}
            color3f outputs:out
        }}
    }}""")
    return f"""#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{{{''.join(rows)}
    def Camera "cam"
    {{
        token projection = "orthographic"
        float horizontalAperture = 10
        float verticalAperture = {n * 10 / W}
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0.5, {-(n - 1) / (2 * W)}, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
}}
def Scope "Looks"
{{{''.join(mats)}
}}
def Scope "Render"
{{
    def RenderSettings "s"
    {{
        rel camera = </World/cam>
        int2 resolution = ({W}, {n})
        rel products = [</Render/Products/p>]
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1e-06
        token karma:global:convergence_mode = "Path Traced"
        float karma:global:colorlimit = 1e9
    }}
    def Scope "Products"
    {{
        def RenderProduct "p"
        {{
            token productName = "bb.exr"
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


def main():
    if "--table-only" in sys.argv: # rewrite the header from the golden data
        with open(GOLDEN) as f:
            write_table(json.load(f)["rgb"])
        return
    path = os.path.join(OUT, "bb_probe.usda")
    open(path, "w", newline="\n").write(scene())
    prefs = tempfile.mkdtemp(prefix="nova_hprefs_")
    env = dict(os.environ)
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for v in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH", "HOME"):
        env.pop(v, None)
    env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    exr = os.path.join(OUT, "bb_probe.exr")
    r = subprocess.run([os.path.join(HFS, "bin", "husk.exe"), "--renderer", "BRAY_HdKarma", "--headlight", "none",
                        "-s", "/Render/s", "-p", "1", "-o", exr, path], env=env, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-2000:])
        raise SystemExit(1)
    px = oiio.ImageBuf(exr).get_pixels(oiio.FLOAT)
    rgb = [[float(px[k][0][c]) for c in range(3)] for k in range(len(TEMPS))]
    with open(GOLDEN, "w", newline="\n") as f:
        json.dump({"generator": "tests/karma/probe_blackbody.py", "houdini": os.path.basename(HFS),
                   "kelvin": TEMPS, "rgb": rgb}, f, indent=1)
        f.write("\n")
    print("wrote", GOLDEN)
    write_table(rgb)
    for t_ref, karma_ref in ((2500.0, (1.4342, 0.5323, 0.0963)), (6000.0, (24.89, 22.31, 21.64))):
        k = min(range(len(TEMPS)), key=lambda i: abs(math.log(TEMPS[i] / t_ref)))
        print("near %.0f K (%.0f K): %s, research (volumes) at %.0f K: %s" % (t_ref, TEMPS[k], rgb[k], t_ref, karma_ref))


def write_table(rgb):
    entries = []
    for k in range(0, len(TEMPS), 2):
        c = rgb[k]
        y = sum(w * v for w, v in zip(REC709_Y, c))
        y = max(y, 1e-30)
        entries.append((y, [v / y for v in c]))
    lines = [
        "#pragma once",
        "",
        "// Generated by tests/karma/probe_blackbody.py from Karma CPU's ND_blackbody",
        f"// ({os.path.basename(HFS)}): Rec.709 luminance and luminance-normalized RGB at",
        f"// {len(entries)} temperatures, log-uniform from {T_MIN:g} K to {T_MAX:g} K. Do not edit.",
        "",
        "#define NOVA_BLACKBODY_TABLE_MIN_KELVIN " + repr(T_MIN) + "f",
        "#define NOVA_BLACKBODY_TABLE_MAX_KELVIN " + repr(T_MAX) + "f",
        f"#define NOVA_BLACKBODY_TABLE_COUNT {len(entries)}",
        "// { luminance, r, g, b } per entry",
        "#define NOVA_BLACKBODY_TABLE_DATA \\",
    ]
    body = []
    for y, c in entries:
        body.append("    {%.8ef, %.8ef, %.8ef, %.8ef}" % (y, c[0], c[1], c[2]))
    lines.append("    {" + ", \\\n".join(body).lstrip() + "}")
    with open(TABLE, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", TABLE)


main()
