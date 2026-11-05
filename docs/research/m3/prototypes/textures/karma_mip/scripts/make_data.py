"""Writes the test data for the Karma MIP-selection probe.

mips3.exr: 1024^2 RGB float, tiled 64, MIP-mapped; level L is the constant L.
One USD scene per resolution: an orthographic camera looking down at a unit
plane whose uv spans [0,1]^2, emission = the texture (mtlx standard_surface).
"""
import os
import sys

out = sys.argv[1]
hfs = sys.argv[2]
os.add_dll_directory(os.path.join(hfs, "bin"))
import OpenImageIO as oiio  # noqa: E402

os.makedirs(out, exist_ok=True)
levels = []
res = 1024
level = 0
while True:
    name = os.path.join(out, "lvl%d.exr" % level)
    buf = oiio.ImageBuf(oiio.ImageSpec(res, res, 3, oiio.FLOAT))
    oiio.ImageBufAlgo.fill(buf, (float(level), float(level), float(level)))
    buf.write(name)
    levels.append(name)
    if res == 1:
        break
    res //= 2
    level += 1
cfg = oiio.ImageSpec()
cfg.tile_width = cfg.tile_height = 64
cfg.attribute("maketx:mipimages", ";".join(levels[1:]))
cfg.attribute("maketx:constant_color_detect", 0)
cfg.attribute("maketx:fileformatname", "openexr")
ok = oiio.ImageBufAlgo.make_texture(oiio.MakeTxTexture, levels[0], os.path.join(out, "mips3.exr"), cfg)
print("make_texture", ok, oiio.geterror())

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
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = "vertex")
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
            asset inputs:file = @./mips3.exr@
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
    def Camera "persp"
    {
        float focalLength = 50
        float horizontalAperture = 20.955
        float verticalAperture = 20.955
        float2 clippingRange = (0.01, 1000)
        double3 xformOp:translate = (0, 0.25, 1.2)
        float xformOp:rotateX = -8
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
    }
}
def Scope "Render"
{
    def RenderSettings "settings"
    {
        rel camera = </World/cam>
        int2 resolution = (RES, RES)
    }
    def RenderSettings "grazing"
    {
        rel camera = </World/persp>
        int2 resolution = (256, 256)
    }
}
"""
for r in (16, 64, 256, 1024):
    with open(os.path.join(out, "plane_%d.usda" % r), "w") as f:
        f.write(SCENE.replace("RES", str(r)))
print("ok")
