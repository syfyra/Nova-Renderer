# Writes variant layers over the flattened stage that crypto_export.py wrote
# (crypto_render.usda), for husk runs:
#   crypto_var_multi.usda   multiSampled = 1 on every crypto var, CryptoMaterials rank 3,
#                           CryptoPrimvar_asset_name with a sidecar manifest,
#                           per-instance asset_name on the point instancer,
#                           instanceable references and a "cloud" instancer of 400 tiny
#                           spheres with unique per-instance asset_name values (overlap test)
#   crypto_var_box.usda     as multi, beauty pixel filter "box" (does the crypto follow it?)
#   crypto_var_single.usda  as multi but multiSampled = 0 (the LOP default) - same geometry
#   crypto_var_rank1.usda   as multi, every crypto rank 1
#   crypto_var_faces.usda   multi + int primvar crypto, uniform string primvar, GeomSubset materials
#   crypto_var_opacity.usda multi + a 50 % transparent card in front of ballB
# Usage: python crypto_variants.py <dir containing crypto_render.usda>
import os
import random
import sys

out = os.path.abspath(sys.argv[1])
base = "crypto_render.usda"
VARS = ["CryptoPrimitives", "CryptoMaterials", "CryptoKind_component", "CryptoPrimvar_asset_name"]

random.seed(7)
cloud_pos = []
for i in range(400):
    cloud_pos.append((3.0 + random.uniform(0, 0.6), 2.2 + random.uniform(0, 0.6), random.uniform(-0.3, 0.3)))


def layer(name, multi=True, rank=None, pixelfilter=None):
    lines = ["#usda 1.0", "(", "    subLayers = [@./%s@]" % base, "    startTimeCode = 1", "    endTimeCode = 1", ")", ""]
    lines += ['over "Render"', "{"]
    if pixelfilter:
        lines += ['    over "karmarendersettings1"', "    {",
                  '        string karma:global:pixelfilter = "%s"' % pixelfilter, "    }"]
    lines += ['    over "Products"', "    {", '        over "Vars"', "        {"]
    for v in VARS:
        lines += ['            over "%s"' % v, "            {",
                  "                bool driver:parameters:aov:husk:multiSampled = %d" % (1 if multi else 0)]
        r = rank if rank is not None else (3 if v == "CryptoMaterials" else None)
        if r is not None:
            lines.append("                int driver:parameters:aov:karma:cryptomatterank = %d" % r)
        if v == "CryptoPrimvar_asset_name":
            lines.append('                string driver:parameters:aov:karma:cryptomattesidecar = "crypto_primvar_manifest.json"')
        lines += ["            }"]
    lines += ["        }", "    }", "}", ""]
    # Scene additions.
    lines += ['class "ProtoAsset"', "{",
              '    def Sphere "ball" (prepend apiSchemas = ["MaterialBindingAPI"])', "    {",
              "        double radius = 0.5", "        rel material:binding = </World/mtl/blue>", "    }", "}", ""]
    lines += ['over "World"', "{",
              '    over "inst"', "    {",
              '        string[] primvars:asset_name = ["inst0", "inst1", "inst2", "inst3"] (interpolation = "vertex")',
              "    }",
              '    def Xform "refA" (', "        instanceable = true", "        references = </ProtoAsset>", "    )", "    {",
              "        double3 xformOp:translate = (-1.5, 2.3, 0)", '        uniform token[] xformOpOrder = ["xformOp:translate"]', "    }",
              '    def Xform "refB" (', "        instanceable = true", "        references = </ProtoAsset>", "    )", "    {",
              "        double3 xformOp:translate = (0, 2.3, 0)", '        uniform token[] xformOpOrder = ["xformOp:translate"]', "    }",
              '    def PointInstancer "cloud"', "    {",
              "        point3f[] positions = [%s]" % ", ".join("(%.4f, %.4f, %.4f)" % p for p in cloud_pos),
              "        int[] protoIndices = [%s]" % ", ".join("0" for _ in cloud_pos),
              '        string[] primvars:asset_name = [%s] (interpolation = "vertex")' %
              ", ".join('"c%d"' % i for i in range(len(cloud_pos))),
              "        rel prototypes = [</World/cloud/protos/dot>]",
              '        def Scope "protos"', "        {",
              '            def Sphere "dot" (prepend apiSchemas = ["MaterialBindingAPI"])', "            {",
              "                double radius = 0.035", "                rel material:binding = </World/mtl/red>", "            }",
              "        }", "    }", "}", ""]
    path = os.path.join(out, name)
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote", path)


FACES = """#usda 1.0
(
    subLayers = [@./crypto_var_multi.usda@]
    startTimeCode = 1
    endTimeCode = 1
)

# Integer primvar cryptomatte, uniform (per-face) string primvar and per-face
# materials through GeomSubsets.
over "Render"
{
    over "karmarendersettings1"
    {
        over "Products"
        {
            over "renderproduct"
            {
                append rel orderedVars = </Render/Products/Vars/CryptoPrimvar_piece>
            }
        }
    }
    over "Products"
    {
        over "Vars"
        {
            def RenderVar "CryptoPrimvar_piece" (
                prepend apiSchemas = ["KarmaRenderVarAPI", "HuskRenderVarAPI"]
            )
            {
                token dataType = "float3"
                float driver:parameters:aov:husk:clearValue = 0
                token driver:parameters:aov:husk:format = "float3"
                bool driver:parameters:aov:husk:multiSampled = 1
                string driver:parameters:aov:husk:name = "CryptoPrimvar_piece"
                bool driver:parameters:aov:karma:cryptomatte = 1
                int driver:parameters:aov:karma:cryptomatterank = 6
                string driver:parameters:aov:karma:cryptomattesidecar = ""
                string driver:parameters:aov:karma:filter = '["ubox",{}]'
                string sourceName = "piece"
                token sourceType = "primvar"
            }
        }
    }
}

over "World"
{
    over "geo"
    {
        over "ballA"
        {
            int primvars:piece = 7 (interpolation = "constant")
        }
        over "ballB"
        {
            int primvars:piece = -3 (interpolation = "constant")
        }
    }
    def Mesh "facemesh" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4, 4]
        int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
        point3f[] points = [(-4.1, 1.7, 0), (-3.5, 1.7, 0), (-2.9, 1.7, 0), (-4.1, 2.9, 0), (-3.5, 2.9, 0), (-2.9, 2.9, 0)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/mtl/red>
        string[] primvars:asset_name = ["faceA", "faceB"] (interpolation = "uniform")
        int primvars:piece = 42 (interpolation = "constant")
        uniform token subsetFamily:materialBind:familyType = "partition"
        def GeomSubset "right" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [1]
            rel material:binding = </World/mtl/blue>
        }
    }
    def Sphere "bare"
    {
        # no material binding at all
        double radius = 0.4
        double3 xformOp:translate = (3.5, -1.0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
"""

OPACITY ="""#usda 1.0
(
    subLayers = [@./crypto_var_multi.usda@]
    startTimeCode = 1
    endTimeCode = 1
)

# A 50 % transparent card (Standard Surface opacity 0.5) in front of ballB:
# how does coverage split between the card and what is behind it?
over "World"
{
    over "mtl"
    {
        def Material "half"
        {
            token outputs:mtlx:surface.connect = </World/mtl/half/surf.outputs:out>
            def Shader "surf"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color = (0.1, 0.8, 0.1)
                color3f inputs:opacity = (0.5, 0.5, 0.5)
                token outputs:out
            }
        }
    }
    def Mesh "card" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-0.2, -0.6, 1.5), (1.4, -0.6, 1.5), (1.4, 0.6, 1.5), (-0.2, 0.6, 1.5)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/mtl/half>
        string primvars:asset_name = "card" (interpolation = "constant")
    }
}
"""

layer("crypto_var_multi.usda")
layer("crypto_var_box.usda", pixelfilter="box")
layer("crypto_var_single.usda", multi=False)
layer("crypto_var_rank1.usda", rank=1)
for fname, text in (("crypto_var_faces.usda", FACES), ("crypto_var_opacity.usda", OPACITY)):
    with open(os.path.join(out, fname), "w", newline="\n") as f:
        f.write(text)
    print("wrote", os.path.join(out, fname))
