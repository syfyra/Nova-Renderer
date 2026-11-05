"""husk check of MaterialX materials (docs/design/M3.md, M3.6).

One quad per case, seen from above by its own orthographic camera under a
white dome with direct light only (nova:maxDepth 1), the setup of the Karma
calibration in docs/research/m3/hydra-materials.md 2.5:
  * render context priority: kma > mtlx > universal (Karma: exactly red);
  * Standard Surface with base_color from the displayColor primvar and no
    specular is exactly Lambert (Karma: exactly C);
  * Standard Surface with nothing authored: 0.8089 (Karma), 1 %;
  * a kma-only material (the Karma Material Builder's context);
  * a Material interface input promoted into a shader input;
  * GeomSubsets: two materials on one mesh;
  * nodegraph expansion (smoothstep_color3 and remap from the library);
  * unbound geometry keeps Nova's default, Lambert(displayColor);
  * UsdPreviewSurface (M3.12, native): Karma's (0.5139, 0.2733, 0.1530) for
    diffuseColor C, 3 % (Karma's Schlick specular lacks the compensation);
  * a Principled Shader material with its auto-generated preview, as Houdini
    writes it (universal info:sourceAsset): Nova renders the vex network
    natively (docs/design/M5.md D3), as Karma CPU's VEX (measured);
  * VEX builder code (info:sourceCode) renders through the CVEX bridge from
    the usda alone, no hip file (D4);
  * VEX code that calls trace() falls back to its preview with one warning (D5).

Needs OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_materials.py ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402

C = (0.5, 0.25, 0.125)


def quad(name, x, binding=None, color=C, extra=""):
    bind = f'\n    rel material:binding = <{binding}>' if binding else ""
    api = '\n(\n    prepend apiSchemas = ["MaterialBindingAPI"]\n)' if binding else ""
    color_line = f"\n    color3f[] primvars:displayColor = [{color}] (interpolation = \"constant\")" if color else ""
    return f"""
def Mesh "{name}"{api}
{{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 3, 2, 1]
    point3f[] points = [({x - 10}, 0, -10), ({x + 10}, 0, -10), ({x + 10}, 0, 10), ({x - 10}, 0, 10)]
    uniform token subdivisionScheme = "none"{color_line}{bind}{extra}
}}
"""


def camera_and_settings(name, x):
    return f"""
def Camera "cam_{name}"
{{
    token projection = "orthographic"
    float horizontalAperture = 16
    float verticalAperture = 16
    float2 clippingRange = (0.1, 100)
    double3 xformOp:translate = ({x}, 5, 0)
    float xformOp:rotateX = -90
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
}}
def RenderSettings "settings_{name}"
{{
    rel camera = </cam_{name}>
    int2 resolution = (16, 16)
    int nova:samples = 128
    int nova:maxDepth = 1
    token nova:pixelFilter = "box"
}}
"""


CASES = [
    # name, x, binding, displayColor, extra mesh lines
    ("priority", 0, "/mat/both", C, ""),
    ("mtlxonly", 30, "/mat/mtlxonly", C, ""),
    ("lambert", 60, "/mat/lambert", C, ""),
    ("ssdefault", 90, "/mat/ssdefault", None, ""),
    ("kma", 120, "/mat/kma", None, ""),
    ("interface", 150, "/mat/interface", None, ""),
    ("graph", 180, "/mat/graph", None, ""),
    ("unbound", 210, None, C, ""),
    ("preview", 270, "/mat/preview", None, ""),
    ("vexpreview", 300, "/mat/vexpreview", None, ""),
    ("vexbuilder", 330, "/mat/vexbuilder", None, ""),
    ("vextrace", 360, "/mat/vextrace", None, ""),
]

MATERIALS = """
def Scope "mat"
{
    def Material "both"
    {
        token outputs:kma:surface.connect = </mat/both/red.outputs:out>
        token outputs:mtlx:surface.connect = </mat/both/green.outputs:out>
        token outputs:surface.connect = </mat/both/blue.outputs:surface>
        def Shader "red"
        {
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color = (1, 0, 0)
            token outputs:out
        }
        def Shader "green"
        {
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color = (0, 1, 0)
            token outputs:out
        }
        def Shader "blue"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:emissiveColor = (0, 0, 1)
            color3f inputs:diffuseColor = (0, 0, 0)
            token outputs:surface
        }
    }
    def Material "mtlxonly"
    {
        token outputs:mtlx:surface.connect = </mat/mtlxonly/green.outputs:out>
        token outputs:surface.connect = </mat/mtlxonly/blue.outputs:surface>
        def Shader "green"
        {
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color = (0, 1, 0)
            token outputs:out
        }
        def Shader "blue"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:emissiveColor = (0, 0, 1)
            token outputs:surface
        }
    }
    def Material "lambert"
    {
        token outputs:mtlx:surface.connect = </mat/lambert/surface.outputs:out>
        def Shader "surface"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 1
            color3f inputs:base_color.connect = </mat/lambert/primvar.outputs:out>
            float inputs:specular = 0
            token outputs:out
        }
        def Shader "primvar"
        {
            uniform token info:id = "ND_geompropvalue_color3"
            string inputs:geomprop = "displayColor"
            color3f inputs:default = (1, 0, 1)
            color3f outputs:out
        }
    }
    def Material "ssdefault"
    {
        token outputs:mtlx:surface.connect = </mat/ssdefault/surface.outputs:out>
        def Shader "surface"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            token outputs:out
        }
    }
    def Material "kma"
    {
        token outputs:kma:surface.connect = </mat/kma/surface.outputs:out>
        def Shader "surface"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.2, 0.4, 0.6)
            float inputs:specular = 0
            token outputs:out
        }
    }
    def Material "interface"
    {
        color3f inputs:tint = (0.3, 0.6, 0.9)
        token outputs:mtlx:surface.connect = </mat/interface/unlit.outputs:out>
        def Shader "unlit"
        {
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color.connect = </mat/interface.inputs:tint>
            token outputs:out
        }
    }
    def Material "graph"
    {
        token outputs:mtlx:surface.connect = </mat/graph/unlit.outputs:out>
        def Shader "unlit"
        {
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color.connect = </mat/graph/smooth.outputs:out>
            token outputs:out
        }
        def Shader "smooth"
        {
            uniform token info:id = "ND_smoothstep_color3"
            color3f inputs:in.connect = </mat/graph/remap.outputs:out>
            color3f inputs:low = (0, 0, 0)
            color3f inputs:high = (1, 1, 1)
            color3f outputs:out
        }
        def Shader "remap"
        {
            uniform token info:id = "ND_remap_color3FA"
            color3f inputs:in = (0.25, 0.5, 0.75)
            float inputs:inlow = 0
            float inputs:inhigh = 2
            float inputs:outlow = 0
            float inputs:outhigh = 1
            color3f outputs:out
        }
    }
    def Material "preview"
    {
        token outputs:surface.connect = </mat/preview/s.outputs:surface>
        def Shader "s"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.5, 0.25, 0.125)
            token outputs:surface
        }
    }
    def Material "vexpreview"
    {
        token outputs:vex:surface.connect = </mat/vexpreview/vex.outputs:surface>
        token outputs:surface.connect = </mat/vexpreview/preview.outputs:surface>
        def Shader "vex"
        {
            uniform token info:implementationSource = "sourceAsset"
            uniform asset info:sourceAsset = @opdef:/Vop/principledshader::2.0?SurfaceVexCode@
            color3f inputs:basecolor = (0.9, 0.1, 0.1)
            token outputs:surface
        }
        def Shader "preview"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.5, 0.25, 0.125)
            token outputs:surface
        }
    }
    def Material "vexbuilder"
    {
        token outputs:vex:surface.connect = </mat/vexbuilder/vex.outputs:surface>
        def Shader "vex"
        {
            uniform token info:implementationSource = "sourceCode"
            string info:sourceCode = "surface\\nbuilt(vector tint = {1, 1, 1})\\n{\\n    F = 2.0 * diffuse(normalize(N)) * tint;\\n}\\n"
            vector3f inputs:tint = (0.2, 0.4, 0.6)
            token outputs:surface
        }
    }
    def Material "vextrace"
    {
        token outputs:vex:surface.connect = </mat/vextrace/vex.outputs:surface>
        token outputs:surface.connect = </mat/vextrace/preview.outputs:surface>
        def Shader "vex"
        {
            uniform token info:implementationSource = "sourceCode"
            string info:sourceCode = "surface\\nmirror()\\n{\\n    vector c = trace(P, reflect(I, N), Time);\\n    F = diffuse(N) * c;\\n}\\n"
            token outputs:surface
        }
        def Shader "preview"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.5, 0.25, 0.125)
            token outputs:surface
        }
    }
    def Material "red"
    {
        token outputs:mtlx:surface.connect = </mat/red/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color = (1, 0, 0)
            token outputs:out
        }
    }
    def Material "green"
    {
        token outputs:mtlx:surface.connect = </mat/green/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color = (0, 1, 0)
            token outputs:out
        }
    }
}
"""

# Two faces side by side (x < x0: face 0, x > x0: face 1); face 1 is a subset
# bound to red, the mesh itself to green.
SUBSETS = """
def Mesh "subsets" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 3, 4, 1, 1, 4, 5, 2]
    point3f[] points = [(230, 0, -10), (240, 0, -10), (250, 0, -10), (230, 0, 10), (240, 0, 10), (250, 0, 10)]
    uniform token subdivisionScheme = "none"
    rel material:binding = </mat/green>
    uniform token subsetFamily:materialBind:familyType = "nonOverlapping"
    def GeomSubset "right" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform token elementType = "face"
        uniform token familyName = "materialBind"
        int[] indices = [1]
        rel material:binding = </mat/red>
    }
}
"""


def scene():
    parts = ['#usda 1.0\n(\n    upAxis = "Y"\n    metersPerUnit = 1\n)\n', MATERIALS, SUBSETS,
             'def DomeLight "dome"\n{\n    float inputs:intensity = 1\n}\n']
    for name, x, binding, color, extra in CASES:
        parts.append(quad(name, x, binding, color, extra))
        parts.append(camera_and_settings(name, x))
    parts.append(camera_and_settings("subsets", 240))
    return "".join(parts)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--scenes", required=False)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import OpenImageIO as oiio

    path = os.path.join(args.out, "materials.usda")
    with open(path, "w") as f:
        f.write(scene())

    env, prefs = isolated_env(args)
    try:
        def render(name):
            image = os.path.join(args.out, f"{name}.exr")
            proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "--settings", f"/settings_{name}", "-o", image,
                        path], env, args.out)
            check(proc.returncode == 0, f"husk {name} exits with 0")
            return oiio.ImageBuf(image).get_pixels(oiio.FLOAT), proc

        def mean(pixels):
            return [float(pixels[..., c].mean()) for c in range(3)]

        def expect(name, got, wanted, tol):
            worst = max(abs(g - w) for g, w in zip(got, wanted))
            check(worst <= tol, f"{name}: {[round(g, 5) for g in got]} vs {list(wanted)} (max deviation {worst:.2g})")

        expect("context priority: kma wins", mean(render("priority")[0]), (1, 0, 0), 1e-6)
        expect("without kma: mtlx wins over the universal network", mean(render("mtlxonly")[0]), (0, 1, 0), 1e-6)
        expect("Standard Surface, base_color = displayColor, specular 0, is Lambert",
               mean(render("lambert")[0]), C, 2e-3)
        ss = mean(render("ssdefault")[0])
        expect("Standard Surface with nothing authored (Karma 0.8089)", ss, (0.8089,) * 3, 0.0081)
        expect("a kma-only material renders", mean(render("kma")[0]), (0.2, 0.4, 0.6), 2e-3)
        expect("a promoted Material input", mean(render("interface")[0]), (0.3, 0.6, 0.9), 3e-4)  # half-float EXR
        x = [v / 2 for v in (0.25, 0.5, 0.75)]  # remap 0..2 -> 0..1, then smoothstep 0..1
        expect("library nodegraphs (smoothstep_color3) expand", mean(render("graph")[0]),
               [t * t * (3 - 2 * t) for t in x], 1e-6)
        expect("unbound geometry: Lambert(displayColor)", mean(render("unbound")[0]), C, 2e-3)
        karma_preview = (0.5139, 0.2733, 0.1530)
        got = mean(render("preview")[0])
        worst = max(abs(g - w) / w for g, w in zip(got, karma_preview))
        check(worst <= 0.03, f"UsdPreviewSurface (Karma {karma_preview}): {[round(g, 5) for g in got]} "
                             f"(max rel. deviation {worst:.3f})")
        karma_principled = (0.8113, 0.12534, 0.12534)  # Karma CPU's VEX, 256 spp
        got = mean(render("vexpreview")[0])
        worst = max(abs(g - w) / w for g, w in zip(got, karma_principled))
        check(worst <= 0.02, f"a Principled Shader material renders natively (Karma {karma_principled}): "
                             f"{[round(g, 5) for g in got]} (max rel. deviation {worst:.3f})")
        expect("VEX builder code renders through the CVEX bridge from the usda",
               mean(render("vexbuilder")[0]), (0.2, 0.4, 0.6), 5e-3)
        pixels, proc = render("vextrace")
        got = mean(pixels)
        worst = max(abs(g - w) / w for g, w in zip(got, karma_preview))
        check(worst <= 0.03, f"VEX code with trace() renders its preview: {[round(g, 5) for g in got]} "
                             f"(max rel. deviation {worst:.3f})")
        warnings = [line for line in (proc.stdout + proc.stderr).splitlines() if "trace()" in line]
        check(len(warnings) == 1, f"one warning names trace(): {warnings}")
        pixels, _ = render("subsets")
        expect("GeomSubset: the left face keeps the mesh's material", mean(pixels[:, :7]), (0, 1, 0), 1e-6)
        expect("GeomSubset: the right face has the subset's material", mean(pixels[:, 9:]), (1, 0, 0), 1e-6)
    finally:
        shutil.rmtree(prefs, ignore_errors=True)

    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
