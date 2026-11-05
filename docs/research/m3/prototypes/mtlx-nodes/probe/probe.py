"""Karma probe of MaterialX stdlib node semantics: one unlit quad per test,
orthographic camera, emission_color = pattern. Compares Karma's pixels with
MaterialX-reference expectations (and Karma's VEX templates where they differ)."""
import math, os, re, subprocess, sys, tempfile, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mxnoise as mx

HFS = r"C:/Program Files/Side Effects Software/Houdini 22.0.466"
OUT = os.path.dirname(os.path.abspath(__file__)).replace("\\", "/")
COLS, ROWS, PITCH, PPU = 5, 6, 1.25, 128
W, H = int(COLS * PITCH * PPU), int(ROWS * PITCH * PPU)
ZOFF = 0.37

def srgb_to_lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

def clamp01(x):
    return min(1.0, max(0.0, x))

# --- test definitions -------------------------------------------------------
# node: (name, info:id, output type, [input lines]); input line helpers:
def v(t, n, val): return f"{t} inputs:{n} = {val}"
def c(t, n, node, out="out"): return f"{t} inputs:{n}.connect = <{{M}}/{node}.outputs:{out}>"

TC = ("tc", "ND_texcoord_vector2", "float2", [])
def scaled_tc(name, s, off=0.0):
    nodes = [TC, (name + "m", "ND_multiply_vector2FA", "float2", [c("float2", "in1", "tc"), v("float", "in2", s)])]
    if off:
        nodes.append((name, "ND_subtract_vector2FA", "float2", [c("float2", "in1", name + "m"), v("float", "in2", off)]))
    else:
        nodes[-1] = (name, nodes[-1][1], nodes[-1][2], nodes[-1][3])
    return nodes

TEX2 = OUT + "/tex2x2.png"
GRAY = OUT + "/gray128.png"

tests = []
def T(name, nodes, out, mx_expect, karma_expect=None, extra_file_meta=None):
    tests.append(dict(name=name, nodes=nodes, out=out, mx=mx_expect, karma=karma_expect))

T("ramplr_uv3m1", scaled_tc("tc3", 3, 1) + [("p", "ND_ramplr_color3", "color3f", [v("color3f", "valuel", "(0,0,0)"), v("color3f", "valuer", "(1,1,1)"), c("float2", "texcoord", "tc3")])], "p",
  lambda u, vv, P, Q: [clamp01(3 * u - 1)] * 3, lambda u, vv, P, Q: [3 * u - 1] * 3)
T("ramptb", [("p", "ND_ramptb_color3", "color3f", [v("color3f", "valuet", "(1,1,1)"), v("color3f", "valueb", "(0,0,0)")])], "p",
  lambda u, vv, P, Q: [vv] * 3, lambda u, vv, P, Q: [1 - vv] * 3)
T("splittb", [("p", "ND_splittb_color3", "color3f", [v("color3f", "valuet", "(1,0,0)"), v("color3f", "valueb", "(0,0,1)"), v("float", "center", 0.5)])], "p",
  lambda u, vv, P, Q: [1, 0, 0] if vv > 0.5 else [0, 0, 1], lambda u, vv, P, Q: [0, 0, 1] if vv > 0.5 else [1, 0, 0])
T("splitlr", [("p", "ND_splitlr_color3", "color3f", [v("color3f", "valuel", "(1,0,0)"), v("color3f", "valuer", "(0,0,1)"), v("float", "center", 0.5)])], "p",
  lambda u, vv, P, Q: [1, 0, 0] if u < 0.5 else [0, 0, 1])
T("noise2d_c3", scaled_tc("tc4", 4) + [("p", "ND_noise2d_color3", "color3f", [v("color3f", "amplitude", "(1,1,1)"), v("float", "pivot", 0.5), c("float2", "texcoord", "tc4")])], "p",
  lambda u, vv, P, Q: [0.5 + n for n in mx.perlin_v3_2d(4 * u, 4 * vv)])
T("noise3d_Pobject_default", [("p", "ND_noise3d_color3", "color3f", [v("color3f", "amplitude", "(1,1,1)"), v("float", "pivot", 0.5)])], "p",
  lambda u, vv, P, Q: [0.5 + n for n in mx.perlin_v3_3d(*P)], lambda u, vv, P, Q: [0.5 + n for n in mx.perlin_v3_3d(*Q)])
T("noise3d_Pworld_x2", [("pos", "ND_position_vector3", "float3", [v("string", "space", '"world"')]),
                        ("pos2", "ND_multiply_vector3FA", "float3", [c("float3", "in1", "pos"), v("float", "in2", 2)]),
                        ("p", "ND_noise3d_color3", "color3f", [v("color3f", "amplitude", "(1,1,1)"), v("float", "pivot", 0.5), c("float3", "position", "pos2")])], "p",
  lambda u, vv, P, Q: [0.5 + n for n in mx.perlin_v3_3d(2 * Q[0], 2 * Q[1], 2 * Q[2])])
T("fractal3d_Pobj_x3", [("pos", "ND_position_vector3", "float3", [v("string", "space", '"object"')]),
                        ("pos3", "ND_multiply_vector3FA", "float3", [c("float3", "in1", "pos"), v("float", "in2", 3)]),
                        ("f", "ND_fractal3d_color3", "color3f", [v("color3f", "amplitude", "(1,1,1)"), v("int", "octaves", 3), v("float", "lacunarity", 2), v("float", "diminish", 0.5), c("float3", "position", "pos3")]),
                        ("p", "ND_add_color3FA", "color3f", [c("color3f", "in1", "f"), v("float", "in2", 1)])], "p",
  lambda u, vv, P, Q: [1 + n for n in mx.fractal_v3(3 * P[0], 3 * P[1], 3 * P[2], 3, 2.0, 0.5)])
T("cellnoise2d", scaled_tc("tc4", 4) + [("n", "ND_cellnoise2d_float", "float", [c("float2", "texcoord", "tc4")]),
                                        ("p", "ND_convert_float_color3", "color3f", [c("float", "in", "n")])], "p",
  lambda u, vv, P, Q: [mx.cell2(4 * u, 4 * vv)] * 3)
T("worley2d_dist", scaled_tc("tc4", 4) + [("n", "ND_worleynoise2d_vector3", "float3", [c("float2", "texcoord", "tc4"), v("float", "jitter", 1), v("int", "style", 0)]),
                                          ("p", "ND_convert_vector3_color3", "color3f", [c("float3", "in", "n")])], "p",
  lambda u, vv, P, Q: mx.worley2(4 * u, 4 * vv, 1.0, 0))
T("worley2d_solid", scaled_tc("tc4", 4) + [("n", "ND_worleynoise2d_vector3", "float3", [c("float2", "texcoord", "tc4"), v("float", "jitter", 1), v("int", "style", 1)]),
                                           ("p", "ND_convert_vector3_color3", "color3f", [c("float3", "in", "n")])], "p",
  lambda u, vv, P, Q: mx.worley2(4 * u, 4 * vv, 1.0, 1))
TEXC = {(0, 0): [1, 0, 0], (1, 0): [1, 1, 1], (0, 1): [0, 0, 1], (1, 1): [0, 0, 0]}  # (col, row-from-top)
def tex_lookup(s, t):  # MaterialX: t=0 is the bottom of the image
    col = min(1, int(s * 2)); row_from_bottom = min(1, int(t * 2))
    return TEXC[(col, 1 - row_from_bottom)]
def addr(x, mode):
    if mode == "periodic": return x - math.floor(x)
    if mode == "clamp": return min(max(x, 0.0), 0.999999)
    if mode == "mirror":
        i = math.floor(x); f = x - i
        return 1 - f if (i & 1) else f
    return x
def img_expect(umode, vmode):
    def f(u, vv, P, Q):
        s, t = 3 * u - 1, 3 * vv - 1
        if (umode == "constant" and not 0 <= s <= 1) or (vmode == "constant" and not 0 <= t <= 1):
            return [0, 1, 0]
        return tex_lookup(addr(s, umode), addr(t, vmode))
    return f
for um, vm in (("constant", "periodic"), ("mirror", "clamp")):
    T(f"image_{um}_{vm}", scaled_tc("tc3", 3, 1) + [("p", "ND_image_color3", "color3f", [v("asset", "file", f"@{TEX2}@"), v("string", "filtertype", '"closest"'),
                                                       v("string", "uaddressmode", f'"{um}"'), v("string", "vaddressmode", f'"{vm}"'), v("color3f", "default", "(0,1,0)"), c("float2", "texcoord", "tc3")])], "p",
      img_expect(um, vm))
g8 = 128 / 255.0
T("image_png_auto", [("p", "ND_image_color3", "color3f", [v("asset", "file", f"@{GRAY}@")])], "p",
  lambda u, vv, P, Q: [srgb_to_lin(g8)] * 3)
T("image_png_meta_linrec709", [("p", "ND_image_color3", "color3f", [f'asset inputs:file = @{GRAY}@ (\n                colorSpace = "lin_rec709"\n            )'])], "p",
  lambda u, vv, P, Q: [g8] * 3, lambda u, vv, P, Q: [srgb_to_lin(g8)] * 3)
T("smoothstep_inverted", [TC, ("x", "ND_extract_vector2", "float", [c("float2", "in", "tc"), v("int", "index", 0)]),
                          ("s", "ND_smoothstep_float", "float", [c("float", "in", "x"), v("float", "low", 1), v("float", "high", 0)]),
                          ("p", "ND_convert_float_color3", "color3f", [c("float", "in", "s")])], "p",
  lambda u, vv, P, Q: [1.0] * 3, lambda u, vv, P, Q: [(1 - u) ** 2 * (3 - 2 * (1 - u))] * 3)
T("luminance_default", [("p", "ND_luminance_color3", "color3f", [v("color3f", "in", "(1,0,0)")])], "p",
  lambda u, vv, P, Q: [0.2722287] * 3, lambda u, vv, P, Q: [0.2126] * 3)
T("modulo_neg", [TC, ("x", "ND_extract_vector2", "float", [c("float2", "in", "tc"), v("int", "index", 0)]),
                 ("x4", "ND_multiply_float", "float", [c("float", "in1", "x"), v("float", "in2", 4)]),
                 ("x2", "ND_subtract_float", "float", [c("float", "in1", "x4"), v("float", "in2", 2)]),
                 ("m", "ND_modulo_float", "float", [c("float", "in1", "x2"), v("float", "in2", 1)]),
                 ("p", "ND_convert_float_color3", "color3f", [c("float", "in", "m")])], "p",
  lambda u, vv, P, Q: [(4 * u - 2) - math.floor(4 * u - 2)] * 3, lambda u, vv, P, Q: [math.fmod(4 * u - 2, 1.0)] * 3)
T("image_vector3_raw", [("i", "ND_image_vector3", "float3", [v("asset", "file", f"@{GRAY}@")]),
                        ("p", "ND_convert_vector3_color3", "color3f", [c("float3", "in", "i")])], "p",
  lambda u, vv, P, Q: [g8] * 3)
T("checkerboard", [("p", "ND_checkerboard_color3", "color3f", [])], "p",
  lambda u, vv, P, Q: [float((math.floor(8 * u) + math.floor(8 * vv)) % 2)] * 3)
T("noise3d_Pobject_pointinstancer", [("p", "ND_noise3d_color3", "color3f", [v("color3f", "amplitude", "(1,1,1)"), v("float", "pivot", 0.5)])], "p",
  lambda u, vv, P, Q: [0.5 + n for n in mx.perlin_v3_3d(*P)], lambda u, vv, P, Q: [0.5 + n for n in mx.perlin_v3_3d(*Q)])
tests[-1]["instanced"] = True
TEX2EXR = OUT + "/tex2x2_tiled.exr"
T("image_exr_constant_default", scaled_tc("tc3", 3, 1) + [("p", "ND_image_color3", "color3f", [v("asset", "file", f"@{TEX2EXR}@"), v("string", "filtertype", '"closest"'),
                                                   v("string", "uaddressmode", '"constant"'), v("string", "vaddressmode", '"periodic"'), v("color3f", "default", "(0,1,0)"), c("float2", "texcoord", "tc3")])], "p",
  img_expect("constant", "periodic"), lambda u, vv, P, Q: [0, 0, 0] if not 0 <= 3 * u - 1 <= 1 else img_expect("constant", "periodic")(u, vv, P, Q))
T("image_png_constant_blackalt", scaled_tc("tc3", 3, 1) + [("p", "ND_image_color3", "color3f", [v("asset", "file", f"@{TEX2}@"), v("string", "filtertype", '"closest"'),
                                                   v("string", "uaddressmode", '"constant"'), v("string", "vaddressmode", '"periodic"'), v("color3f", "default", "(0,1,0)"), c("float2", "texcoord", "tc3")])], "p",
  img_expect("constant", "periodic"), lambda u, vv, P, Q: [0, 0, 0] if not 0 <= 3 * u - 1 <= 1 else img_expect("constant", "periodic")(u, vv, P, Q))
T("dodge_fg0_bg03", [("p", "ND_dodge_color3", "color3f", [v("color3f", "fg", "(0,0,0)"), v("color3f", "bg", "(0.3,0.3,0.3)"), v("float", "mix", 1)])], "p",
  lambda u, vv, P, Q: [0.3] * 3, lambda u, vv, P, Q: [0.7] * 3)
T("burn_fg05_bg08", [("p", "ND_burn_color3", "color3f", [v("color3f", "fg", "(0.5,0.5,0.5)"), v("color3f", "bg", "(0.8,0.8,0.8)"), v("float", "mix", 1)])], "p",
  lambda u, vv, P, Q: [0.6] * 3)
T("smoothstep_low_eq_high", [TC, ("x", "ND_extract_vector2", "float", [c("float2", "in", "tc"), v("int", "index", 0)]),
                          ("s", "ND_smoothstep_float", "float", [c("float", "in", "x"), v("float", "low", 0.5), v("float", "high", 0.5)]),
                          ("p", "ND_convert_float_color3", "color3f", [c("float", "in", "s")])], "p",
  lambda u, vv, P, Q: [1.0 if u >= 0.5 else 0.0] * 3, lambda u, vv, P, Q: [0.0] * 3)
T("divide_by_zero", [TC, ("x", "ND_extract_vector2", "float", [c("float2", "in", "tc"), v("int", "index", 0)]),
                     ("d", "ND_divide_float", "float", [c("float", "in1", "x"), v("float", "in2", 0)]),
                     ("p", "ND_convert_float_color3", "color3f", [c("float", "in", "d")])], "p",
  lambda u, vv, P, Q: [float("inf")] * 3, lambda u, vv, P, Q: [0.0] * 3)
assert len(tests) <= COLS * ROWS, len(tests)

def quad_origin(k):
    col, row = k % COLS, k // COLS
    return col * PITCH + (PITCH - 1) / 2, row * PITCH + (PITCH - 1) / 2

def scene():
    mats, meshes = [], []
    for k, t in enumerate(tests):
        mpath = f"/World/mats/m{k}"
        body = []
        for (name, nid, otype, inputs) in t["nodes"]:
            lines = "\n".join("            " + l.replace("{M}", mpath) for l in inputs)
            body.append(f'''        def Shader "{name}"
        {{
            uniform token info:id = "{nid}"
{lines}
            {otype} outputs:out
        }}''')
        seen = set(); uniq = []
        for b in body:
            key = b.split('"')[1]
            if key not in seen:
                seen.add(key); uniq.append(b)
        mats.append(f'''    def Material "m{k}"
    {{
        token outputs:mtlx:surface.connect = <{mpath}/surf.outputs:out>
        def Shader "surf"
        {{
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color.connect = <{mpath}/{t["out"]}.outputs:out>
            token outputs:out
        }}
{chr(10).join(uniq)}
    }}''')
        x0, y0 = quad_origin(k)
        quad = f'''int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(0, 0, {ZOFF}), (1, 0, {ZOFF}), (1, 1, {ZOFF}), (0, 1, {ZOFF})]
            texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = "vertex")
            uniform token subdivisionScheme = "none"
            rel material:binding = </World/mats/m{k}>'''
        if t.get("instanced"):
            # Point instancer at the origin; the prototype quad is moved by the instance position.
            meshes.append(f'''    def PointInstancer "pi{k}"
    {{
        point3f[] positions = [({x0}, {y0}, {-ZOFF})]
        int[] protoIndices = [0]
        rel prototypes = </World/pi{k}/proto>
        def Mesh "proto" (prepend apiSchemas = ["MaterialBindingAPI"])
        {{
            {quad}
        }}
    }}''')
        else:
            meshes.append(f'''    def Mesh "q{k}" (prepend apiSchemas = ["MaterialBindingAPI"])
    {{
            {quad}
        double3 xformOp:translate = ({x0}, {y0}, {-ZOFF})
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}''')
    return f'''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{{
    def Scope "mats"
    {{
{chr(10).join(mats)}
    }}
{chr(10).join(meshes)}
    def Camera "cam"
    {{
        token projection = "orthographic"
        float horizontalAperture = {COLS * PITCH * 10}
        float verticalAperture = {ROWS * PITCH * 10}
        float2 clippingRange = (0.1, 1000)
        double3 xformOp:translate = ({COLS * PITCH / 2}, {ROWS * PITCH / 2}, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
}}
def Scope "Render"
{{
    def RenderSettings "settings"
    {{
        rel camera = </World/cam>
        int2 resolution = ({W}, {H})
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = 16
    }}
}}
'''

def write_ppm(path, w, h, pixels):
    with open(path, "wb") as f:
        f.write(f"P6 {w} {h} 255\n".encode())
        f.write(bytes(pixels))

def main():
    hoiio = os.path.join(HFS, "bin", "hoiiotool.exe")
    write_ppm(OUT + "/tex2x2.ppm", 2, 2, [255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 0])
    write_ppm(OUT + "/gray128.ppm", 4, 4, [128] * 48)
    for b in ("tex2x2", "gray128"):
        subprocess.run([hoiio, OUT + f"/{b}.ppm", "-o", OUT + f"/{b}.png"], check=True)
    subprocess.run([hoiio, OUT + "/tex2x2.ppm", "--tile", "64", "64", "-d", "half", "-o", OUT + "/tex2x2_tiled.exr"], check=True)
    usd = OUT + "/probe.usda"
    open(usd, "w").write(scene())
    env = dict(os.environ)
    prefs = tempfile.mkdtemp(prefix="nova_hprefs_", dir=OUT)
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for var in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
        env.pop(var, None)
    env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    exr = OUT + "/probe_karma.exr"
    if "--skip-render" not in sys.argv:
        p = subprocess.run([os.path.join(HFS, "bin", "husk.exe"), "--renderer", "BRAY_HdKarma", "--settings", "/Render/settings",
                            "-V", "2", "-o", exr, usd], env=env, capture_output=True, text=True)
        print(p.stdout[-3000:], p.stderr[-3000:])
    dump = subprocess.run([hoiio, "--dumpdata", exr], capture_output=True, text=True).stdout
    img = {}
    for m in re.finditer(r"Pixel \((\d+), (\d+)\):\s*([-0-9.e]+)\s+([-0-9.e]+)\s+([-0-9.e]+)", dump):
        img[(int(m.group(1)), int(m.group(2)))] = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
    print("pixels read:", len(img))
    for k, t in enumerate(tests):
        x0, y0 = quad_origin(k)
        res = {"mx": [], "karma": []}
        for iy in range(4, PPU, 8):
            for ix in range(4, PPU, 8):
                px = int(x0 * PPU) + ix; py = H - 1 - (int(y0 * PPU) + iy)
                wx = (px + 0.5) / PPU; wy = (H - (py + 0.5)) / PPU
                u, vv = wx - x0, wy - y0
                if not (0.02 < u < 0.98 and 0.02 < vv < 0.98): continue
                P = (u, vv, ZOFF); Q = (wx, wy, 0.0)
                got = img.get((px, py))
                if got is None: continue
                for key in ("mx", "karma"):
                    f = t[key] or t["mx"]
                    e = f(u, vv, P, Q)
                    res[key].append(max(abs(got[i] - e[i]) for i in range(3)))
        def summ(errs):
            if not errs: return "n/a"
            frac = sum(1 for e in errs if e < 0.02) / len(errs)
            return f"median {statistics.median(errs):.4f}  within0.02 {100 * frac:5.1f}%"
        line = f"{t['name']:<26} MX-ref: {summ(res['mx'])}"
        if t["karma"]: line += f" | alt(Karma VEX template): {summ(res['karma'])}"
        print(line)

if __name__ == "__main__":
    main()
