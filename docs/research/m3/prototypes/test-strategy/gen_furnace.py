"""Karma albedo probe: one sphere (radius 1, explicit 160x80 UV mesh with
vertex normals) in a uniform dome (L = 1), orthographic camera. Every pixel
of the disk shows the directional albedo E(mu) with mu = sqrt(1 - r^2); the
disk mean equals the cosine-weighted hemispherical albedo E_avg."""
import math, os, sys, json

def sphere_asset(nu=160, nv=80):
    pts, idx = [], []
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu + 1):
            ph = 2 * math.pi * i / nu
            pts.append((math.sin(th) * math.cos(ph), math.cos(th), -math.sin(th) * math.sin(ph)))
    for j in range(nv):
        for i in range(nu):
            a = j * (nu + 1) + i
            idx += [a, a + nu + 1, a + nu + 2, a + 1]
    f3 = lambda p: "(%.6g, %.6g, %.6g)" % p
    st = [(i / nu, 1 - j / nv) for j in range(nv + 1) for i in range(nu + 1)]
    return ('#usda 1.0\n(\n    defaultPrim = "sphere"\n    metersPerUnit = 1\n    upAxis = "Y"\n)\n\n'
            'def Mesh "sphere"\n{\n'
            f"    int[] faceVertexCounts = [{', '.join(['4'] * (nu * nv))}]\n"
            f"    int[] faceVertexIndices = [{', '.join(map(str, idx))}]\n"
            f"    point3f[] points = [{', '.join(f3(p) for p in pts)}]\n"
            f"    normal3f[] normals = [{', '.join(f3(p) for p in pts)}] (\n        interpolation = \"vertex\"\n    )\n"
            f"    texCoord2f[] primvars:st = [{', '.join('(%.6g, %.6g)' % s for s in st)}] (\n        interpolation = \"vertex\"\n    )\n"
            '    uniform token subdivisionScheme = "none"\n    uniform bool doubleSided = 1\n}\n')

COMMON = """#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

class "_class_MtlRefSettings"
{
    # Nova (M3)
    float nova:clampIndirect = 0
    int nova:maxDepth = 64
    token nova:pixelFilter = "box"
    float nova:pixelFilterScale = 1

    # Karma CPU: same estimator expectation as an unbiased path tracer
    token karma:global:convergence_mode = "Path Traced"
    string karma:global:pixeloracle = '["uniform",{}]'
    string karma:global:pixelfilter = "box"
    float karma:global:pixelfilterscale = 1
    float karma:global:colorlimit = 1e9
    float karma:global:colorlimitindirect = 1e9
    bool karma:global:colorlimitindlinked = 1
    bool karma:global:constrainmaxrough = 0
    float karma:object:diffuselimit = LIMIT
    float karma:object:reflectlimit = LIMIT
    float karma:object:refractlimit = LIMIT
    float karma:object:ssslimit = LIMIT
    bool karma:object:causticsenable = CAUSTICS
    float karma:object:causticsroughnessclamp = 0
    bool karma:object:internalreflectenable = INTREFL
    bool karma:object:thinwallatrefractlimit = 0
    rel products = [</Render/Products/beauty>]
}

def Scope "Render"
{
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


def val(v):
    if isinstance(v, bool):
        return ("bool", "1" if v else "0")
    if isinstance(v, (int, float)):
        return ("float", "%g" % v)
    return ("color3f", "(%g, %g, %g)" % tuple(v))


def material(shader_id, inputs):
    lines = []
    for k, v in inputs.items():
        t, s = val(v)
        lines.append(f"            {t} inputs:{k} = {s}")
    body = "\n".join(lines)
    return f"""def Scope "Looks"
{{
    def Material "probe"
    {{
        token outputs:mtlx:surface.connect = </Looks/probe/surface.outputs:out>
        def Shader "surface"
        {{
            uniform token info:id = "{shader_id}"
{body}
            token outputs:out
        }}
    }}
}}
"""


def scene(shader_id, inputs, res=88):
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
        rel material:binding = </Looks/probe>
    }}
    def DomeLight "sky"
    {{
        float inputs:intensity = 1
        color3f inputs:color = (1, 1, 1)
    }}
    def Camera "cam"
    {{
        token projection = "orthographic"
        float horizontalAperture = 22
        float verticalAperture = 22
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
}}

{material(shader_id, inputs)}
def Scope "Render"
{{
    def RenderSettings "ref" (
        inherits = </_class_MtlRefSettings>
    )
    {{
        rel camera = </World/cam>
        int2 resolution = ({res}, {res})
    }}
}}
"""


SS = "ND_standard_surface_surfaceshader"
OP = "ND_open_pbr_surface_surfaceshader"
# Explicit values for everything that matters (v1.0.0 and v1.0.1 defaults differ).
SS_BASE = {"base": 1.0, "base_color": (1, 1, 1), "specular": 0.0, "metalness": 0.0, "coat": 0.0, "sheen": 0.0,
           "transmission": 0.0, "emission": 0.0, "subsurface": 0.0, "diffuse_roughness": 0.0}
OP_BASE = {"base_weight": 1.0, "base_color": (1, 1, 1), "specular_weight": 0.0, "base_metalness": 0.0,
           "coat_weight": 0.0, "fuzz_weight": 0.0, "transmission_weight": 0.0, "emission_luminance": 0.0,
           "subsurface_weight": 0.0, "base_diffuse_roughness": 0.0}


def ss(**kw):
    d = dict(SS_BASE)
    d.update(kw)
    return (SS, d)


def op(**kw):
    d = dict(OP_BASE)
    d.update(kw)
    return (OP, d)


CASES = {
    "ss_lambert": ss(),
    "ss_lambert_c": ss(base_color=(0.8, 0.4, 0.2)),
    "op_lambert": op(),
}
for r in (0.0, 0.2, 0.4, 0.6, 0.8, 1.0):
    t = f"{int(round(r * 10)):02d}"
    CASES[f"ss_metal_r{t}"] = ss(metalness=1.0, specular=1.0, specular_color=(1, 1, 1), specular_roughness=r)
    CASES[f"ss_speconly_r{t}"] = ss(base=0.0, specular=1.0, specular_IOR=1.5, specular_roughness=r)
    CASES[f"ss_plastic_r{t}"] = ss(specular=1.0, specular_IOR=1.5, specular_roughness=r)
    CASES[f"op_metal_r{t}"] = op(base_metalness=1.0, specular_weight=1.0, specular_color=(1, 1, 1),
                                 specular_roughness=r)
    CASES[f"op_plastic_r{t}"] = op(specular_weight=1.0, specular_ior=1.5, specular_roughness=r)
for r in (0.0, 0.3, 0.7):
    t = f"{int(round(r * 10)):02d}"
    CASES[f"ss_coat_r{t}"] = ss(coat=1.0, coat_roughness=r, coat_IOR=1.5, coat_color=(1, 1, 1))
    CASES[f"ss_glass_r{t}"] = ss(base=0.0, transmission=1.0, transmission_color=(1, 1, 1), specular=1.0,
                                 specular_IOR=1.5, specular_roughness=r)
    CASES[f"op_glass_r{t}"] = op(base_weight=0.0, transmission_weight=1.0, specular_weight=1.0, specular_ior=1.5,
                                 specular_roughness=r)
    CASES[f"op_coat_r{t}"] = op(coat_weight=1.0, coat_roughness=r, coat_ior=1.6, coat_darkening=1.0)
for r in (0.3, 0.7):
    t = f"{int(round(r * 10)):02d}"
    CASES[f"ss_sheen_r{t}"] = ss(sheen=1.0, sheen_color=(1, 1, 1), sheen_roughness=r)
    CASES[f"ss_sheenonly_r{t}"] = ss(base=0.0, sheen=1.0, sheen_color=(1, 1, 1), sheen_roughness=r)
    CASES[f"op_fuzz_r{t}"] = op(fuzz_weight=1.0, fuzz_color=(1, 1, 1), fuzz_roughness=r)
    CASES[f"op_fuzzonly_r{t}"] = op(base_weight=0.0, fuzz_weight=1.0, fuzz_color=(1, 1, 1), fuzz_roughness=r)
CASES.update({
    "ss_glass_ior1": ss(base=0.0, transmission=1.0, specular=1.0, specular_IOR=1.0, specular_roughness=0.0),
    "ss_glass_thin": ss(base=0.0, transmission=1.0, specular=1.0, specular_IOR=1.5, specular_roughness=0.0,
                        thin_walled=True),
    "ss_glass_thin_r05": ss(base=0.0, transmission=1.0, specular=1.0, specular_IOR=1.5, specular_roughness=0.5,
                            thin_walled=True),
    "op_glass_ior1": op(base_weight=0.0, transmission_weight=1.0, specular_weight=1.0, specular_ior=1.0,
                        specular_roughness=0.0),
    "ss_emission": ss(base=0.0, emission=1.0, emission_color=(0.5, 0.25, 0.125)),
    "op_emission": op(base_weight=0.0, emission_luminance=1.0, emission_color=(0.5, 0.25, 0.125)),
    "ss_metal_aniso": ss(metalness=1.0, specular=1.0, specular_roughness=0.5, specular_anisotropy=0.8),
    "ss_thinfilm": ss(base=0.0, specular=1.0, specular_IOR=1.5, specular_roughness=0.0,
                      thin_film_thickness=500.0, thin_film_IOR=1.33),
    "ss_metal_gold_r04": ss(metalness=1.0, base_color=(0.944, 0.776, 0.373), specular=1.0,
                            specular_color=(0.998, 0.981, 0.751), specular_roughness=0.4),
    "ss_translucent": ss(subsurface=1.0, subsurface_color=(1, 1, 1), thin_walled=True),
    "ss_opacity05": ss(opacity=(0.5, 0.5, 0.5)),
})


def write_all(out, limit=16, caustics=1, intrefl=1, only=None):
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "sphere.usda"), "w", newline="\n") as f:
        f.write(sphere_asset())
    with open(os.path.join(out, "common.usda"), "w", newline="\n") as f:
        f.write(COMMON.replace("LIMIT", str(limit)).replace("CAUSTICS", str(caustics))
                .replace("INTREFL", str(intrefl)))
    names = [n for n in CASES if not only or n in only]
    for name in names:
        sid, inputs = CASES[name]
        with open(os.path.join(out, name + ".usda"), "w", newline="\n") as f:
            f.write(scene(sid, inputs))
    return names


if __name__ == "__main__":
    out = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    caustics = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    intrefl = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    only = sys.argv[5].split(",") if len(sys.argv) > 5 else None
    print(len(write_all(out, limit, caustics, intrefl, only)), "cases")
