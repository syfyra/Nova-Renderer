"""Lit material stage for Karma references: floating spheres (no ground, so
no diffuse surface sees the specular ones: no caustic paths), a textured
dome (sky_soft.exr) and one rect key light. Plus a caustics probe: a glass
sphere over a diffuse plane, rendered with karma causticsenable 0 and 1."""
import os, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_furnace as G  # noqa: E402

OUT = sys.argv[1]
SKY = sys.argv[2]  # tests/reference/scenes/assets/sky_soft.exr (copied)
os.makedirs(OUT, exist_ok=True)
shutil.copy(SKY, os.path.join(OUT, "sky_soft.exr"))
with open(os.path.join(OUT, "sphere.usda"), "w", newline="\n") as f:
    f.write(G.sphere_asset())
with open(os.path.join(OUT, "common.usda"), "w", newline="\n") as f:
    f.write(G.COMMON.replace("LIMIT", "16").replace("CAUSTICS", "1").replace("INTREFL", "1")
            .replace('token nova:pixelFilter = "box"', 'token nova:pixelFilter = "gauss"')
            .replace('string karma:global:pixelfilter = "box"', 'string karma:global:pixelfilter = "gauss"'))

ss = G.ss
GRID = [
    # row 0: diffuse family
    [("lambert_red", ss(base_color=(0.8, 0.2, 0.15))),
     ("oren_nayar", ss(base_color=(0.8, 0.8, 0.8), diffuse_roughness=1.0)),
     ("plastic_r02", ss(base_color=(0.1, 0.3, 0.8), specular=1.0, specular_roughness=0.2)),
     ("plastic_r05", ss(base_color=(0.1, 0.3, 0.8), specular=1.0, specular_roughness=0.5)),
     ("sheen", ss(base_color=(0.2, 0.05, 0.3), sheen=1.0, sheen_color=(1, 0.9, 0.8), sheen_roughness=0.4)),
     ("coat", ss(base_color=(0.7, 0.1, 0.1), coat=1.0, coat_roughness=0.05, specular=1.0,
                 specular_roughness=0.5))],
    # row 1: metals
    [("metal_r00", ss(metalness=1.0, base_color=(0.95, 0.93, 0.88), specular=1.0, specular_roughness=0.0)),
     ("metal_r02", ss(metalness=1.0, base_color=(0.95, 0.64, 0.54), specular=1.0, specular_roughness=0.2)),
     ("metal_r05", ss(metalness=1.0, base_color=(0.944, 0.776, 0.373), specular=1.0, specular_roughness=0.5)),
     ("metal_aniso", ss(metalness=1.0, base_color=(0.9, 0.9, 0.9), specular=1.0, specular_roughness=0.4,
                        specular_anisotropy=0.8)),
     ("thinfilm", ss(metalness=1.0, base_color=(0.6, 0.6, 0.6), specular=1.0, specular_roughness=0.1,
                     thin_film_thickness=400.0, thin_film_IOR=1.5)),
     ("emission", ss(base=0.2, emission=1.0, emission_color=(1.0, 0.5, 0.1), specular=0.0))],
    # row 2: transmission
    [("glass_r00", ss(base=0.0, transmission=1.0, specular=1.0, specular_IOR=1.5, specular_roughness=0.0)),
     ("glass_r03", ss(base=0.0, transmission=1.0, specular=1.0, specular_IOR=1.5, specular_roughness=0.3)),
     ("glass_tint", ss(base=0.0, transmission=1.0, transmission_color=(0.4, 0.8, 0.5), specular=1.0,
                       specular_IOR=1.5, specular_roughness=0.0)),
     ("glass_thin", ss(base=0.0, transmission=1.0, specular=1.0, specular_IOR=1.5, specular_roughness=0.0,
                       thin_walled=True)),
     ("opacity", ss(base_color=(0.9, 0.7, 0.2), opacity=(0.4, 0.4, 0.4))),
     ("glass_ior1", ss(base=0.0, transmission=1.0, specular=1.0, specular_IOR=1.0, specular_roughness=0.0))],
]


def mat_block(name, sid, inputs):
    body = "\n".join(f"            {G.val(v)[0]} inputs:{k} = {G.val(v)[1]}" for k, v in inputs.items())
    return f"""    def Material "{name}"
    {{
        token outputs:mtlx:surface.connect = </Looks/{name}/surface.outputs:out>
        def Shader "surface"
        {{
            uniform token info:id = "{sid}"
{body}
            token outputs:out
        }}
    }}
"""


def stage():
    spheres, mats = [], []
    for j, row in enumerate(GRID):
        for i, (name, (sid, inputs)) in enumerate(row):
            x, y = (i - 2.5) * 1.25, (1 - j) * 1.25
            spheres.append(f"""    def Mesh "{name}" (
        prepend apiSchemas = ["MaterialBindingAPI"]
        prepend references = @./sphere.usda@
    )
    {{
        rel material:binding = </Looks/{name}>
        double3 xformOp:translate = ({x}, {y}, 0)
        double3 xformOp:scale = (0.5, 0.5, 0.5)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
    }}
""")
            mats.append(mat_block(name, sid, inputs))
    # Backdrop-free: the dome is not camera visible (black background).
    return f"""#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    subLayers = [@./common.usda@]
)

def Xform "World"
{{
{''.join(spheres)}
    def DomeLight "sky"
    {{
        float inputs:intensity = 0.35
        color3f inputs:color = (0.85, 0.9, 1)
    }}
    def RectLight "rim"
    {{
        float inputs:intensity = 4
        color3f inputs:color = (1, 0.75, 0.5)
        float inputs:width = 3
        float inputs:height = 0.8
        bool inputs:normalize = 0
        double3 xformOp:translate = (4, 1.5, -3)
        float3 xformOp:rotateXYZ = (-15, 125, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }}
    def RectLight "key"
    {{
        float inputs:intensity = 6
        float inputs:width = 2
        float inputs:height = 1.2
        bool inputs:normalize = 0
        double3 xformOp:translate = (-3, 3, 4)
        float3 xformOp:rotateXYZ = (-32, -36, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }}
    def Camera "cam"
    {{
        float focalLength = 50
        float horizontalAperture = 36
        float verticalAperture = 18
        float fStop = 0
        float2 clippingRange = (0.1, 1000)
        double3 xformOp:translate = (0, 0, 12)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
}}

def Scope "Looks"
{{
{''.join(mats)}}}

def Scope "Render"
{{
    def RenderSettings "ref" (
        inherits = </_class_MtlRefSettings>
    )
    {{
        rel camera = </World/cam>
        int2 resolution = (320, 160)
    }}
}}
"""


def caustics(enable):
    glass = mat_block("glass", G.SS, dict(G.SS_BASE, base=0.0, transmission=1.0, specular=1.0, specular_IOR=1.5,
                                          specular_roughness=0.0))
    ground = mat_block("ground", G.SS, dict(G.SS_BASE, base_color=(0.7, 0.7, 0.7)))
    return f"""#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    subLayers = [@./common.usda@]
)

def Xform "World"
{{
    def Mesh "ball" (
        prepend apiSchemas = ["MaterialBindingAPI"]
        prepend references = @./sphere.usda@
    )
    {{
        rel material:binding = </Looks/glass>
        double3 xformOp:translate = (0, 1, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
    def Mesh "ground" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-20, 0, 20), (20, 0, 20), (20, 0, -20), (-20, 0, -20)]
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        rel material:binding = </Looks/ground>
    }}
    def RectLight "key"
    {{
        float inputs:intensity = 20
        float inputs:width = 1
        float inputs:height = 1
        bool inputs:normalize = 0
        double3 xformOp:translate = (0, 5, 0)
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }}
    def Camera "cam"
    {{
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 24
        float2 clippingRange = (0.1, 1000)
        double3 xformOp:translate = (0, 3.5, 6)
        float3 xformOp:rotateXYZ = (-28, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }}
}}

def Scope "Looks"
{{
{glass}{ground}}}

def Scope "Render"
{{
    def RenderSettings "ref" (
        inherits = </_class_MtlRefSettings>
    )
    {{
        rel camera = </World/cam>
        int2 resolution = (180, 120)
        bool karma:object:causticsenable = {enable}
    }}
}}
"""


with open(os.path.join(OUT, "mtl_stage.usda"), "w", newline="\n") as f:
    f.write(stage())
for e in (0, 1):
    with open(os.path.join(OUT, f"caustics_{e}.usda"), "w", newline="\n") as f:
        f.write(caustics(e))
print("ok")
