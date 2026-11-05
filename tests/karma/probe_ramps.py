"""Karma probe of Houdini's ramps (kma_rampconst_float) for the M4.3 ramp
lowering (docs/design/M4.md): every basis, two key sets, evaluated at
t = (x + 0.5) / 200 along an unlit row (st.s, point-sampled). Writes
tests/data/karma_ramps.json (every 4th sample), the golden values of
nova_hydra_tests "Houdini ramps match Karma". Needs a Karma license; not part
of ctest. Run with Houdini's Python:
    "$HFS/python313/python.exe" tests/karma/probe_ramps.py
"""
import os, subprocess, sys, tempfile, json
HFS = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
os.add_dll_directory(os.path.join(HFS, "bin"))
import OpenImageIO as oiio

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = tempfile.mkdtemp(prefix="nova_ramps_")
GOLDEN = os.path.join(ROOT, "tests", "data", "karma_ramps.json")
W = 200
BASES = ["constant", "linear", "catmull-rom", "bspline", "monotonecubic", "bezier"]
SETS = {
    "a": ([0.2, 0.5, 0.8], [0.0, 0.25, 1.0]),
    "b": ([0.1, 0.3, 0.45, 0.7, 0.9], [0.2, 1.0, 0.1, 0.6, 0.3]),
    "c": ([0.0, 0.19773299, 0.627204, 1.0], [0.0, 0.9, 0.1, 1.0]),
    "d": ([0.0, 0.1, 0.2, 0.4, 0.6, 0.8, 1.0], [0.0, 1.0, 0.0, 0.5, 1.0, 0.0, 1.0]),
}


def scene():
    rows = []
    mats = []
    k = 0
    for sname, (keys, vals) in SETS.items():
        for basis in BASES:
            y = -k / W
            name = f"r_{sname}_{basis.replace('-', '_')}"
            rows.append(f"""
    def Mesh "{name}" (prepend apiSchemas = ["MaterialBindingAPI"])
    {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, {y - 0.5 / W}, 0), (1, {y - 0.5 / W}, 0), (1, {y + 0.5 / W}, 0), (0, {y + 0.5 / W}, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = "vertex")
        uniform token subdivisionScheme = "none"
        rel material:binding = </Looks/{name}>
    }}""")
            basis_list = ", ".join(f'"{basis}"' for _ in keys)
            mats.append(f"""
    def Material "{name}"
    {{
        token outputs:kma:surface.connect = </Looks/{name}/s.outputs:out>
        def Shader "s"
        {{
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color.connect = </Looks/{name}/c.outputs:out>
            token outputs:out
        }}
        def Shader "c"
        {{
            uniform token info:id = "ND_convert_float_color3"
            float inputs:in.connect = </Looks/{name}/r.outputs:out>
            color3f outputs:out
        }}
        def Shader "r"
        {{
            uniform token info:id = "kma_rampconst_float"
            float inputs:t.connect = </Looks/{name}/st.outputs:out>
            float[] inputs:fkeys = [{", ".join(map(str, keys))}]
            float[] inputs:fvalues = [{", ".join(map(str, vals))}]
            token[] inputs:fbasis = [{basis_list}]
            int inputs:framp = {len(keys)}
            float outputs:out
        }}
        def Shader "st"
        {{
            uniform token info:id = "ND_extract_vector2"
            float2 inputs:in.connect = </Looks/{name}/tc.outputs:out>
            int inputs:index = 0
            float outputs:out
        }}
        def Shader "tc"
        {{
            uniform token info:id = "ND_texcoord_vector2"
            float2 outputs:out
        }}
    }}""")
            k += 1
    n = k
    return n, f"""#usda 1.0
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
    }}
    def Scope "Products"
    {{
        def RenderProduct "p"
        {{
            token productName = "ramp.exr"
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
    n, text = scene()
    path = os.path.join(OUT, "ramp_probe.usda")
    open(path, "w", newline="\n").write(text)
    prefs = tempfile.mkdtemp(prefix="nova_hprefs_")
    env = dict(os.environ)
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for v in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH", "HOME"):
        env.pop(v, None)
    env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    exr = os.path.join(OUT, "ramp_probe.exr")
    r = subprocess.run([os.path.join(HFS, "bin", "husk.exe"), "--renderer", "BRAY_HdKarma", "-s", "/Render/s",
                        "-p", "1", "-o", exr, path], env=env, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-2000:])
        return
    px = oiio.ImageBuf(exr).get_pixels(oiio.FLOAT)
    out = {}
    k = 0
    for sname in SETS:
        for basis in BASES:
            out[f"{sname}:{basis}"] = [round(float(px[k][x][0]), 6) for x in range(W)]
            k += 1
    golden = {"generator": "tests/karma/probe_ramps.py", "houdini": os.path.basename(HFS),
              "sets": {k: {"keys": v[0], "values": v[1]} for k, v in SETS.items()},
              "t": [(x + 0.5) / W for x in range(0, W, 4)],
              "karma": {k: row[::4] for k, row in out.items()}}
    with open(GOLDEN, "w", newline="\n") as f:
        json.dump(golden, f, indent=1)
        f.write("\n")
    print("wrote", GOLDEN)


main()
