"""UDIM probe for Karma: tiles 1001 red, 1002 green, 1011 white-top/blue-bottom,
1012 missing. The plane's u runs from -1 to 2 and v from 0 to 2."""
import os
import sys

out = sys.argv[1]
hfs = sys.argv[2]
os.add_dll_directory(os.path.join(hfs, "bin"))
import OpenImageIO as oiio  # noqa: E402

os.makedirs(out, exist_ok=True)


def tile(name, top, bottom):
    buf = oiio.ImageBuf(oiio.ImageSpec(64, 64, 3, oiio.HALF))
    oiio.ImageBufAlgo.fill(buf, top, oiio.ROI(0, 64, 0, 32, 0, 1, 0, 3))
    oiio.ImageBufAlgo.fill(buf, bottom, oiio.ROI(0, 64, 32, 64, 0, 1, 0, 3))
    buf.write(os.path.join(out, name))


tile("udim.1001.exr", (1, 0, 0), (1, 0, 0))
tile("udim.1002.exr", (0, 1, 0), (0, 1, 0))
tile("udim.1011.exr", (1, 1, 1), (0, 0, 1))

SCENE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{
    def Mesh "plane" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, 0, 1), (1, 0, 1), (1, 0, -1), (-1, 0, -1)]
        texCoord2f[] primvars:st = [(-1, 0), (2, 0), (2, 2), (-1, 2)] (interpolation = "vertex")
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/mat>
    }
    def Material "mat"
    {
        token outputs:mtlx:surface.connect = </World/mat/surf.outputs:out>
        def Shader "surf"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 0
            float inputs:specular = 0
            float inputs:emission = 1
            color3f inputs:emission_color.connect = </World/mat/img.outputs:out>
            token outputs:out
        }
        def Shader "img"
        {
            uniform token info:id = "ND_image_color3"
            asset inputs:file = @./udim.<UDIM>.exr@
            color3f inputs:default = (0.25, 0.25, 0.25)
            color3f outputs:out
        }
    }
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 20
        float verticalAperture = 20
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 5, 0)
        float xformOp:rotateX = -90
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
    }
}
def Scope "Render"
{
    def RenderSettings "settings"
    {
        rel camera = </World/cam>
        int2 resolution = (96, 96)
    }
}
"""
with open(os.path.join(out, "udim.usda"), "w") as f:
    f.write(SCENE)
print("ok")
