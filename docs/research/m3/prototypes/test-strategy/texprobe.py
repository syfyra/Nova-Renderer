"""Karma texture probe: an unlit (emission-only) quad with st = position,
seen by an orthographic camera, shows MaterialX image / ramp values
directly. Checks the v orientation, UDIM tile selection, closest filtering
and the default texcoord primvar (UV0 -> st)."""
import os, shutil, subprocess, sys, tempfile
HFS = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
os.add_dll_directory(os.path.join(HFS, "bin"))
import numpy as np  # noqa: E402
import OpenImageIO as oiio  # noqa: E402

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

for tile in (1001, 1002):
    px = np.zeros((2, 4, 3), np.float32)  # file order: row 0 = top
    for j in range(2):
        for i in range(4):
            px[j, i] = (0.1 + i / 4, 0.1 + j / 2, (tile - 1000) * 0.25)
    spec = oiio.ImageSpec(4, 2, 3, oiio.FLOAT)
    o = oiio.ImageOutput.create(os.path.join(OUT, f"tex.{tile}.exr"))
    o.open(os.path.join(OUT, f"tex.{tile}.exr"), spec)
    o.write_image(px)
    o.close()

SCENE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "quad" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (2, 0, 0), (2, 1, 0), (0, 1, 0)]
        texCoord2f[] primvars:st = [(0, 0), (2, 0), (2, 1), (0, 1)] (
            interpolation = "vertex"
        )
        uniform token subdivisionScheme = "none"
        rel material:binding = </Looks/MAT>
    }
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 20
        float verticalAperture = 10
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (1, 0.5, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}

def Scope "Looks"
{
    def Material "img"
    {
        token outputs:mtlx:surface.connect = </Looks/img/surface.outputs:out>
        def Shader "surface"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 0
            float inputs:specular = 0
            float inputs:emission = 1
            color3f inputs:emission_color.connect = </Looks/img/tex.outputs:out>
            token outputs:out
        }
        def Shader "tex"
        {
            uniform token info:id = "ND_image_color3"
            asset inputs:file = @./tex.<UDIM>.exr@
            string inputs:filtertype = "closest"
            color3f outputs:out
        }
    }
    def Material "ramp"
    {
        token outputs:mtlx:surface.connect = </Looks/ramp/surface.outputs:out>
        def Shader "surface"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 0
            float inputs:specular = 0
            float inputs:emission = 1
            color3f inputs:emission_color.connect = </Looks/ramp/r.outputs:out>
            token outputs:out
        }
        def Shader "r"
        {
            uniform token info:id = "ND_ramplr_color3"
            color3f inputs:valuel = (0, 0, 0)
            color3f inputs:valuer = (1, 1, 1)
            color3f outputs:out
        }
    }
}

def Scope "Render"
{
    def RenderSettings "ref"
    {
        rel camera = </World/cam>
        int2 resolution = (64, 32)
        token karma:global:convergence_mode = "Path Traced"
        string karma:global:pixeloracle = '["uniform",{}]'
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1
        rel products = [</Render/Products/beauty>]
    }
    def Scope "Products"
    {
        def RenderProduct "beauty"
        {
            token productName = "beauty.exr"
            rel orderedVars = [</Render/Vars/C>]
        }
    }
    def Scope "Vars"
    {
        def RenderVar "C"
        {
            uniform token dataType = "color4f"
            uniform string sourceName = "C.*[LO]"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "color4f"
            string driver:parameters:aov:name = "C"
        }
    }
}
"""

prefs = tempfile.mkdtemp()
env = dict(os.environ, HOUDINI_USER_PREF_DIR=os.path.join(prefs, "houdini__HVER__"), HOUDINI_PACKAGE_SKIPLIST="nova")
for v in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
    env.pop(v, None)
try:
    for mat in ("img", "ramp"):
        path = os.path.join(OUT, f"probe_{mat}.usda")
        open(path, "w", newline="\n").write(SCENE.replace("/Looks/MAT", f"/Looks/{mat}"))
        exr = os.path.join(OUT, f"probe_{mat}.exr")
        p = subprocess.run([os.path.join(HFS, "bin", "husk.exe"), "--renderer", "BRAY_HdKarma", "--headlight", "none",
                            "-s", "/Render/ref", "-o", exr, "-p", "16", path], env=env, capture_output=True, text=True)
        print(mat, "rc", p.returncode, p.stderr[-400:])
        img = oiio.ImageBuf(exr).get_pixels(oiio.FLOAT)
        if mat == "img":
            # pixel centres of texel (i, row) in tile 1001: x = 8 i + 4, top row y = 8, bottom row y = 24
            for name, (x, y) in {"u0 top": (4, 8), "u0 bottom": (4, 24), "u3 top": (28, 8),
                                 "tile1002 u0 bottom": (36, 24)}.items():
                print(f"  {name:20s} {np.round(img[y, x, :3], 4)}")
        else:
            row = img[16, :, 0]
            print("  ramp at x=0,16,31,32,48,63:", np.round(row[[0, 16, 31, 32, 48, 63]], 4),
                  " expected u_center mod?", [round((x + 0.5) / 32, 4) for x in (0, 16, 31, 32, 48, 63)])
finally:
    shutil.rmtree(prefs, ignore_errors=True)
