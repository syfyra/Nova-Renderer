"""Karma probe of MaterialX flake2d for the M4.3 flake op (docs/design/M4.md):
an unlit quad (st in [0, 1]) seen orthographically, point-sampled (box filter
at scale 1e-6, 1 spp), 32 x 32 pixels, rendered four times with the emission
set to (s, t, 0), (presence, rand, id / 2^24), and the flake normal at two
roughnesses. Writes tests/data/karma_flakes.json, the golden values of
nova_hydra_tests "Flakes match Karma". Needs a Karma license; not part of
ctest. Run with Houdini's Python:
    "$HFS/python313/python.exe" tests/karma/probe_flakes.py
"""
import json
import os
import subprocess
import tempfile

HFS = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
os.add_dll_directory(os.path.join(HFS, "bin"))
import OpenImageIO as oiio  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
GOLDEN = os.path.join(ROOT, "tests", "data", "karma_flakes.json")
RES = 32
SIZE, COVERAGE = 0.05, 0.5

FLAKE = f"""
        def Shader "f"
        {{
            uniform token info:id = "ND_flake2d"
            float inputs:size = {SIZE}
            float inputs:coverage = {COVERAGE}
            float inputs:roughness = ROUGHNESS
            int outputs:id
            float outputs:rand
            float outputs:presence
            vector3f outputs:flakenormal
        }}"""

EMISSION = {
    "st": """
        def Shader "tc"
        {
            uniform token info:id = "ND_texcoord_vector2"
            float2 outputs:out
        }
        def Shader "e"
        {
            uniform token info:id = "ND_convert_vector2_color3"
            float2 inputs:in.connect = </Looks/m/tc.outputs:out>
            color3f outputs:out
        }""",
    "ids": FLAKE.replace("ROUGHNESS", "0.1") + """
        def Shader "idf"
        {
            uniform token info:id = "ND_convert_integer_float"
            int inputs:in.connect = </Looks/m/f.outputs:id>
            float outputs:out
        }
        def Shader "idn"
        {
            uniform token info:id = "ND_divide_float"
            float inputs:in1.connect = </Looks/m/idf.outputs:out>
            float inputs:in2 = 16777216
            float outputs:out
        }
        def Shader "e"
        {
            uniform token info:id = "ND_combine3_color3"
            float inputs:in1.connect = </Looks/m/f.outputs:presence>
            float inputs:in2.connect = </Looks/m/f.outputs:rand>
            float inputs:in3.connect = </Looks/m/idn.outputs:out>
            color3f outputs:out
        }""",
}
for r in ("0.1", "0.5"):
    EMISSION["normal" + r] = FLAKE.replace("ROUGHNESS", r) + """
        def Shader "e"
        {
            uniform token info:id = "ND_convert_vector3_color3"
            vector3f inputs:in.connect = </Looks/m/f.outputs:flakenormal>
            color3f outputs:out
        }"""


def scene(emission):
    return f"""#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{{
    def Mesh "quad" (prepend apiSchemas = ["MaterialBindingAPI"])
    {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = "vertex")
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (interpolation = "vertex")
        uniform token subdivisionScheme = "none"
        rel material:binding = </Looks/m>
    }}
    def Camera "cam"
    {{
        token projection = "orthographic"
        float horizontalAperture = 10
        float verticalAperture = 10
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0.5, 0.5, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
}}
def Scope "Looks"
{{
    def Material "m"
    {{
        token outputs:mtlx:surface.connect = </Looks/m/s.outputs:out>
        def Shader "s"
        {{
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color.connect = </Looks/m/e.outputs:out>
            token outputs:out
        }}{emission}
    }}
}}
def Scope "Render"
{{
    def RenderSettings "s"
    {{
        rel camera = </World/cam>
        int2 resolution = ({RES}, {RES})
        rel products = [</Render/Products/p>]
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1e-06
        token karma:global:convergence_mode = "Path Traced"
    }}
    def Scope "Products"
    {{
        def RenderProduct "p"
        {{
            token productName = "flakes.exr"
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
    work = tempfile.mkdtemp(prefix="nova_flakes_")
    prefs = tempfile.mkdtemp(prefix="nova_hprefs_")
    env = dict(os.environ)
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for v in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH", "HOME"):
        env.pop(v, None)
    env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    golden = {"generator": "tests/karma/probe_flakes.py", "houdini": os.path.basename(HFS), "resolution": RES,
              "size": SIZE, "coverage": COVERAGE}
    for name, emission in EMISSION.items():
        layer = os.path.join(work, name + ".usda")
        with open(layer, "w", newline="\n") as f:
            f.write(scene(emission))
        exr = os.path.join(work, name + ".exr")
        r = subprocess.run([os.path.join(HFS, "bin", "husk.exe"), "--renderer", "BRAY_HdKarma", "-s", "/Render/s",
                            "-p", "1", "-o", exr, layer], env=env, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(r.stderr[-2000:])
        px = oiio.ImageBuf(exr).get_pixels(oiio.FLOAT)
        golden[name] = [[round(float(px[y][x][c]), 7) for c in range(3)] for y in range(RES) for x in range(RES)]
    with open(GOLDEN, "w", newline="\n") as f:
        json.dump(golden, f)
        f.write("\n")
    flakes = sum(1 for v in golden["ids"] if v[0] > 0)
    print("wrote", GOLDEN, f"({flakes} of {RES * RES} pixels on a flake)")


main()
