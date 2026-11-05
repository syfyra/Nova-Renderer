"""Generate direct-lighting albedo probe scenes: unit sphere in a white dome,
orthographic camera, Karma direct lighting only. Pixel value = E(mu_o)."""
import math, os, sys, json

OUT = os.path.dirname(os.path.abspath(__file__))

def sphere_mesh(nu=160, nv=80):
    pts, nrm, counts, idx = [], [], [], []
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu + 1):
            ph = 2 * math.pi * i / nu
            p = (math.sin(th) * math.cos(ph), math.cos(th), -math.sin(th) * math.sin(ph))
            pts.append(p); nrm.append(p)
    for j in range(nv):
        for i in range(nu):
            a = j * (nu + 1) + i
            b = a + 1
            c = a + (nu + 1) + 1
            d = a + (nu + 1)
            counts.append(4); idx += [a, d, c, b]
    f = lambda v: "(%.6f, %.6f, %.6f)" % v
    return counts, idx, "[" + ", ".join(map(f, pts)) + "]", "[" + ", ".join(map(f, nrm)) + "]"

def fmt(v):
    if isinstance(v, bool): return "1" if v else "0"
    if isinstance(v, (int, float)): return repr(float(v))
    return "(" + ", ".join(repr(float(x)) for x in v) + ")"

def typ(name, v, shader):
    if isinstance(v, bool): return "bool"
    if isinstance(v, (int, float)): return "float"
    if "normal" in name or "tangent" in name: return "vector3f"
    return "color3f"

def scene(name, shader_id, inputs, spp=256, res=96, limit=0):
    counts, idx, pts, nrm = sphere_mesh()
    ins = "\n".join("            %s inputs:%s = %s" % (typ(k, v, shader_id), k, fmt(v)) for k, v in inputs.items())
    s = f'''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{{
    def Mesh "ball" (prepend apiSchemas = ["MaterialBindingAPI"])
    {{
        int[] faceVertexCounts = {json.dumps(counts)}
        int[] faceVertexIndices = {json.dumps(idx)}
        point3f[] points = {pts}
        normal3f[] normals = {nrm} (interpolation = "vertex")
        uniform token subdivisionScheme = "none"
        uniform bool doubleSided = 1
        rel material:binding = </Looks/m>
    }}
    def DomeLight "dome"
    {{
        float inputs:intensity = 1
        bool inputs:karma:light:renderlightgeo = 1
    }}
    def Camera "cam"
    {{
        token projection = "orthographic"
        float horizontalAperture = 20
        float verticalAperture = 20
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0, 5)
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
            uniform token info:id = "{shader_id}"
{ins}
            token outputs:out
        }}
    }}
}}
def Scope "Render"
{{
    def RenderSettings "rs"
    {{
        rel camera = </World/cam>
        int2 resolution = ({res}, {res})
        rel products = [</Render/Products/beauty>]
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = {spp}
        string karma:global:pixeloracle = '["uniform",{{}}]'
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
        bool karma:global:colorlimitindlinked = 1
        bool karma:global:constrainmaxrough = 0
        float karma:object:diffuselimit = {limit}
        float karma:object:reflectlimit = {limit}
        float karma:object:refractlimit = {limit}
        float karma:object:volumelimit = 0
        float karma:object:sslimit = {limit}
    }}
    def Scope "Products"
    {{
        def RenderProduct "beauty"
        {{
            token productName = "{name}.exr"
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
'''
    with open(os.path.join(OUT, name + ".usda"), "w") as f:
        f.write(s)

SS = "ND_standard_surface_surfaceshader"
OP = "ND_open_pbr_surface_surfaceshader"
W = (1.0, 1.0, 1.0)
CASES = {
  # Standard Surface (1.0.1 defaults: base=1, base_color=0.8, spec_rough=0.2)
  "ss_lambert":      (SS, dict(base=1, base_color=W, specular=0)),
  "ss_diffspec":     (SS, dict(base=1, base_color=W, specular=1, specular_roughness=0.2)),
  "ss_diffspec_r6":  (SS, dict(base=1, base_color=W, specular=1, specular_roughness=0.6)),
  "ss_diffspec_c05": (SS, dict(base=1, base_color=(0.5, 0.5, 0.5), specular=1, specular_roughness=0.3)),
  "ss_metal_r2":     (SS, dict(base=1, base_color=W, metalness=1, specular_roughness=0.2)),
  "ss_metal_r7":     (SS, dict(base=1, base_color=W, metalness=1, specular_roughness=0.7)),
  "ss_coat_diff":    (SS, dict(base=1, base_color=W, specular=0, coat=1, coat_roughness=0.1)),
  "ss_sheen_diff":   (SS, dict(base=1, base_color=W, specular=0, sheen=1, sheen_color=W, sheen_roughness=0.3)),
  "ss_specred":      (SS, dict(base=1, base_color=W, specular=1, specular_color=(1.0, 0.0, 0.0), specular_roughness=0.3)),
  "ss_ondiff_r1":    (SS, dict(base=1, base_color=W, specular=0, diffuse_roughness=1.0)),
  # OpenPBR (1.1.1 defaults)
  "op_default_w":    (OP, dict(base_color=W)),
  "op_diff_only":    (OP, dict(base_color=W, specular_weight=0)),
  "op_eon_r1":       (OP, dict(base_color=W, specular_weight=0, base_diffuse_roughness=1.0)),
  "op_metal_r3":     (OP, dict(base_color=W, base_metalness=1, specular_roughness=0.3)),
  "op_metal_r7":     (OP, dict(base_color=W, base_metalness=1, specular_roughness=0.7)),
  "op_coat_w":       (OP, dict(base_color=W, coat_weight=1, coat_roughness=0.1)),
  "op_coat_c05":     (OP, dict(base_color=(0.5, 0.5, 0.5), specular_weight=0, coat_weight=1, coat_roughness=0.0)),
  "op_fuzz_w":       (OP, dict(base_color=W, fuzz_weight=1, fuzz_color=W, fuzz_roughness=0.5)),
}
if __name__ == "__main__":
    which = sys.argv[1:] or list(CASES)
    for n in which:
        sid, ins = CASES[n]
        scene(n, sid, ins)
    print("\n".join(which))
