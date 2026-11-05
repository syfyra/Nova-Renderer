# Writes a Karma probe scene: head-on quads (mu = 1) under a uniform white
# dome; each pixel then equals the directional albedo E(mu=1) of the material.
import sys

out = sys.argv[1]

SS = "ND_standard_surface_surfaceshader"
OP = "ND_open_pbr_surface_surfaceshader"
# (name, nodedef, {input: (type, value)})
mats = [
    ("ss_spec", SS, {"base": ("float", 0), "specular": ("float", 1), "specular_roughness": ("float", 0.3),
                     "specular_IOR": ("float", 1.5)}),
    ("ss_spec_tf", SS, {"base": ("float", 0), "specular": ("float", 1), "specular_roughness": ("float", 0.3),
                        "specular_IOR": ("float", 1.5), "thin_film_thickness": ("float", 3000),
                        "thin_film_IOR": ("float", 1.33)}),
    ("ss_sheen", SS, {"base": ("float", 0), "specular": ("float", 0), "sheen": ("float", 1),
                      "sheen_color": ("color3f", (1, 1, 1)), "sheen_roughness": ("float", 0.3)}),
    ("ss_metal_rough", SS, {"base": ("float", 1), "base_color": ("color3f", (1, 1, 1)), "metalness": ("float", 1),
                            "specular_color": ("color3f", (1, 1, 1)), "specular_roughness": ("float", 0.8)}),
    ("ss_diffuse_on1", SS, {"base": ("float", 1), "base_color": ("color3f", (1, 1, 1)), "specular": ("float", 0),
                            "diffuse_roughness": ("float", 1.0)}),
    ("op_diffuse_eon1", OP, {"base_weight": ("float", 1), "base_color": ("color3f", (1, 1, 1)),
                             "specular_weight": ("float", 0), "base_diffuse_roughness": ("float", 1.0)}),
    ("op_fuzz", OP, {"base_weight": ("float", 0), "specular_weight": ("float", 0), "fuzz_weight": ("float", 1),
                     "fuzz_color": ("color3f", (1, 1, 1)), "fuzz_roughness": ("float", 0.5)}),
    ("ss_sheen_r08", SS, {"base": ("float", 0), "specular": ("float", 0), "sheen": ("float", 1),
                          "sheen_color": ("color3f", (1, 1, 1)), "sheen_roughness": ("float", 0.8)}),
    ("op_metal_rough", OP, {"base_weight": ("float", 1), "base_color": ("color3f", (1, 1, 1)),
                            "base_metalness": ("float", 1), "specular_roughness": ("float", 0.8)}),
    ("op_spec", OP, {"base_weight": ("float", 0), "specular_roughness": ("float", 0.3)}),
]

def fmt(t, v):
    if t == "color3f":
        return "(%g, %g, %g)" % v
    return "%g" % v

L = ['#usda 1.0', '(', '    defaultPrim = "World"', '    metersPerUnit = 1', '    upAxis = "Y"', ')', '',
     'def Xform "World"', '{']
n = len(mats)
for i, (name, nd, inputs) in enumerate(mats):
    x0 = i * 1.25
    L += [f'    def Mesh "q_{name}" (', '        prepend apiSchemas = ["MaterialBindingAPI"]', '    )', '    {',
          '        int[] faceVertexCounts = [4]', '        int[] faceVertexIndices = [0, 1, 2, 3]',
          f'        point3f[] points = [({x0}, 0, 0), ({x0 + 1}, 0, 0), ({x0 + 1}, 1, 0), ({x0}, 1, 0)]',
          '        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (interpolation = "vertex")',
          '        uniform token subdivisionScheme = "none"', '        uniform bool doubleSided = 1',
          f'        rel material:binding = </Looks/{name}>', '    }']
L += ['    def DomeLight "sky"', '    {', '        float inputs:intensity = 1', '        uniform token[] xformOpOrder = []', '    }',
      '    def Camera "cam"', '    {', '        token projection = "orthographic"',
      f'        float horizontalAperture = {n * 1.25 * 10}', '        float verticalAperture = 12.5',
      '        float2 clippingRange = (0.01, 10000)',
      f'        double3 xformOp:translate = ({n * 1.25 / 2 - 0.125}, 0.5, 10)',
      '        uniform token[] xformOpOrder = ["xformOp:translate"]', '    }', '}', '']
L += ['def Scope "Looks"', '{']
for name, nd, inputs in mats:
    L += [f'    def Material "{name}"', '    {', f'        token outputs:mtlx:surface.connect = </Looks/{name}/surf.outputs:out>',
          '        def Shader "surf"', '        {', f'            uniform token info:id = "{nd}"']
    for k, (t, v) in inputs.items():
        L.append(f'            {t} inputs:{k} = {fmt(t, v)}')
    L += ['            token outputs:out', '        }', '    }']
L += ['}', '']
L += ['def Scope "Render"', '{', '    def RenderSettings "rs"', '    {', '        rel camera = </World/cam>',
      f'        int2 resolution = ({n * 40}, 40)', '        rel products = [</Render/Products/beauty>]',
      '        token karma:global:convergence_mode = "Path Traced"',
      '        string karma:global:pixeloracle = \'["uniform",{}]\'',
      '        int karma:global:pathtracedsamples = 256',
      '        float karma:global:colorlimit = 1e9', '        float karma:global:colorlimitindirect = 1e9',
      '        bool karma:global:constrainmaxrough = 0',
      '    }', '    def Scope "Products"', '    {', '        def RenderProduct "beauty"', '        {',
      '            token productName = "probe.exr"', '            rel orderedVars = [</Render/Vars/C>]', '        }', '    }',
      '    def Scope "Vars"', '    {', '        def RenderVar "C"', '        {', '            uniform token dataType = "color4f"',
      '            uniform string sourceName = "C.*[LO]"', '            uniform token sourceType = "lpe"',
      '            token driver:parameters:aov:format = "color4f"', '            string driver:parameters:aov:name = "C"',
      '        }', '    }', '}', '']
open(out, "w").write("\n".join(L))
print("\n".join(m[0] for m in mats))
