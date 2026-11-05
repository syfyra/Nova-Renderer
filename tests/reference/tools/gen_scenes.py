"""Writes the reference scenes in tests/reference/scenes (run once; the
generated .usda files are committed, edit this script and rerun to change
them). Standard library only.

    python gen_scenes.py <tests/reference/scenes> [--hfs <HFS>]

With --hfs the missing image assets (sky_soft.exr, rect_pattern.exr and the
textures of mtl_textured) are generated with Houdini's hoiiotool as well;
--force-assets rewrites existing ones (which invalidates the references that
hash them).

Rules for every scene (see tests/reference/README.md):
  * sublayer common.usda; the RenderSettings prim inherits _class_RefSettings
    and sets the same ray limits for both renderers (diffuse N - 1 for N lit
    events; reflect and refract as the scene sets them for Karma, else both
    renderers' default 4);
  * explicit meshes only (no UsdGeom implicit prims; implicit_spheres tests
    the analytic spheres on purpose), subdivisionScheme "none"
    unless subdivision is under test, doubleSided = 1, albedo from
    primvars:displayColor through /Looks/displayColorLambert;
  * no surface inside a sphere or cylinder light (Nova: no emission inside,
    UsdLux; Karma: some other value);
  * camera fStop 0 unless depth of field is under test (Karma's DOF is on by
    default).
"""

import math
import os
import re
import subprocess
import sys

COMMON_HEADER = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = {mpu}
    upAxis = "Y"
    subLayers = [@./common.usda@]
{extra})

"""


def f3(v, digits=6):
    return "(" + ", ".join(f"{x:.{digits}g}" for x in v) + ")"


def mesh(name, counts, indices, points, color, normals=None, scheme="none", extra="", indent="    "):
    lines = [f'def Mesh "{name}" (', '    prepend apiSchemas = ["MaterialBindingAPI"]', ")", "{",
             f"    int[] faceVertexCounts = [{', '.join(map(str, counts))}]",
             f"    int[] faceVertexIndices = [{', '.join(map(str, indices))}]",
             f"    point3f[] points = [{', '.join(f3(p) for p in points)}]"]
    if normals:
        lines += [f"    normal3f[] normals = [{', '.join(f3(n, 5) for n in normals)}] (",
                  '        interpolation = "vertex"', "    )"]
    lines += [f'    uniform token subdivisionScheme = "{scheme}"', "    uniform bool doubleSided = 1",
              f"    color3f[] primvars:displayColor = [{f3(color)}]",
              "    rel material:binding = </Looks/displayColorLambert>"]
    lines += ["    " + l for l in extra.strip().splitlines() if l.strip()]
    lines.append("}")
    return "\n".join(indent + l for l in lines) + "\n"


def quads(name, quad_list, color, indent="    "):
    pts, idx = [], []
    for q in quad_list:
        idx += [len(pts) + i for i in range(4)]
        pts += q
    return mesh(name, [4] * len(quad_list), idx, pts, color, indent=indent)


def box_quads(center, size, rot_y_deg=0.0):
    sx, sy, sz = (s * 0.5 for s in size)
    c, s = math.cos(math.radians(rot_y_deg)), math.sin(math.radians(rot_y_deg))

    def xf(x, y, z):
        return (center[0] + c * x + s * z, center[1] + y, center[2] - s * x + c * z)

    def face(xs, ys, zs):
        return [xf(x * sx, y * sy, z * sz) for x, y, z in zip(xs, ys, zs)]

    return [face([-1, 1, 1, -1], [-1, -1, 1, 1], [1, 1, 1, 1]), face([1, -1, -1, 1], [-1, -1, 1, 1], [-1, -1, -1, -1]),
            face([1, 1, 1, 1], [-1, -1, 1, 1], [1, -1, -1, 1]), face([-1, -1, -1, -1], [-1, -1, 1, 1], [-1, 1, 1, -1]),
            face([-1, 1, 1, -1], [1, 1, 1, 1], [1, 1, -1, -1]), face([-1, 1, 1, -1], [-1, -1, -1, -1], [-1, -1, 1, 1])]


def sphere_asset(nu=48, nv=24, st=False, uv=False):
    """Unit UV sphere with vertex normals (assets/sphere.usda; with st:
    assets/sphere_st.usda, u around +Y from +X, v from the bottom pole; with
    uv: the same coordinates also as Houdini's uv primvar, assets/sphere_uv.usda)."""
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
    body = (f"    int[] faceVertexCounts = [{', '.join(['4'] * (nu * nv))}]\n"
            f"    int[] faceVertexIndices = [{', '.join(map(str, idx))}]\n"
            f"    point3f[] points = [{', '.join(f3(p, 5) for p in pts)}]\n"
            f"    normal3f[] normals = [{', '.join(f3(p, 5) for p in pts)}] (\n"
            '        interpolation = "vertex"\n    )\n'
            '    uniform token subdivisionScheme = "none"\n    uniform bool doubleSided = 1\n')
    if st or uv:
        uvs = [(i / nu, 1.0 - j / nv) for j in range(nv + 1) for i in range(nu + 1)]
        for name in ["st"] + (["uv"] if uv else []):
            body += (f"    texCoord2f[] primvars:{name} = [{', '.join(f'({u:.5g}, {v:.5g})' for u, v in uvs)}] (\n"
                     '        interpolation = "vertex"\n    )\n')
    return ('#usda 1.0\n(\n    defaultPrim = "sphere"\n    metersPerUnit = 1\n    upAxis = "Y"\n)\n\n'
            f'def Mesh "sphere"\n{{\n{body}}}\n')


def sphere(name, center, radius, color, indent="    "):
    text = f'''def Mesh "{name}" (
    prepend apiSchemas = ["MaterialBindingAPI"]
    prepend references = @./assets/sphere.usda@
)
{{
    color3f[] primvars:displayColor = [{f3(color)}]
    rel material:binding = </Looks/displayColorLambert>
    double3 xformOp:translate = {f3(center)}
    double3 xformOp:scale = ({radius}, {radius}, {radius})
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
}}'''
    return "\n".join(indent + l for l in text.splitlines()) + "\n"


def xform_ops(translate=None, rotate=None):
    ops, order = [], []
    if translate is not None:
        ops.append(f"double3 xformOp:translate = {f3(translate)}")
        order.append('"xformOp:translate"')
    if rotate is not None:
        ops.append(f"float3 xformOp:rotateXYZ = {f3(rotate)}")
        order.append('"xformOp:rotateXYZ"')
    ops.append(f"uniform token[] xformOpOrder = [{', '.join(order)}]")
    return "\n        ".join(ops)


def light(kind, name, attrs, translate=None, rotate=None, schemas=None):
    meta = f' (\n        prepend apiSchemas = [{", ".join(chr(34) + s + chr(34) for s in schemas)}]\n    )' if schemas else ""
    body = "\n        ".join(attrs)
    return f"""    def {kind} "{name}"{meta}
    {{
        {body}
        {xform_ops(translate, rotate)}
    }}
"""


def camera(t, r=(0, 0, 0), focal=35, aperture=(36, 21.6), extra=""):
    return f"""    def Camera "cam"
    {{
        float focalLength = {focal}
        float horizontalAperture = {aperture[0]}
        float verticalAperture = {aperture[1]}
        float2 clippingRange = (0.01, 10000){extra}
        {xform_ops(t, r)}
    }}
"""


def settings(res, depth, extra="", render_extra=""):
    return f"""def Scope "Render"
{{
    def RenderSettings "ref" (
        inherits = </_class_RefSettings>
    )
    {{
        rel camera = </World/cam>
        int2 resolution = ({res[0]}, {res[1]})
        float nova:object:diffuseLimit = {depth - 1}{nova_limits(extra)}
        float karma:object:diffuselimit = {depth - 1}{extra}
    }}
{render_extra}}}
"""


def nova_limits(extra):
    """Nova's reflect and refract limits equal to the Karma ones in `extra`
    (docs/design/M4.md 3.2: the same semantics)."""
    out = ""
    for kind in ("reflect", "refract"):
        m = re.search(rf"karma:object:{kind}limit = (\S+)", extra)
        if m:
            out += f"\n        float nova:object:{kind}Limit = {m.group(1)}"
    return out


def scene(body, res, depth, mpu=1, header_extra="", settings_extra="", render_extra=""):
    return (COMMON_HEADER.format(mpu=mpu, extra=header_extra) + 'def Xform "World"\n{\n' + body + "}\n\n"
            + settings(res, depth, settings_extra, render_extra))


WHITE, RED, GREEN = (0.73, 0.73, 0.73), (0.63, 0.065, 0.05), (0.14, 0.45, 0.091)


def cornell_rect():
    b = quads("floor", [[(-1, 0, 1), (1, 0, 1), (1, 0, -1), (-1, 0, -1)]], WHITE)
    b += quads("back", [[(-1, 0, -1), (1, 0, -1), (1, 2, -1), (-1, 2, -1)]], WHITE)
    b += quads("left", [[(-1, 0, 1), (-1, 0, -1), (-1, 2, -1), (-1, 2, 1)]], RED)
    b += quads("right", [[(1, 0, -1), (1, 0, 1), (1, 2, 1), (1, 2, -1)]], GREEN)
    # The ceiling has a hole for the light; the light fills it exactly, in
    # the ceiling plane (a gap lets camera rays leak past its edges).
    b += quads("ceiling", [[(-1, 2, 0.2), (1, 2, 0.2), (1, 2, 1), (-1, 2, 1)],
                           [(-1, 2, -1), (1, 2, -1), (1, 2, -0.2), (-1, 2, -0.2)],
                           [(-1, 2, -0.2), (-0.25, 2, -0.2), (-0.25, 2, 0.2), (-1, 2, 0.2)],
                           [(0.25, 2, -0.2), (1, 2, -0.2), (1, 2, 0.2), (0.25, 2, 0.2)]], WHITE)
    b += quads("shortBox", box_quads((0.33, 0.3, 0.3), (0.6, 0.6, 0.6), -18), WHITE)
    b += quads("tallBox", box_quads((-0.33, 0.6, -0.3), (0.6, 1.2, 0.6), 17), WHITE)
    b += light("RectLight", "light", ["float inputs:intensity = 12", "color3f inputs:color = (1, 0.8, 0.55)",
                                      "float inputs:width = 0.5", "float inputs:height = 0.4",
                                      "bool inputs:normalize = 0", "bool inputs:karma:light:renderlightgeo = 1"],
               translate=(0, 2, 0), rotate=(-90, 0, 0))
    b += camera((0, 1, 3.4), focal=35, aperture=(25.4, 25.4))
    return scene(b, (128, 128), 8)


def light_stage():
    """Ground, a white back wall and three spheres, seen from the front."""
    b = quads("ground", [[(-20, 0, 20), (20, 0, 20), (20, 0, -20), (-20, 0, -20)]], (0.6, 0.6, 0.6))
    b += quads("wall", [[(-6, 0, -2.5), (6, 0, -2.5), (6, 4, -2.5), (-6, 4, -2.5)]], (0.7, 0.7, 0.7))
    b += sphere("ballL", (-1.3, 0.5, 0.0), 0.5, (0.8, 0.35, 0.25))
    b += sphere("ballM", (0.0, 0.5, -0.6), 0.5, (0.75, 0.75, 0.75))
    b += sphere("ballR", (1.3, 0.5, 0.0), 0.5, (0.25, 0.4, 0.8))
    b += camera((0, 1.4, 5.2), (-10, 0, 0))
    return b


def light_scene(light_prims):
    return scene(light_stage() + light_prims, (160, 96), 4)


def light_scenes():
    return {
        "light_sphere.usda": light_scene(light(
            "SphereLight", "bulb", ["float inputs:intensity = 25", "color3f inputs:color = (1, 0.85, 0.7)",
                                    "float inputs:radius = 0.25"], translate=(-1.9, 2.0, 1.0))),
        # Single-sided disk whose back faces the wall: emission from the back
        # would light the upper wall.
        "light_disk.usda": light_scene(light(
            "DiskLight", "disk", ["float inputs:intensity = 10", "color3f inputs:color = (0.8, 0.9, 1)",
                                  "float inputs:radius = 0.5"], translate=(1.6, 1.8, 0.6), rotate=(-50, 35, 0))),
        "light_cylinder.usda": light_scene(light(
            "CylinderLight", "tube", ["float inputs:intensity = 6", "bool inputs:normalize = 1",
                                      "color3f inputs:color = (0.7, 1, 0.75)", "float inputs:length = 1.5",
                                      "float inputs:radius = 0.06"], translate=(0, 2.2, 0.6), rotate=(0, 20, 0))),
        "light_spot.usda": light_scene(light(
            "SphereLight", "spot", ["float inputs:intensity = 30", "bool treatAsPoint = 1",
                                    "float inputs:shaping:cone:angle = 30", "float inputs:shaping:cone:softness = 0.4",
                                    "float inputs:shaping:focus = 2", "color3f inputs:shaping:focusTint = (1, 0.5, 0.2)"],
            translate=(0.5, 3.0, 2.0), rotate=(-55, 10, 0), schemas=["ShapingAPI"])),
        "light_point.usda": light_scene(light(
            "SphereLight", "point", ["float inputs:intensity = 12", "bool inputs:normalize = 1", "bool treatAsPoint = 1",
                                     "float inputs:radius = 0.5"], translate=(-0.8, 2.6, 1.4))),
        # Camera-visible textured rect seen from its emitting side, which
        # points to (0.60, -0.29, 0.74): towards the spheres and the camera.
        # Tests the texture orientation (karma:light:recttextureflip default).
        "light_rect_textured.usda": light_scene(light(
            "RectLight", "panel", ["float inputs:intensity = 3", "float inputs:width = 1.6", "float inputs:height = 0.8",
                                   "asset inputs:texture:file = @./assets/rect_pattern.exr@",
                                   "bool inputs:karma:light:renderlightgeo = 1"],
            translate=(-2.6, 1.6, -1.2), rotate=(-17, -141, 0))),
    }


def dome_sky():
    b = quads("ground", [[(-20, 0, 20), (20, 0, 20), (20, 0, -20), (-20, 0, -20)]], (0.6, 0.6, 0.6))
    b += sphere("ballA", (-0.7, 0.5, 0.0), 0.5, (0.8, 0.3, 0.2))
    b += sphere("ballB", (0.6, 0.35, 0.4), 0.35, (0.3, 0.5, 0.8))
    b += quads("cube", box_quads((0.5, 0.4, -0.7), (0.8, 0.8, 0.8), 25), (0.7, 0.7, 0.6))
    b += light("DomeLight", "sky", ["float inputs:intensity = 1", "asset inputs:texture:file = @./assets/sky_soft.exr@",
                                    'token inputs:texture:format = "latlong"',
                                    "# Karma's default MIS for textured domes is biased (-0.6..-1% measured)",
                                    "float inputs:karma:light:misbias = -1",
                                    "bool inputs:karma:light:renderlightgeo = 1"])
    # Looks towards -Z, away from the sun (u = 0.375: +X+Z, 35 degrees up).
    b += camera((0, 1.0, 3.5), (-10, 0, 0), aperture=(36, 24))
    return scene(b, (144, 96), 4)


def dome_plane():
    b = quads("ground", [[(-1000, 0, 1000), (1000, 0, 1000), (1000, 0, -1000), (-1000, 0, -1000)]], (0.5, 0.5, 0.5))
    b += light("DomeLight", "sky", ["float inputs:intensity = 1", "asset inputs:texture:file = @./assets/sky_soft.exr@",
                                    'token inputs:texture:format = "latlong"', "float inputs:karma:light:misbias = -1"])
    b += camera((0, 10, 0), (-90, 0, 0), focal=50, aperture=(20, 20))
    return scene(b, (32, 32), 1)


def furnace_rectbox():
    cup = [[(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5)],
           [(-0.5, -0.5, -0.5), (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5), (0.5, -0.5, -0.5)],
           [(0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5), (0.5, -0.5, 0.5)],
           [(0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5), (-0.5, -0.5, 0.5)],
           [(-0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (-0.5, 0.5, -0.5), (-0.5, -0.5, -0.5)]]
    b = quads("cup", cup, (1, 1, 1))
    b += sphere("ball", (0.0, -0.15, 0.0), 0.3, (1, 1, 1))
    walls = {"px": ((3, 0, 0), (0, 90, 0)), "nx": ((-3, 0, 0), (0, -90, 0)), "py": ((0, 3, 0), (-90, 0, 0)),
             "ny": ((0, -3, 0), (90, 0, 0)), "pz": ((0, 0, 3), (0, 0, 0)), "nz": ((0, 0, -3), (0, 180, 0))}
    for n, (t, r) in walls.items():  # local -Z (the emitting side) points inwards
        b += light("RectLight", f"wall_{n}", ["float inputs:intensity = 1", "float inputs:width = 6",
                                              "float inputs:height = 6", "bool inputs:normalize = 0",
                                              "bool inputs:karma:light:renderlightgeo = 1"], translate=t, rotate=r)
    b += camera((0, 1.2, 1.9), (-35, 0, 0), focal=25, aperture=(25.4, 25.4))
    return scene(b, (64, 64), 64)



# ---------------------------------------------------------------------------
# Materials (M3): scene-local MaterialX materials, bound per sphere
# ---------------------------------------------------------------------------

def mtlx_material(name, node_id, inputs, context="mtlx", output="out"):
    """A Material with one surface shader in a render context ("mtlx", "kma",
    or "" for the universal one); inputs: name -> (type, value)."""
    terminal = f"outputs:{context}:surface" if context else "outputs:surface"
    lines = [f'        def Material "{name}"', "        {",
             f"            token {terminal}.connect = </World/Looks/{name}/surface.outputs:{output}>",
             '            def Shader "surface"', "            {",
             f'                uniform token info:id = "{node_id}"']
    for key, (typ, value) in inputs.items():
        text = f3(value) if isinstance(value, tuple) else (str(int(value)) if typ == "bool" else f"{value:g}")
        lines.append(f"                {typ} inputs:{key} = {text}")
    lines += [f"                token outputs:{output}", "            }", "        }"]
    return "\n".join(lines) + "\n"


PRINCIPLED = "opdef:/Vop/principledshader::2.0?"


def principled_material(name, inputs, displacement=None):
    """A Principled Shader 2.0 material as Solaris writes it (M5): a vex
    context with the shader as an opdef sourceAsset; inputs and displacement
    inputs: name -> (type, value)."""
    def value(typ, v):
        if typ == "asset":
            return f"@{v}@"
        if typ == "string":
            return f'"{v}"'
        if isinstance(v, tuple):
            return f3(v)
        return str(int(v)) if typ == "int" else f"{v:g}"

    def shader(shader_name, section, ins, output):
        lines = [f'            def Shader "{shader_name}"', "            {",
                 '                uniform token info:implementationSource = "sourceAsset"',
                 f"                uniform asset info:sourceAsset = @{PRINCIPLED}{section}@"]
        lines += [f"                {typ} inputs:{key} = {value(typ, v)}" for key, (typ, v) in ins.items()]
        lines += [f"                token outputs:{output}", "            }"]
        return lines

    base = f"/World/Looks/{name}"
    lines = [f'        def Material "{name}"', "        {",
             f"            token outputs:vex:surface.connect = <{base}/surface.outputs:surface>"]
    if displacement:
        lines.append(f"            token outputs:vex:displacement.connect = <{base}/displace.outputs:displacement>")
    lines += shader("surface", "SurfaceVexCode", inputs, "surface")
    if displacement:
        lines += shader("displace", "DisplacementVexCode", displacement, "displacement")
    lines.append("        }")
    return "\n".join(lines) + "\n"


def vex_material(name, code, inputs=None, displacement=None, displacement_inputs=None):
    """A VEX Material Builder-style material (M5.4): the surface function's
    source code in a vex context (Solaris writes builders this way), and
    optionally a displace function's; inputs: name -> (type, value)."""
    def shader(prim, terminal, source, values):
        escaped = source.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        out = [f'            def Shader "{prim}"', "            {",
               '                uniform token info:implementationSource = "sourceCode"',
               f'                string info:sourceCode = "{escaped}"']
        for key, (typ, v) in (values or {}).items():
            text = f"@{v}@" if typ == "asset" else (f3(v) if isinstance(v, tuple) else f"{v:g}")
            out.append(f"                {typ} inputs:{key} = {text}")
        return out + [f"                token outputs:{terminal}", "            }"]

    lines = [f'        def Material "{name}"', "        {",
             f"            token outputs:vex:surface.connect = </World/Looks/{name}/surface.outputs:surface>"]
    if displacement:
        lines.append(f"            token outputs:vex:displacement.connect = "
                     f"</World/Looks/{name}/displace.outputs:displacement>")
    lines += shader("surface", "surface", code, inputs)
    if displacement:
        lines += shader("displace", "displacement", displacement, displacement_inputs)
    lines.append("        }")
    return "\n".join(lines) + "\n"


def looks(materials):
    return '    def Scope "Looks"\n    {\n' + "".join(materials) + "    }\n"


def mat_sphere(name, center, radius, material):
    return (f'    def Mesh "{name}" (\n'
            '        prepend apiSchemas = ["MaterialBindingAPI"]\n'
            '        prepend references = @./assets/sphere.usda@\n'
            '    )\n    {\n'
            f'        rel material:binding = </World/Looks/{material}>\n'
            f'        double3 xformOp:translate = {f3(center)}\n'
            f'        double3 xformOp:scale = ({radius}, {radius}, {radius})\n'
            '        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n'
            '    }\n')


SS, PBR = "ND_standard_surface_surfaceshader", "ND_open_pbr_surface_surfaceshader"


def soft_sky(visible=True):
    attrs = ["float inputs:intensity = 1", "asset inputs:texture:file = @./assets/sky_soft.exr@",
             'token inputs:texture:format = "latlong"', "float inputs:karma:light:misbias = -1"]
    if visible:
        attrs.append("bool inputs:karma:light:renderlightgeo = 1")
    return light("DomeLight", "sky", attrs)


def mtl_spheres():
    """Eight materials lit directly by the soft sky (nova:maxDepth 1; Karma
    limits 0): the lobes and the input mapping of both surface models."""
    mats = [
        ("ss_gold", SS, {"base": ("float", 1.0), "base_color": ("color3f", (0.94, 0.78, 0.37)),
                         "metalness": ("float", 1.0), "specular_roughness": ("float", 0.3)}),
        ("ss_chrome", SS, {"base_color": ("color3f", (0.9, 0.9, 0.9)), "metalness": ("float", 1.0),
                           "specular_roughness": ("float", 0.08)}),
        ("ss_plastic", SS, {"base_color": ("color3f", (0.7, 0.1, 0.08)), "specular_roughness": ("float", 0.25)}),
        ("ss_coat", SS, {"base_color": ("color3f", (0.1, 0.2, 0.6)), "specular_roughness": ("float", 0.5),
                         "coat": ("float", 1.0), "coat_roughness": ("float", 0.05)}),
        ("ss_sheen", SS, {"base_color": ("color3f", (0.2, 0.05, 0.1)), "specular": ("float", 0.0),
                          "sheen": ("float", 1.0), "sheen_color": ("color3f", (0.9, 0.6, 0.8))}),
        ("pbr_copper", PBR, {"base_color": ("color3f", (0.95, 0.64, 0.54)), "base_metalness": ("float", 1.0),
                             "specular_color": ("color3f", (0.98, 0.82, 0.76)),
                             "specular_roughness": ("float", 0.25)}),
        ("pbr_plastic", PBR, {"base_color": ("color3f", (0.1, 0.5, 0.15)), "specular_roughness": ("float", 0.35)}),
        ("pbr_fuzz", PBR, {"base_color": ("color3f", (0.3, 0.3, 0.35)), "fuzz_weight": ("float", 1.0),
                           "fuzz_color": ("color3f", (1.0, 0.9, 0.7)), "fuzz_roughness": ("float", 0.5)}),
    ]
    b = looks([mtlx_material(n, i, inp) for n, i, inp in mats])
    for k, (name, _, _) in enumerate(mats):
        b += mat_sphere(f"ball_{name}", (-1.5 + (k % 4), 0.55 if k < 4 else -0.55, 0.0), 0.42, name)
    b += soft_sky()
    b += camera((0, 0, 5.2), (0, 0, 0), focal=35, aperture=(36, 18))
    return scene(b, (160, 80), 1, settings_extra="\n        float karma:object:reflectlimit = 0"
                                                 "\n        float karma:object:refractlimit = 0")


def mtl_diffuse():
    """Diffuse models against Karma, direct light only: Standard Surface's
    Oren-Nayar (diffuse_roughness 0, 0.5, 1), OpenPBR's EON (0, 0.5, 1),
    UsdPreviewSurface and a default Standard Surface."""
    UP = "UsdPreviewSurface"
    base = (0.7, 0.45, 0.3)
    mats = [
        ("ss_r0", SS, {"base_color": ("color3f", base), "specular": ("float", 0.0)}, "mtlx", "out"),
        ("ss_r05", SS, {"base_color": ("color3f", base), "specular": ("float", 0.0),
                        "diffuse_roughness": ("float", 0.5)}, "mtlx", "out"),
        ("ss_r1", SS, {"base_color": ("color3f", base), "specular": ("float", 0.0),
                       "diffuse_roughness": ("float", 1.0)}, "mtlx", "out"),
        ("ss_default", SS, {}, "mtlx", "out"),
        ("pbr_r0", PBR, {"base_color": ("color3f", base), "specular_weight": ("float", 0.0)}, "mtlx", "out"),
        ("pbr_r05", PBR, {"base_color": ("color3f", base), "specular_weight": ("float", 0.0),
                          "base_diffuse_roughness": ("float", 0.5)}, "mtlx", "out"),
        ("pbr_r1", PBR, {"base_color": ("color3f", base), "specular_weight": ("float", 0.0),
                         "base_diffuse_roughness": ("float", 1.0)}, "mtlx", "out"),
        ("preview", UP, {"diffuseColor": ("color3f", base)}, "", "surface"),
    ]
    b = looks([mtlx_material(n, i, inp, ctx, out) for n, i, inp, ctx, out in mats])
    for k, (name, _, _, _, _) in enumerate(mats):
        b += mat_sphere(f"ball_{name}", (-1.5 + (k % 4), 0.55 if k < 4 else -0.55, 0.0), 0.42, name)
    b += soft_sky()
    b += camera((0, 0, 5.2), (0, 0, 0), focal=35, aperture=(36, 18))
    return scene(b, (160, 80), 1, settings_extra="\n        float karma:object:reflectlimit = 0"
                                                 "\n        float karma:object:refractlimit = 0")


def mtl_kma_context():
    """Materials authored only for Karma's context (outputs:kma:surface, the
    Karma Material Builder): Nova reads them like Karma."""
    mats = [
        ("kma_plastic", SS, {"base_color": ("color3f", (0.1, 0.4, 0.7)), "specular_roughness": ("float", 0.3)}),
        ("kma_metal", PBR, {"base_color": ("color3f", (0.95, 0.64, 0.54)), "base_metalness": ("float", 1.0),
                            "specular_roughness": ("float", 0.25)}),
        ("kma_diffuse", SS, {"base_color": ("color3f", (0.8, 0.2, 0.2)), "specular": ("float", 0.0)}),
    ]
    b = looks([mtlx_material(n, i, inp, "kma") for n, i, inp in mats])
    for k, (name, _, _) in enumerate(mats):
        b += mat_sphere(f"ball_{name}", (-1.1 + 1.1 * k, 0.0, 0.0), 0.48, name)
    b += soft_sky()
    b += camera((0, 0, 4.0), (0, 0, 0), focal=35, aperture=(36, 18))
    return scene(b, (160, 80), 1, settings_extra="\n        float karma:object:reflectlimit = 0"
                                                 "\n        float karma:object:refractlimit = 0")


def mtl_glass():
    """Smooth and rough glass (both models) against the soft sky: purely
    specular paths, so the per-type limits of Karma (7) and Nova's path
    length (8) agree."""
    mats = [
        ("ss_glass", SS, {"base": ("float", 0.0), "transmission": ("float", 1.0),
                          "specular_roughness": ("float", 0.0)}),
        ("ss_rough", SS, {"base": ("float", 0.0), "transmission": ("float", 1.0),
                          "specular_roughness": ("float", 0.3)}),
        ("pbr_glass", PBR, {"transmission_weight": ("float", 1.0), "specular_roughness": ("float", 0.0),
                            "transmission_color": ("color3f", (0.9, 0.95, 1.0))}),
    ]
    b = looks([mtlx_material(n, i, inp) for n, i, inp in mats])
    for k, (name, _, _) in enumerate(mats):
        b += mat_sphere(f"ball_{name}", (-1.1 + 1.1 * k, 0.0, 0.0), 0.5, name)
    b += soft_sky()
    b += camera((0, 0.4, 4.0), (-5, 0, 0), focal=35, aperture=(36, 18))
    return scene(b, (160, 80), 8, settings_extra="\n        float karma:object:reflectlimit = 7"
                                                 "\n        float karma:object:refractlimit = 7")


def furnace_materials():
    """White materials of both models in a uniform white environment: every
    pixel is 1 with unlimited path length (Nova is energy-conserving where
    Karma is not, so no Karma check)."""
    mats = [
        ("ss_white", SS, {"base_color": ("color3f", (1, 1, 1))}),
        ("ss_glass", SS, {"base_color": ("color3f", (1, 1, 1)), "transmission": ("float", 1.0),
                          "specular_roughness": ("float", 0.2)}),
        ("ss_coat", SS, {"base_color": ("color3f", (1, 1, 1)), "coat": ("float", 1.0)}),
        ("pbr_metal", PBR, {"base_color": ("color3f", (1, 1, 1)), "base_metalness": ("float", 1.0),
                            "specular_roughness": ("float", 0.4)}),
        ("pbr_glass", PBR, {"transmission_weight": ("float", 1.0), "specular_roughness": ("float", 0.2)}),
        ("pbr_fuzz", PBR, {"base_color": ("color3f", (1, 1, 1)), "fuzz_weight": ("float", 1.0),
                           "subsurface_color": ("color3f", (1, 1, 1))}),
    ]
    b = looks([mtlx_material(n, i, inp) for n, i, inp in mats])
    for k, (name, _, _) in enumerate(mats):
        b += mat_sphere(f"ball_{name}", (-1.0 + (k % 3), 0.5 if k < 3 else -0.5, 0.0), 0.45, name)
    b += light("DomeLight", "white", ["float inputs:intensity = 1", "bool inputs:karma:light:renderlightgeo = 1"])
    b += camera((0, 0, 4.0), (0, 0, 0), focal=35, aperture=(36, 24))
    # Unlimited paths of every kind (glass and coats need more than the
    # default reflect/refract limits of 4).
    return scene(b, (96, 64), 64, settings_extra="\n        float karma:object:reflectlimit = 63"
                                                  "\n        float karma:object:refractlimit = 63")



def implicit_spheres(karma=False):
    """UsdGeomSphere prims (what the Sphere LOP writes), rendered analytically
    by Nova: radii, a non-uniform scale (an ellipsoid) and a PointInstancer of
    spheres, Lambert through displayColor. Karma tessellates UsdGeomSphere
    (about 7 % smaller silhouettes), so its variant (karma=True) has one-point
    Points prims instead, which Karma renders as exact spheres."""
    def prim(name, radius, indent):
        kind = "Points" if karma else "Sphere"
        head = (f'{indent}def {kind} "{name}" (\n{indent}    prepend apiSchemas = ["MaterialBindingAPI"]\n'
                f"{indent})\n{indent}{{\n")
        if karma:
            return head + f"{indent}    point3f[] points = [(0, 0, 0)]\n{indent}    float[] widths = [{2 * radius:g}]\n"
        return head + f"{indent}    double radius = {radius}\n"

    def gsphere(name, radius, translate, scale=None, color=(0.7, 0.7, 0.7)):
        ops = [f"double3 xformOp:translate = {f3(translate)}"]
        order = ['"xformOp:translate"']
        if scale:
            ops.append(f"float3 xformOp:scale = {f3(scale)}")
            order.append('"xformOp:scale"')
        body = "\n        ".join(ops + [f"uniform token[] xformOpOrder = [{', '.join(order)}]"])
        return (prim(name, radius, "    ") + f"        color3f[] primvars:displayColor = [{f3(color)}]\n"
                "        rel material:binding = </Looks/displayColorLambert>\n"
                f"        {body}\n    }}\n")
    b = quads("ground", [[(-6, 0, 6), (6, 0, 6), (6, 0, -6), (-6, 0, -6)]], (0.5, 0.5, 0.5))
    b += gsphere("big", 0.6, (-1.3, 0.6, 0.0), color=(0.8, 0.3, 0.2))
    b += gsphere("small", 0.25, (0.0, 0.25, 0.6), color=(0.2, 0.6, 0.3))
    b += gsphere("ellipsoid", 0.5, (1.3, 0.35, -0.2), scale=(1.4, 0.7, 0.9), color=(0.25, 0.35, 0.8))
    b += """    def PointInstancer "dots"
    {
        point3f[] positions = [(-0.9, 0.12, 1.2), (-0.3, 0.12, 1.4), (0.3, 0.12, 1.5), (0.9, 0.12, 1.3)]
        int[] protoIndices = [0, 0, 0, 0]
        rel prototypes = [</World/dots/proto>]
""" + prim("proto", 0.12, "        ") + """            color3f[] primvars:displayColor = [(0.9, 0.8, 0.3)]
            rel material:binding = </Looks/displayColorLambert>
        }
    }
"""
    b += light("RectLight", "key", ["float inputs:intensity = 6", "float inputs:width = 2", "float inputs:height = 2"],
               translate=(1.5, 3.5, 2.0), rotate=(-60, 30, 0))
    b += light("DomeLight", "fill", ["float inputs:intensity = 0.25"])
    b += camera((0, 1.6, 4.2), (-18, 0, 0), aperture=(36, 24))
    return scene(b, (144, 96), 3)

# ---------------------------------------------------------------------------
# Pattern nodes (M3.7): unlit probe tiles, compared per pixel
# ---------------------------------------------------------------------------

def usd_value(value):
    if isinstance(value, tuple):
        return f3(value, 7)
    if isinstance(value, str) and value.startswith("@"):
        return value  # asset path
    if isinstance(value, str):
        return f'"{value}"'
    if isinstance(value, bool):
        return str(int(value))
    return f"{value:.7g}"


def node_material(name, nodes):
    """An unlit Material whose emission_color is the color3 output "out" of
    the last node. nodes: [(node, info_id, {input: (usd_type, value)},
    {output: usd_type})]; a value "<node>" or "<node>.<output>" connects."""
    return network_material(name, "ND_surface_unlit", {"emission_color": ("color3f", f"<{nodes[-1][0]}>")}, nodes)


def network_material(name, surface_id, surface_inputs, nodes):
    """A Material with the surface shader `surface_id` and pattern nodes (as
    in node_material); surface inputs may connect to the nodes."""
    base = f"/World/Looks/{name}"

    def value_lines(inputs):
        out = []
        for key, (typ, value) in inputs.items():
            if isinstance(value, str) and value.startswith("<"):
                src, _, o = value[1:-1].partition(".")
                out.append(f"                {typ} inputs:{key}.connect = <{base}/{src}.outputs:{o or 'out'}>")
            else:
                out.append(f"                {typ} inputs:{key} = {usd_value(value)}")
        return out

    lines = [f'        def Material "{name}"', "        {",
             f"            token outputs:mtlx:surface.connect = <{base}/surface.outputs:out>",
             '            def Shader "surface"', "            {",
             f'                uniform token info:id = "{surface_id}"']
    lines += value_lines(surface_inputs)
    lines += ["                token outputs:out", "            }"]
    for node, nid, inputs, outputs in nodes:
        lines += [f'            def Shader "{node}"', "            {", f'                uniform token info:id = "{nid}"']
        lines += value_lines(inputs)
        for out, typ in outputs.items():
            lines.append(f"                {typ} outputs:{out}")
        lines.append("            }")
    lines.append("        }")
    return "\n".join(lines) + "\n"


def tile(name, column, row, material):
    """A unit quad in its own space (Pobject and st from 0 to 1), moved to
    (column, row): with 32 pixels per unit, pixel centers never touch the
    integer lattice of positions scaled by 2, 4 or 8."""
    return (f'    def Mesh "{name}" (\n'
            '        prepend apiSchemas = ["MaterialBindingAPI"]\n'
            '    )\n    {\n'
            "        int[] faceVertexCounts = [4]\n"
            "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
            "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
            "        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (\n"
            '            interpolation = "vertex"\n        )\n'
            '        uniform token subdivisionScheme = "none"\n'
            "        uniform bool doubleSided = 1\n"
            f"        rel material:binding = </World/Looks/{material}>\n"
            f"        double3 xformOp:translate = ({column}, {row}, 0)\n"
            '        uniform token[] xformOpOrder = ["xformOp:translate"]\n'
            "    }\n")


def mtl_nodes():
    """The pattern nodes of M3.7 on unlit tiles (6 x 4 tiles of 32 x 32
    pixels, orthographic camera, no lights): every pixel is a node network
    evaluated at the pixel center, so Nova and Karma agree per pixel."""
    C3, C4, V2, V3, F = "color3f", "color4f", "float2", "float3", "float"
    O3, O4, OF, OV2, OV3 = {"out": C3}, {"out": C4}, {"out": F}, {"out": V2}, {"out": V3}
    tc4 = ("tc", "ND_multiply_vector2FA", {"in1": (V2, "<st>"), "in2": (F, 4.0)}, OV2)
    st = ("st", "ND_texcoord_vector2", {}, OV2)
    pos = ("pos", "ND_position_vector3", {"space": ("string", "object")}, OV3)

    def scaled_pos(scale, offset=0.5):
        return [pos, ("ps", "ND_multiply_vector3FA", {"in1": (V3, "<pos>"), "in2": (F, scale)}, OV3),
                ("p", "ND_add_vector3FA", {"in1": (V3, "<ps>"), "in2": (F, offset)}, OV3)]

    ramp = ("ramp", "ND_ramp4_color3", {"valuetl": (C3, (0.9, 0.1, 0.1)), "valuetr": (C3, (0.1, 0.8, 0.2)),
                                          "valuebl": (C3, (0.1, 0.2, 0.9)), "valuebr": (C3, (0.9, 0.9, 0.2))}, O3)
    noise_c = ("nz", "ND_noise2d_color3", {"texcoord": (V2, "<tc>"), "amplitude": (V3, (0.5, 0.5, 0.5)),
                                           "pivot": (F, 0.5)}, O3)

    def f2c(src):  # float -> color3
        return ("out", "ND_convert_float_color3", {"in": (F, f"<{src}>")}, O3)

    def v2c(src):  # vector3 -> color3
        return ("out", "ND_convert_vector3_color3", {"in": (V3, f"<{src}>")}, O3)

    tiles = {
        "noise2d": [st, tc4, ("out", "ND_noise2d_color3", {"texcoord": (V2, "<tc>"), "amplitude": (V3, (0.5, 0.5, 0.5)),
                                                           "pivot": (F, 0.5)}, O3)],
        "noise3d": scaled_pos(3.0) + [("out", "ND_noise3d_color3", {"position": (V3, "<p>"),
                                                                     "amplitude": (V3, (0.5, 0.5, 0.5)),
                                                                     "pivot": (F, 0.5)}, O3)],
        "noise3d_f": scaled_pos(5.0) + [("n", "ND_noise3d_float", {"position": (V3, "<p>"), "pivot": (F, 0.5)}, OF),
                                         f2c("n")],
        "fractal3d": scaled_pos(2.0) + [("f", "ND_fractal3d_color3", {"position": (V3, "<p>"), "octaves": ("int", 4),
                                                                       "amplitude": (V3, (0.5, 0.5, 0.5))}, O3),
                                         ("out", "ND_add_color3FA", {"in1": (C3, "<f>"), "in2": (F, 0.5)}, O3)],
        "fractal2d": [st, tc4, ("f", "ND_fractal2d_color3", {"texcoord": (V2, "<tc>"), "octaves": ("int", 3),
                                                              "lacunarity": (F, 2.2), "diminish": (F, 0.6)}, O3),
                      ("out", "ND_add_color3FA", {"in1": (C3, "<f>"), "in2": (F, 0.5)}, O3)],
        "cell2d": [st, tc4, ("c", "ND_cellnoise2d_float", {"texcoord": (V2, "<tc>")}, OF), f2c("c")],
        "cell3d": scaled_pos(4.0) + [("c", "ND_cellnoise3d_float", {"position": (V3, "<p>")}, OF), f2c("c")],
        "worley2d": [st, tc4, ("w", "ND_worleynoise2d_float", {"texcoord": (V2, "<tc>"), "jitter": (F, 1.0)}, OF),
                     f2c("w")],
        "worley2d_v3": [st, tc4, ("w", "ND_worleynoise2d_vector3", {"texcoord": (V2, "<tc>"),
                                                                     "jitter": (F, 0.8)}, OV3), v2c("w")],
        "worley3d_v3": scaled_pos(3.0) + [("w", "ND_worleynoise3d_vector3", {"position": (V3, "<p>"),
                                                                              "jitter": (F, 0.75)}, OV3),
                                           v2c("w")],
        "unified2d": [st, ("u", "ND_unifiednoise2d_float", {"texcoord": (V2, "<st>"), "type": ("int", 3),
                                                            "freq": (V2, (6.0, 6.0)), "octaves": ("int", 3)}, OF),
                      f2c("u")],
        "unified3d": [pos, ("u", "ND_unifiednoise3d_float", {"position": (V3, "<pos>"), "type": ("int", 2),
                                                             "freq": (V3, (5.0, 5.0, 5.0)),
                                                             "offset": (V3, (0.1, 0.1, 0.5))}, OF), f2c("u")],
        "hsvtorgb": [st, ("sep", "ND_separate2_vector2", {"in": (V2, "<st>")}, {"outx": F, "outy": F}),
                     ("hsv", "ND_combine3_color3", {"in1": (F, "<sep.outx>"), "in2": (F, 0.8),
                                                    "in3": (F, "<sep.outy>")}, O3),
                     ("out", "ND_hsvtorgb_color3", {"in": (C3, "<hsv>")}, O3)],
        # hue is ill-conditioned near gray: saturated input colors
        "rgbtohsv": [st, tc4, noise_c, ramp,
                     ("mx", "ND_mix_color3", {"fg": (C3, "<nz>"), "bg": (C3, "<ramp>"), "mix": (F, 0.3)}, O3),
                     ("out", "ND_rgbtohsv_color3", {"in": (C3, "<mx>")}, O3)],
        "hsvadjust": [ramp, ("out", "ND_hsvadjust_color3", {"in": (C3, "<ramp>"),
                                                            "amount": (V3, (0.3, 1.2, 0.9))}, O3)],
        "colorcorrect": [ramp, ("out", "ND_colorcorrect_color3", {
            "in": (C3, "<ramp>"), "hue": (F, 0.1), "saturation": (F, 1.5), "gamma": (F, 1.2), "lift": (F, 0.05),
            "gain": (F, 1.1), "contrast": (F, 1.2), "contrastpivot": (F, 0.5), "exposure": (F, 0.5)}, O3)],
        "checker": [("out", "ND_checkerboard_color3", {"color1": (C3, (0.9, 0.8, 0.1)), "color2": (C3, (0.1, 0.2, 0.6)),
                                                       "uvtiling": (V2, (4.0, 4.0))}, O3)],
        "hexagons": [("out", "ND_tiledhexagons_color3", {"uvtiling": (V2, (3.0, 3.0)), "size": (F, 0.8)}, O3)],
        "grid": [("out", "ND_grid_color3", {"uvtiling": (V2, (2.0, 2.0)), "thickness": (F, 0.1)}, O3)],
        "place_rotate": [st, ("pl", "ND_place2d_vector2", {"texcoord": (V2, "<st>"), "pivot": (V2, (0.5, 0.5)),
                                                           "scale": (V2, (0.7, 0.5)), "rotate": (F, 30.0),
                                                           "offset": (V2, (0.1, 0.0))}, OV2),
                         ("ro", "ND_rotate2d_vector2", {"in": (V2, "<pl>"), "amount": (F, 15.0)}, OV2),
                         ("v3", "ND_convert_vector2_vector3", {"in": (V2, "<ro>")}, OV3),
                         ("fr", "ND_modulo_vector3FA", {"in1": (V3, "<v3>"), "in2": (F, 0.5)}, OV3),
                         ("sc", "ND_multiply_vector3FA", {"in1": (V3, "<fr>"), "in2": (F, 2.0)}, OV3),
                         v2c("sc")],
        "blend": [st, tc4, noise_c, ramp,
                  ("sc", "ND_screen_color3", {"fg": (C3, "<nz>"), "bg": (C3, "<ramp>"), "mix": (F, 0.8)}, O3),
                  ("bu", "ND_burn_color3", {"fg": (C3, "<nz>"), "bg": (C3, "<sc>"), "mix": (F, 0.5)}, O3),
                  ("out", "ND_dodge_color3", {"fg": (C3, "<ramp>"), "bg": (C3, "<bu>"), "mix": (F, 0.3)}, O3)],
        "over": [st, tc4, ("nf", "ND_noise2d_float", {"texcoord": (V2, "<tc>"), "pivot": (F, 0.5)}, OF),
                 ("fg", "ND_combine4_color4", {"in1": (F, 0.9), "in2": (F, 0.4), "in3": (F, 0.1),
                                               "in4": (F, "<nf>")}, O4),
                 ("ov", "ND_over_color4", {"fg": (C4, "<fg>"), "bg": (C4, (0.1, 0.3, 0.6, 1.0)),
                                           "mix": (F, 0.9)}, O4),
                 ("out", "ND_convert_color4_color3", {"in": (C4, "<ov>")}, O3)],
        "adjust": [st, tc4, noise_c,
                   ("co", "ND_contrast_color3", {"in": (C3, "<nz>"), "amount": (C3, (1.5, 1.5, 1.5)),
                                                 "pivot": (C3, (0.5, 0.5, 0.5))}, O3),
                   ("ra", "ND_range_color3", {"in": (C3, "<co>"), "inlow": (C3, (0.1, 0.1, 0.1)),
                                              "inhigh": (C3, (0.9, 0.9, 0.9)), "gamma": (C3, (1.4, 1.4, 1.4)),
                                              "outlow": (C3, (0.05, 0.05, 0.05)), "outhigh": (C3, (0.95, 0.95, 0.95)),
                                              "doclamp": ("bool", True)}, O3),
                   ("out", "ND_safepower_color3", {"in1": (C3, "<ra>"), "in2": (C3, (0.8, 1.2, 2.0))}, O3)],
        "hmtlx": [("ch", "ND_hmtlxcheckers2d_float", {"freq": (V2, (3.0, 3.0))}, OF),
                  ("ci", "ND_hmtlxcircles2d_float", {"freq": (V2, (2.0, 2.0)), "scale": (F, 0.8)}, OF),
                  ("out", "ND_combine3_color3", {"in1": (F, "<ch>"), "in2": (F, "<ci>"), "in3": (F, 0.5)}, O3)],
    }
    mats, prims = [], []
    for k, (name, nodes) in enumerate(tiles.items()):
        mats.append(node_material(f"n_{name}", nodes))
        prims.append(tile(f"tile_{name}", k % 6, 3 - k // 6, f"n_{name}"))
    b = looks(mats) + "".join(prims)
    b += """    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 60
        float verticalAperture = 40
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (3, 2, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
"""
    return scene(b, (192, 128), 1, settings_extra=(
        '\n        token nova:pixelFilter = "point"'
        '\n        string karma:global:pixelfilter = "box"'
        "\n        float karma:global:pixelfilterscale = 1e-06"))


def mtl_textured():
    """Textures through both renderers (M3.8, M3.9), direct light only: a UDIM
    base color (tiles 1001, 1002) on an upright card, a raw roughness map on
    an OpenPBR metal sphere, a
    linear, MIP-mapped checker tiled across a ground plane that recedes to
    grazing angles (MIP selection from ray cones), lit by the soft sky and a
    rect. (Karma filters 8-bit sRGB .tx files in their encoded space, which
    darkens minified high-contrast textures, about -30 % here; Nova decodes
    before filtering. So the checker is linear.)"""
    SS = "ND_standard_surface_surfaceshader"
    C3, F, V2 = "color3f", "float", "float2"
    mats = [
        network_material("udim", SS, {"base_color": (C3, "<img>"), "specular_roughness": (F, 0.5)},
                         [("img", "ND_image_color3", {"file": ("asset", "@./assets/udim_base.<UDIM>.png@")},
                           {"out": C3})]),
        network_material("rough_metal", "ND_open_pbr_surface_surfaceshader",
                         {"base_color": (C3, (0.95, 0.64, 0.54)), "base_metalness": (F, 1.0),
                          "specular_roughness": (F, "<rough>")},
                         [("rough", "ND_image_float", {"file": ("asset", "@./assets/rough.png@")}, {"out": F})]),
        # The same raw map as a diffuse weight: read exactly like Karma.
        network_material("raw_diffuse", SS, {"base": (F, "<map>"), "base_color": (C3, (1.0, 1.0, 1.0)),
                                             "specular": (F, 0.0)},
                         [("map", "ND_image_float", {"file": ("asset", "@./assets/rough.png@")}, {"out": F})]),
        network_material("ground", SS, {"base_color": (C3, "<chk>"), "specular": (F, 0.0)},
                         [("chk", "ND_tiledimage_color3", {"file": ("asset", "@./assets/checker_lin.exr@"),
                                                           "uvtiling": (V2, (8.0, 32.0))}, {"out": C3})]),
    ]
    b = looks(mats)

    def card(name, corners, uvs, material):
        return (f'    def Mesh "{name}" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )\n    {{\n'
                "        int[] faceVertexCounts = [4]\n        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                f"        point3f[] points = [{', '.join(f3(c) for c in corners)}]\n"
                f"        texCoord2f[] primvars:st = [{', '.join(f3(u) for u in uvs)}] (\n"
                '            interpolation = "vertex"\n        )\n'
                '        uniform token subdivisionScheme = "none"\n        uniform bool doubleSided = 1\n'
                f"        rel material:binding = </World/Looks/{material}>\n    }}\n")
    b += card("ground", [(-4, 0, 2), (4, 0, 2), (4, 0, -30), (-4, 0, -30)], [(0, 0), (1, 0), (1, 1), (0, 1)], "ground")
    b += card("udim_card", [(-2.3, 0.15, -0.5), (-0.7, 0.15, -0.5), (-0.7, 0.95, -0.5), (-2.3, 0.95, -0.5)],
              [(0, 0), (2, 0), (2, 1), (0, 1)], "udim")
    b += card("raw_card", [(-0.45, 0.1, -1.2), (0.35, 0.1, -1.2), (0.35, 0.6, -1.2), (-0.45, 0.6, -1.2)],
              [(0, 0), (1, 0), (1, 1), (0, 1)], "raw_diffuse")
    b += ('    def Mesh "ball" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n'
          '        prepend references = @./assets/sphere_st.usda@\n    )\n    {\n'
          "        rel material:binding = </World/Looks/rough_metal>\n"
          "        double3 xformOp:translate = (1.2, 0.55, -0.5)\n"
          "        double3 xformOp:scale = (0.5, 0.5, 0.5)\n"
          '        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n    }\n')
    b += soft_sky()
    b += light("RectLight", "key", ["float inputs:intensity = 4", "float inputs:width = 2", "float inputs:height = 2"],
               translate=(2.0, 3.0, 2.0), rotate=(-50, 35, 0))
    b += camera((0, 1.0, 3.2), (-9, 0, 0), focal=35, aperture=(36, 18))
    return scene(b, (192, 96), 1, settings_extra="\n        float karma:object:reflectlimit = 0"
                                                 "\n        float karma:object:refractlimit = 0")


def mtl_normals():
    """Normals and tangents against Karma (M3.10), direct light only: a
    normal-mapped sphere (an image of tilted checks, raw), a card bumped by
    noise (Karma's world-space heighttonormal), two anisotropic metal cards
    whose st are swapped (the tangent frame follows st) and one rotated by
    45 degrees (specular_rotation 0.125: the rotation's direction)."""
    SS = "ND_standard_surface_surfaceshader"
    C3, F, V2, V3 = "color3f", "float", "float2", "float3"
    mats = [
        network_material("nmap", SS, {"base_color": (C3, (0.7, 0.7, 0.7)), "specular": (F, 0.0),
                                      "normal": (V3, "<nm>")},
                         [("img", "ND_image_vector3", {"file": ("asset", "@./assets/normal_checks.png@")}, {"out": V3}),
                          ("nm", "ND_normalmap_float", {"in": (V3, "<img>")}, {"out": V3})]),
        network_material("bumped", SS, {"base_color": (C3, (0.6, 0.65, 0.7)), "specular": (F, 0.0),
                                        "normal": (V3, "<bmp>")},
                         [("st", "ND_texcoord_vector2", {}, {"out": V2}),
                          ("tc", "ND_multiply_vector2FA", {"in1": (V2, "<st>"), "in2": (F, 6.0)}, {"out": V2}),
                          ("h", "ND_noise2d_float", {"texcoord": (V2, "<tc>"), "amplitude": (F, 0.5)}, {"out": F}),
                          ("bmp", "ND_bump_vector3", {"height": (F, "<h>"), "scale": (F, 0.08)}, {"out": V3})]),
        network_material("aniso", SS, {"base_color": (C3, (0.9, 0.9, 0.9)), "metalness": (F, 1.0),
                                       "specular_roughness": (F, 0.35), "specular_anisotropy": (F, 0.8)}, []),
        network_material("aniso_rot", SS, {"base_color": (C3, (0.9, 0.9, 0.9)), "metalness": (F, 1.0),
                                           "specular_roughness": (F, 0.35), "specular_anisotropy": (F, 0.8),
                                           "specular_rotation": (F, 0.125)}, []),
    ]
    b = looks(mats)

    def card(name, corners, uvs, material):
        return (f'    def Mesh "{name}" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )\n    {{\n'
                "        int[] faceVertexCounts = [4]\n        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                f"        point3f[] points = [{', '.join(f3(c) for c in corners)}]\n"
                f"        texCoord2f[] primvars:st = [{', '.join(f3(u) for u in uvs)}] (\n"
                '            interpolation = "vertex"\n        )\n'
                '        uniform token subdivisionScheme = "none"\n        uniform bool doubleSided = 1\n'
                f"        rel material:binding = </World/Looks/{material}>\n    }}\n")
    b += card("ground", [(-6, 0, 3), (6, 0, 3), (6, 0, -6), (-6, 0, -6)], [(0, 0), (1, 0), (1, 1), (0, 1)], "bumped")
    b += ('    def Mesh "ball" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n'
          '        prepend references = @./assets/sphere_st.usda@\n    )\n    {\n'
          "        rel material:binding = </World/Looks/nmap>\n"
          "        double3 xformOp:translate = (-1.4, 0.6, -0.5)\n"
          "        double3 xformOp:scale = (0.55, 0.55, 0.55)\n"
          '        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n    }\n')
    b += card("aniso_st", [(0.2, 0.15, -0.6), (1.0, 0.15, -0.6), (1.0, 0.95, -0.6), (0.2, 0.95, -0.6)],
              [(0, 0), (1, 0), (1, 1), (0, 1)], "aniso")
    b += card("aniso_swapped", [(1.2, 0.15, -0.6), (2.0, 0.15, -0.6), (2.0, 0.95, -0.6), (1.2, 0.95, -0.6)],
              [(0, 0), (0, 1), (1, 1), (1, 0)], "aniso")
    b += card("aniso_rotated", [(-0.8, 0.15, -0.9), (0.0, 0.15, -0.9), (0.0, 0.95, -0.9), (-0.8, 0.95, -0.9)],
              [(0, 0), (1, 0), (1, 1), (0, 1)], "aniso_rot")
    b += soft_sky()
    b += light("RectLight", "key", ["float inputs:intensity = 6", "float inputs:width = 1", "float inputs:height = 1"],
               translate=(0.8, 2.2, 2.5), rotate=(-35, 10, 0))
    b += camera((0, 1.0, 3.2), (-9, 0, 0), focal=35, aperture=(36, 18))
    return scene(b, (192, 96), 1, settings_extra="\n        float karma:object:reflectlimit = 0"
                                                 "\n        float karma:object:refractlimit = 0")


TEXTURE_ASSETS = [  # (file, hoiiotool arguments, output flag)
    # disp_textured: a smooth height field (a blurred checker), linear float.
    # Blurred at three times the size and cut out of the middle, so it tiles
    # without a hard step at the wrap seam (--blur clamps at the border).
    ("height_bumps.exr", ["--pattern", "checker:width=32:height=32:color1=0:color2=1", "768x768", "1",
                          "--blur", "21x21", "--cut", "256x256+256+256", "--origin", "+0+0", "-d", "float"], "-o"),
    ("normal_checks.png", ["--pattern", "checker:width=16:height=16:color1=0.78,0.5,0.92:color2=0.5,0.22,0.92",
                           "128x128", "3", "-d", "uint8"], "-o"),
    ("udim_base.1001.png", ["--pattern", "checker:width=32:height=32:color1=0.85,0.45,0.1:color2=0.25,0.12,0.05",
                            "256x256", "3", "-d", "uint8"], "-o"),
    ("udim_base.1002.png", ["--pattern", "checker:width=32:height=32:color1=0.1,0.35,0.8:color2=0.05,0.1,0.25",
                            "256x256", "3", "-d", "uint8"], "-o"),
    ("rough.png", ["--pattern", "fill:topleft=0.1:topright=0.7:bottomleft=0.35:bottomright=0.95", "128x128", "1",
                   "-d", "uint8"], "-o"),
    ("checker_lin.exr", ["--pattern", "checker:width=4:height=4:color1=0.7,0.7,0.7:color2=0.014,0.014,0.014",
                         "256x256", "3", "-d", "half"], "-otex"),
]


def quat_y(deg):
    h = math.radians(deg) * 0.5
    return (math.cos(h), 0.0, math.sin(h), 0.0)


def instancer():
    b = quads("ground", [[(-20, 0, 20), (20, 0, 20), (20, 0, -20), (-20, 0, -20)]], (0.6, 0.6, 0.6))
    positions, orients, scales, protos = [], [], [], []
    k = 0
    for j in range(5):
        for i in range(6):
            x, z = -2.5 + i + 0.3 * math.sin(7.1 * k), -2.5 + j + 0.3 * math.cos(5.3 * k)
            s = 0.25 + 0.15 * (0.5 + 0.5 * math.sin(3.7 * k))
            proto = k % 3
            positions.append((x, s if proto == 0 else s * 0.5 if proto == 1 else s * 0.15, z))
            orients.append(quat_y(37.0 * k))
            scales.append((s, s, s) if proto == 0 else (s, s * (1.0 + 0.5 * (k % 2)), s))
            protos.append(proto)
            k += 1
    b += f"""    def PointInstancer "scatter"
    {{
        rel prototypes = [</World/scatter/protos/ball>, </World/scatter/protos/box>, </World/scatter/protos/slab>]
        int[] protoIndices = [{', '.join(map(str, protos))}]
        point3f[] positions = [{', '.join(f3(p) for p in positions)}]
        quath[] orientations = [{', '.join(f3(q, 5) for q in orients)}]
        float3[] scales = [{', '.join(f3(s) for s in scales)}]
        int64[] invisibleIds = [7]

        def Scope "protos"
        {{
{sphere("ball", (0, 0, 0), 1.0, (0.8, 0.3, 0.25), indent="            ")}{quads("box", box_quads((0, 0, 0), (1, 1, 1)), (0.3, 0.7, 0.35), indent="            ")}{quads("slab", box_quads((0, 0, 0), (2, 0.3, 1)), (0.3, 0.4, 0.8), indent="            ")}        }}
    }}
"""
    # Scene-graph instancing as well: instanceable references to a class.
    for n, t in enumerate([(-3.3, 0, 1.8), (3.2, 0, 1.6), (3.4, 0, -2.2)]):
        b += f"""    def Xform "pair{n}" (
        instanceable = true
        prepend references = </_pair>
    )
    {{
        {xform_ops(t, (0, 40 * n, 0))}
    }}
"""
    b += light("DistantLight", "sun", ["float inputs:intensity = 3", "float inputs:angle = 1",
                                       "bool inputs:normalize = 1"], rotate=(-45, 30, 0))
    b += light("DomeLight", "sky", ["float inputs:intensity = 0.4"])
    b += camera((0, 4.2, 6.5), (-32, 0, 0))
    pair = quads("a", box_quads((0, 0.25, 0), (0.5, 0.5, 0.5)), (0.8, 0.8, 0.2), indent="    ")
    pair += quads("b", box_quads((0.6, 0.15, 0.2), (0.3, 0.3, 0.3)), (0.2, 0.8, 0.8), indent="    ")
    cls = f'class Xform "_pair"\n{{\n{pair}}}\n\n'
    return scene(b, (160, 96), 3).replace('def Xform "World"', cls + 'def Xform "World"', 1)


CUBE_POINTS = [(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (0.5, 0.5, 0.5),
               (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5)]
CUBE_FACES = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]


def subdiv(karma=False):
    def cube(name, offset, color, extra=""):
        pts = [(p[0] + offset[0], p[1] + offset[1], p[2] + offset[2]) for p in CUBE_POINTS]
        return mesh(name, [4] * 6, CUBE_FACES, pts, color, scheme="catmullClark", extra=extra)

    b = quads("ground", [[(-20, 0, 20), (20, 0, 20), (20, 0, -20), (-20, 0, -20)]], (0.6, 0.6, 0.6))
    b += cube("smooth", (-1.6, 0.5, 0), (0.8, 0.35, 0.25))
    if not karma:
        # Infinitely sharp creases around the top face (edges 2-3, 3-5, 5-4, 4-2).
        b += cube("creased", (0, 0.5, -0.3), (0.75, 0.75, 0.75),
                  "int[] creaseIndices = [2, 3, 3, 5, 5, 4, 4, 2]\nint[] creaseLengths = [2, 2, 2, 2]\n"
                  "float[] creaseSharpnesses = [10, 10, 10, 10]")
    else:
        # Karma shades infinitely sharp creases unlike the OpenSubdiv limit
        # surface (tests/reference/README.md), so its reference renders the
        # same limit surface in split form: the open box and the top quad,
        # both with edgeOnly boundaries (the crease rules are the boundary
        # rules).
        pts = [(p[0], p[1] + 0.5, p[2] - 0.3) for p in CUBE_POINTS]
        edge_only = 'uniform token interpolateBoundary = "edgeOnly"'
        b += mesh("creased", [4] * 5, CUBE_FACES[0:4] + CUBE_FACES[8:24], pts, (0.75, 0.75, 0.75),
                  scheme="catmullClark", extra=edge_only)
        b += mesh("creasedTop", [4], [0, 1, 2, 3], [pts[i] for i in (2, 3, 5, 4)], (0.75, 0.75, 0.75),
                  scheme="catmullClark", extra=edge_only)
    b += cube("cornered", (1.6, 0.5, 0), (0.25, 0.4, 0.8),
              "int[] cornerIndices = [0, 3, 5, 6]\nfloat[] cornerSharpnesses = [10, 10, 10, 10]")
    b += light("DomeLight", "sky", ["float inputs:intensity = 0.5"])
    b += light("RectLight", "key", ["float inputs:intensity = 8", "float inputs:width = 2", "float inputs:height = 1"],
               translate=(1.0, 3.0, 2.0), rotate=(-60, 25, 0))
    b += camera((0, 1.6, 4.5), (-14, 0, 0))
    # Karma dices the limit surface finely at render complexity "veryhigh";
    # Nova's default there is level 3 (8 x 8 quads per face), whose chords cut
    # the cone tips of "cornered" by a few percent. Level 4 compares the limit
    # surfaces themselves (the default level is checked in tests/husk/check_subdiv.py).
    return scene(b, (160, 96), 3, settings_extra="\n        int nova:subdivLevel = 4")


def dof():
    """Centimetres (metersPerUnit 0.01) so the lens attributes read as mm:
    focalLength 50 = 5 cm. Checker cards at 100 (left), 300 (in focus) and
    900 cm (right); f/1.4 gives circles of confusion of about 6.5 and 2.2 px."""
    b = ""
    for n, (dist, angle) in enumerate([(100, -14.0), (300, 0.0), (900, 14.0)]):
        size, cx = 0.3 * dist, dist * math.tan(math.radians(angle))
        cells, cols = 8, []
        pts, idx = [], []
        for j in range(cells):
            for i in range(cells):
                x0 = cx - size / 2 + size * i / cells
                y0 = -size / 2 + size * j / cells
                x1, y1 = x0 + size / cells, y0 + size / cells
                idx += [len(pts), len(pts) + 1, len(pts) + 2, len(pts) + 3]
                pts += [(x0, y0, -dist), (x1, y0, -dist), (x1, y1, -dist), (x0, y1, -dist)]
                cols.append((0.9, 0.9, 0.9) if (i + j) % 2 else (0.05, 0.05, 0.05))
        b += mesh(f"card{n}", [4] * cells * cells, idx, pts, (1, 1, 1)).replace(
            "color3f[] primvars:displayColor = [(1, 1, 1)]",
            f"color3f[] primvars:displayColor = [{', '.join(f3(c) for c in cols)}] (\n"
            '            interpolation = "uniform"\n        )')
    b += light("DomeLight", "sky", ["float inputs:intensity = 1"])
    b += camera((0, 0, 0), focal=50, aperture=(36, 21.6),
                extra="\n        float fStop = 1.4\n        float focusDistance = 300")
    return scene(b, (192, 112), 2, mpu=0.01, settings_extra="\n        bool karma:global:enable_dof = 1")


def motion_blur():
    """Frame 1, shutter [-0.25, 0.25] frames: a translating box, a rotating
    box (transform blur) and a deforming quad (deformation blur)."""
    b = quads("ground", [[(-20, 0, 20), (20, 0, 20), (20, 0, -20), (-20, 0, -20)]], (0.6, 0.6, 0.6))
    b += quads("mover", box_quads((0, 0, 0), (0.6, 0.6, 0.6)), (0.8, 0.35, 0.25)).replace(
        "    rel material:binding = </Looks/displayColorLambert>\n",
        "    rel material:binding = </Looks/displayColorLambert>\n"
        "    double3 xformOp:translate.timeSamples = {\n        0: (-2.2, 0.3, 0),\n        1: (-1.4, 0.3, 0),\n"
        "        2: (-0.6, 0.3, 0),\n    }\n    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n")
    b += quads("spinner", box_quads((0, 0, 0), (1.2, 0.3, 0.3)), (0.3, 0.7, 0.35)).replace(
        "    rel material:binding = </Looks/displayColorLambert>\n",
        "    rel material:binding = </Looks/displayColorLambert>\n"
        "    double3 xformOp:translate = (0.4, 0.8, 0)\n"
        "    float xformOp:rotateY.timeSamples = {\n        0: -60,\n        1: 0,\n        2: 60,\n    }\n"
        "    uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateY\"]\n")
    deform = [[(1.6, 0.2, 0), (2.4, 0.2, 0), (2.4, 1.0, 0), (1.6, 1.0, 0)],
              [(1.6, 0.2, 0), (2.4, 0.2, 0), (2.4, 1.8, 0), (1.6, 1.0, 0)],
              [(1.6, 0.2, 0), (2.4, 0.2, 0), (2.4, 2.6, 0), (1.6, 1.0, 0)]]
    samples = ",\n        ".join(f"{t}: [{', '.join(f3(p) for p in q)}]" for t, q in enumerate(deform))
    b += mesh("stretch", [4], [0, 1, 2, 3], deform[1], (0.3, 0.4, 0.8)).replace(
        f"    point3f[] points = [{', '.join(f3(p) for p in deform[1])}]",
        f"    point3f[] points.timeSamples = {{\n        {samples},\n    }}")
    b += light("DistantLight", "sun", ["float inputs:intensity = 2.5", "float inputs:angle = 1",
                                       "bool inputs:normalize = 1"], rotate=(-50, 20, 0))
    b += light("DomeLight", "sky", ["float inputs:intensity = 0.4"])
    b += camera((0, 1.2, 5), (-8, 0, 0), extra="\n        double shutter:open = -0.25\n        double shutter:close = 0.25")
    extra = ("\n        bool disableMotionBlur = 0\n        int karma:object:xformsamples = 2"
             "\n        int karma:object:geosamples = 2")
    return scene(b, (160, 96), 2, header_extra="    startTimeCode = 1\n    endTimeCode = 1\n"
                 "    timeCodesPerSecond = 24\n    framesPerSecond = 24\n", settings_extra=extra)


def motion_velocity():
    """Velocity and acceleration blur (docs/design/M8.md D19) at frame 1,
    shutter [-0.25, 0.25] frames, 24 time codes per second, nothing
    time-sampled: a box moving by its velocities, a quad on an accelerated
    arc (Karma's 4 geometry samples), points, curves and the instances of a
    point instancer moving and turning by velocities and angular velocities.
    Karma's settings drive both renderers; the box also sets Nova's own."""
    vblur = lambda k: f'    int primvars:karma:object:vblur = {k} (interpolation = "constant")\n'

    def add(text, extra):
        return text.replace("    rel material:binding = </Looks/displayColorLambert>\n",
                            "    rel material:binding = </Looks/displayColorLambert>\n" + extra)

    def vec(v, n):
        return "[" + ", ".join([f3(v)] * n) + "]"

    b = quads("ground", [[(-20, 0, 20), (20, 0, 20), (20, 0, -20), (-20, 0, -20)]], (0.6, 0.6, 0.6))
    b += add(quads("box", box_quads((-1.4, 0.3, 0), (0.6, 0.6, 0.6)), (0.8, 0.35, 0.25)),
             f"    vector3f[] velocities = {vec((19.2, 0, 0), 24)}\n" + vblur(1) +
             '    int primvars:nova:object:velocityBlur = 1 (interpolation = "constant")\n')
    # Left edge: 0.25 u in x and 0.25 u - 0.5 u^2 in y (u = -1..1 over the shutter).
    b += add(quads("arc", [[(-0.3, 1.2, 0), (0.1, 1.2, 0), (0.1, 1.6, 0), (-0.3, 1.6, 0)]], (0.3, 0.4, 0.8)),
             f"    vector3f[] velocities = {vec((24, 24, 0), 4)}\n"
             f"    vector3f[] accelerations = {vec((0, -9216, 0), 4)}\n" + vblur(2) +
             '    int primvars:karma:object:geosamples = 4 (interpolation = "constant")\n')
    pts = [(-0.6 + 0.35 * i, 0.25 + 0.3 * (i % 3), 0.4 - 0.2 * (i % 2)) for i in range(6)]
    vel = [(19.2 * math.cos(1.1 * i), 19.2 * math.sin(1.1 * i), 0) for i in range(6)]
    b += f"""    def Points "particles" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {{
        point3f[] points = [{', '.join(f3(p) for p in pts)}]
        float[] widths = [{', '.join(['0.18'] * 6)}] (interpolation = "vertex")
        vector3f[] velocities = [{', '.join(f3(v) for v in vel)}]
        color3f[] primvars:displayColor = [(0.85, 0.75, 0.3)]
        rel material:binding = </Looks/displayColorLambert>
    {vblur(1)}    }}
"""
    strands = [[(1.6 + 0.12 * i, 0.0, -0.2), (1.6 + 0.12 * i, 0.9 + 0.1 * (i % 2), -0.2)] for i in range(5)]
    b += f"""    def BasisCurves "strands" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {{
        uniform token type = "linear"
        int[] curveVertexCounts = [{', '.join(['2'] * 5)}]
        point3f[] points = [{', '.join(f3(p) for s in strands for p in s)}]
        float[] widths = [{', '.join(['0.05'] * 10)}] (interpolation = "vertex")
        vector3f[] velocities = [{', '.join(f3((0, 0, 0)) + ', ' + f3((24, 0, 0)) for s in strands)}]
        color3f[] primvars:displayColor = [(0.3, 0.7, 0.35)]
        rel material:binding = </Looks/displayColorLambert>
    {vblur(1)}    }}
"""
    b += f"""    def PointInstancer "spinners"
    {{
        rel prototypes = [</World/spinners/protos/slab>]
        int[] protoIndices = [0, 0, 0]
        point3f[] positions = [(-2.0, 1.5, -0.5), (0.9, 1.65, -0.5), (1.9, 1.5, -0.5)]
        vector3f[] velocities = [(0, -14.4, 0), (9.6, 0, 0), (0, 0, 0)]
        vector3f[] angularVelocities = [(0, 0, 2880), (0, 2160, 0), (1440, 0, 2880)]
        int primvars:karma:object:instance_vblur = 1 (interpolation = "constant")

        def Scope "protos"
        {{
{quads("slab", box_quads((0, 0, 0), (0.6, 0.15, 0.3)), (0.75, 0.3, 0.7), indent="            ")}        }}
    }}
"""
    b += light("DistantLight", "sun", ["float inputs:intensity = 2.5", "float inputs:angle = 1",
                                       "bool inputs:normalize = 1"], rotate=(-50, 20, 0))
    b += light("DomeLight", "sky", ["float inputs:intensity = 0.4"])
    b += camera((0, 1.2, 5), (-8, 0, 0), extra="\n        double shutter:open = -0.25\n        double shutter:close = 0.25")
    extra = "\n        bool disableMotionBlur = 0"
    return scene(b, (160, 96), 2, header_extra="    startTimeCode = 1\n    endTimeCode = 1\n"
                 "    timeCodesPerSecond = 24\n    framesPerSecond = 24\n", settings_extra=extra)


SKY_SOFT = ["--create", "256x128", "3",
            "--fill:top=0.25,0.42,0.85:bottom=0.65,0.72,0.85", "256x64+0+0",
            "--fill:top=0.20,0.17,0.14:bottom=0.08,0.07,0.06", "256x64+0+64",
            "--create", "256x128", "3", "--fill:color=150,140,125", "6x6+93+36", "--blur", "11x11",
            "--add", "-d", "half", "--compression", "zip"]
RECT_PATTERN = ["--create", "64x32", "3",
                "--fill:color=1,0.2,0.2", "32x16+0+0", "--fill:color=0.2,1,0.2", "32x16+32+0",
                "--fill:color=0.2,0.2,1", "32x16+0+16", "--fill:color=1,1,0.2", "32x16+32+16",
                "--fill:color=4,4,4", "6x4+2+2", "-d", "half", "--compression", "zip"]


def mtl_graphs():
    """Generic MaterialX surfaces (M4.5) lit directly by the soft sky: pbrlib
    layer, mix and add, a sheen layer, a thin film on a conductor, a mix of
    two surface shaders, glTF PBR and Disney principled."""
    def surface(name, nodes, root):
        return network_material(name, "ND_surface", {"bsdf": ("token", f"<{root}>")}, nodes)

    def bsdf(node, nid, inputs):
        return (node, nid, inputs, {"out": "token"})

    gold_n, gold_k = (0.183, 0.421, 1.373), (3.424, 2.346, 1.77)
    mats = [
        surface("layer_diffuse", [
            bsdf("coat", "ND_dielectric_bsdf", {"ior": ("float", 1.5), "roughness": ("float2", (0.1, 0.1))}),
            bsdf("base", "ND_oren_nayar_diffuse_bsdf", {"color": ("color3f", (0.7, 0.3, 0.2))}),
            bsdf("layer", "ND_layer_bsdf", {"top": ("token", "<coat>"), "base": ("token", "<base>")})], "layer"),
        surface("mix_metal", [
            bsdf("metal", "ND_conductor_bsdf", {"ior": ("color3f", gold_n), "extinction": ("color3f", gold_k),
                                                "roughness": ("float2", (0.2, 0.2))}),
            bsdf("diffuse", "ND_oren_nayar_diffuse_bsdf", {"color": ("color3f", (0.5, 0.5, 0.5))}),
            bsdf("mix", "ND_mix_bsdf", {"fg": ("token", "<metal>"), "bg": ("token", "<diffuse>"),
                                        "mix": ("float", 0.5)})], "mix"),
        surface("add_diffuse", [
            bsdf("a", "ND_oren_nayar_diffuse_bsdf", {"color": ("color3f", (0.3, 0.3, 0.3))}),
            bsdf("b", "ND_oren_nayar_diffuse_bsdf", {"color": ("color3f", (0.25, 0.05, 0.05))}),
            bsdf("add", "ND_add_bsdf", {"in1": ("token", "<a>"), "in2": ("token", "<b>")})], "add"),
        surface("sheen_layer", [
            bsdf("fuzz", "ND_sheen_bsdf", {"color": ("color3f", (0.9, 0.8, 0.7)), "roughness": ("float", 0.5),
                                           "mode": ("string", "zeltner")}),
            bsdf("base", "ND_oren_nayar_diffuse_bsdf", {"color": ("color3f", (0.2, 0.3, 0.6))}),
            bsdf("layer", "ND_layer_bsdf", {"top": ("token", "<fuzz>"), "base": ("token", "<base>")})], "layer"),
        surface("film_metal", [
            bsdf("metal", "ND_conductor_bsdf", {"ior": ("color3f", gold_n), "extinction": ("color3f", gold_k),
                                                "roughness": ("float2", (0.15, 0.15)),
                                                "thinfilm_thickness": ("float", 400.0),
                                                "thinfilm_ior": ("float", 1.5)})], "metal"),
        network_material("surface_mix", "ND_mix_surfaceshader",
                         {"fg": ("token", "<ss>"), "bg": ("token", "<pbr>"), "mix": ("float", 0.25)},
                         [("ss", SS, {"base_color": ("color3f", (0.95, 0.64, 0.54)), "metalness": ("float", 1.0),
                                      "specular_roughness": ("float", 0.3)}, {"out": "token"}),
                          ("pbr", PBR, {}, {"out": "token"})]),
        mtlx_material("gltf", "ND_gltf_pbr_surfaceshader",
                      {"base_color": ("color3f", (0.8, 0.3, 0.2)), "metallic": ("float", 0.0),
                       "roughness": ("float", 0.4)}),
        mtlx_material("disney", "ND_disney_principled",
                      {"baseColor": ("color3f", (0.3, 0.5, 0.7)), "roughness": ("float", 0.5)}),
    ]
    names = ["layer_diffuse", "mix_metal", "add_diffuse", "sheen_layer", "film_metal", "surface_mix", "gltf",
             "disney"]
    b = looks(mats)
    for k, name in enumerate(names):
        b += mat_sphere(f"ball_{name}", (-1.5 + (k % 4), 0.55 if k < 4 else -0.55, 0.0), 0.42, name)
    b += soft_sky()
    b += camera((0, 0, 5.2), (0, 0, 0), focal=35, aperture=(36, 18))
    return scene(b, (160, 80), 1, settings_extra="\n        float karma:object:reflectlimit = 0"
                                                 "\n        float karma:object:refractlimit = 0")


def displaced_material(name, color, displacement_nodes, displacement_root):
    """A Standard Surface material with a displacement network (outputs:mtlx:displacement)."""
    base = f"/World/Looks/{name}"
    text = mtlx_material(name, SS, {"base_color": ("color3f", color), "specular_roughness": ("float", 0.4)})
    lines = text.rstrip("\n").split("\n")[:-1]  # without the closing brace
    lines.insert(2, f"            token outputs:mtlx:displacement.connect = <{base}/{displacement_root}.outputs:out>")
    for node, nid, inputs, outputs in displacement_nodes:
        lines += [f'            def Shader "{node}"', "            {", f'                uniform token info:id = "{nid}"']
        for key, (typ, value) in inputs.items():
            if isinstance(value, str) and value.startswith("<"):
                src, _, o = value[1:-1].partition(".")
                lines.append(f"                {typ} inputs:{key}.connect = <{base}/{src}.outputs:{o or 'out'}>")
            else:
                lines.append(f"                {typ} inputs:{key} = {usd_value(value)}")
        for out, typ in outputs.items():
            lines.append(f"                {typ} outputs:{out}")
        lines.append("            }")
    lines.append("        }")
    return "\n".join(lines) + "\n"


DICING = ("float primvars:karma:object:dicingquality = 1 (interpolation = \"constant\")\n"
          "float primvars:nova:dicingQuality = 1 (interpolation = \"constant\")")


def disp_textured():
    """Displacement (M4.6) against Karma's: a height map on a polygon plane and
    on a Catmull-Clark cube, a vector displacement on a polygon sphere, at
    dicing quality 1 in both renderers, under a grazing rect light and the sky."""
    height = [("tex", "ND_image_float", {"file": ("asset", "@./assets/height_bumps.exr@")}, {"out": "float"}),
              ("disp", "ND_displacement_float", {"displacement": ("float", "<tex>"), "scale": ("float", 0.08)},
               {"out": "token"})]
    vector = [("disp", "ND_displacement_vector3", {"displacement": ("vector3f", (0.03, 0.0, 0.06))},
               {"out": "token"})]
    b = looks([displaced_material("bumps", (0.6, 0.55, 0.5), height, "disp"),
               displaced_material("cube_bumps", (0.35, 0.45, 0.7), height, "disp"),
               displaced_material("vector", (0.7, 0.35, 0.3), vector, "disp")])
    # The plane: 2 x 2, st 0..1 (polygon, diced bilinearly).
    b += ('    def Mesh "plane" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )\n    {\n'
          "        int[] faceVertexCounts = [4]\n        int[] faceVertexIndices = [0, 1, 2, 3]\n"
          "        point3f[] points = [(-1.6, 0, 1), (0.4, 0, 1), (0.4, 0, -1), (-1.6, 0, -1)]\n"
          "        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (\n"
          '            interpolation = "vertex"\n        )\n'
          '        uniform token subdivisionScheme = "none"\n        uniform bool doubleSided = 1\n'
          f"        {DICING.replace(chr(10), chr(10) + '        ')}\n"
          "        rel material:binding = </World/Looks/bumps>\n    }\n")
    # The cube: a Catmull-Clark cage with st per face.
    st = "(0, 0), (1, 0), (1, 1), (0, 1)"
    b += ('    def Mesh "cube" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )\n    {\n'
          f"        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]\n"
          f"        int[] faceVertexIndices = [{', '.join(map(str, CUBE_FACES))}]\n"
          f"        point3f[] points = [{', '.join(f3((p[0] * 0.8 + 1.1, p[1] * 0.8 + 0.4, p[2] * 0.8)) for p in CUBE_POINTS)}]\n"
          f"        texCoord2f[] primvars:st = [{', '.join([st] * 6)}] (\n"
          '            interpolation = "faceVarying"\n        )\n'
          '        uniform token subdivisionScheme = "catmullClark"\n        uniform bool doubleSided = 1\n'
          f"        {DICING.replace(chr(10), chr(10) + '        ')}\n"
          "        rel material:binding = </World/Looks/cube_bumps>\n    }\n")
    # The sphere with st, vector displacement.
    b += ('    def Mesh "ball" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n'
          '        prepend references = @./assets/sphere_st.usda@\n    )\n    {\n'
          f"        {DICING.replace(chr(10), chr(10) + '        ')}\n"
          "        rel material:binding = </World/Looks/vector>\n"
          "        double3 xformOp:translate = (-0.6, 0.45, 0.2)\n"
          "        double3 xformOp:scale = (0.35, 0.35, 0.35)\n"
          '        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n    }\n')
    b += light("DomeLight", "sky", ["float inputs:intensity = 0.4"])
    b += light("RectLight", "key", ["float inputs:intensity = 12", "float inputs:width = 1.5", "float inputs:height = 0.6"],
               translate=(-3.0, 1.2, 0.5), rotate=(-15, -80, 0))
    b += camera((0, 2.2, 3.6), (-30, 0, 0))
    return scene(b, (160, 96), 2)


def limits_caustics():
    """Per-type limits and fake caustics (M4.1, M4.2) against Karma: two
    facing mirrors with reflect limit 2, and a glass sphere whose shadow on
    the floor is a fake caustic in both renderers (Karma's default fake
    caustics with causticsenable 0 on the glass; Nova's nova:object:fakeCaustics)."""
    glass = mtlx_material("glass", SS, {"base": ("float", 0.0), "transmission": ("float", 1.0),
                                         "specular_roughness": ("float", 0.0)})
    mirror = mtlx_material("mirror", SS, {"base_color": ("color3f", (0.9, 0.9, 0.9)), "metalness": ("float", 1.0),
                                          "specular_roughness": ("float", 0.0)})
    b = looks([glass, mirror])
    b += quads("floor", [[(-6, 0, 6), (6, 0, 6), (6, 0, -6), (-6, 0, -6)]], (0.6, 0.6, 0.6))
    b += ('    def Mesh "ball" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n'
          '        prepend references = @./assets/sphere.usda@\n    )\n    {\n'
          "        bool primvars:nova:object:fakeCaustics = 1 (interpolation = \"constant\")\n"
          "        bool primvars:karma:object:causticsenable = 0 (interpolation = \"constant\")\n"
          "        rel material:binding = </World/Looks/glass>\n"
          "        double3 xformOp:translate = (0.9, 0.55, 0.3)\n"
          "        double3 xformOp:scale = (0.5, 0.5, 0.5)\n"
          '        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n    }\n')
    for name, x, ry in (("mirrorL", -2.0, 55), ("mirrorR", -0.2, -55)):  # a V the camera looks into
        b += ('    def Mesh "' + name + '" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )\n    {\n'
              "        int[] faceVertexCounts = [4]\n        int[] faceVertexIndices = [0, 1, 2, 3]\n"
              "        point3f[] points = [(-0.8, 0, 0), (0.8, 0, 0), (0.8, 1.4, 0), (-0.8, 1.4, 0)]\n"
              '        uniform token subdivisionScheme = "none"\n        uniform bool doubleSided = 1\n'
              "        rel material:binding = </World/Looks/mirror>\n"
              f"        double3 xformOp:translate = ({x}, 0, -0.6)\n"
              f"        float3 xformOp:rotateXYZ = (0, {ry}, 0)\n"
              '        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]\n    }\n')
    b += sphere("orb", (-1.1, 0.3, -0.6), 0.3, (0.8, 0.3, 0.2))
    b += light("DomeLight", "sky", ["float inputs:intensity = 0.3"])
    b += light("RectLight", "key", ["float inputs:intensity = 1.2", "float inputs:width = 3", "float inputs:height = 3"],
               translate=(0.9, 4.0, 0.3), rotate=(-90, 0, 0))
    b += camera((0, 2.4, 4.2), (-25, 0, 0))
    return scene(b, (160, 96), 2, settings_extra="\n        float karma:object:reflectlimit = 2"
                                                 "\n        float karma:object:refractlimit = 4")


def vex_principled():
    """Houdini's Principled Shader 2.0 (M5.1, native in Nova) against Karma's
    VEX, direct light from the soft sky: plastic, metal, coat, sheen, a
    tinted half reflectivity, anisotropic metal, colour and roughness
    textures, and texture displacement (dicing quality 1 in both)."""
    F, V, I, S, A = "float", "vector3f", "int", "string", "asset"
    mats = [
        principled_material("p_plastic", {"basecolor": (V, (0.7, 0.1, 0.08)), "rough": (F, 0.25)}),
        principled_material("p_metal", {"basecolor": (V, (0.94, 0.78, 0.37)), "metallic": (F, 1.0),
                                        "rough": (F, 0.3)}),
        principled_material("p_coat", {"basecolor": (V, (0.1, 0.2, 0.6)), "rough": (F, 0.5), "coat": (F, 1.0),
                                       "coatrough": (F, 0.05)}),
        principled_material("p_sheen", {"basecolor": (V, (0.2, 0.05, 0.1)), "rough": (F, 0.6), "reflect": (F, 0.0),
                                        "sheen": (F, 1.0), "sheentint": (F, 0.5)}),
        principled_material("p_tint", {"basecolor": (V, (0.6, 0.3, 0.1)), "rough": (F, 0.2), "reflect": (F, 0.5),
                                       "reflecttint": (F, 1.0)}),
        principled_material("p_aniso", {"basecolor": (V, (0.9, 0.9, 0.9)), "metallic": (F, 1.0), "rough": (F, 0.4),
                                        "aniso": (F, 0.8), "anisodir": (F, 0.25)}),
        principled_material("p_tex", {"basecolor": (V, (1.0, 1.0, 1.0)), "basecolor_useTexture": (I, 1),
                                      "basecolor_texture": (A, "./assets/normal_checks.png"),
                                      "rough_useTexture": (I, 1), "rough_texture": (A, "./assets/rough.png"),
                                      "rough": (F, 1.0)}),
        principled_material("p_disp", {"basecolor": (V, (0.5, 0.6, 0.4)), "rough": (F, 0.4)},
                            displacement={"dispTex_enable": (I, 1),
                                          "dispTex_texture": (A, "./assets/height_bumps.exr"),
                                          "dispTex_scale": (F, 0.08)}),
    ]
    names = ["p_plastic", "p_metal", "p_coat", "p_sheen", "p_tint", "p_aniso", "p_tex", "p_disp"]
    b = looks(mats)
    for k, name in enumerate(names):
        center = (-1.5 + (k % 4), 0.55 if k < 4 else -0.55, 0.0)
        # p_aniso carries Houdini's uv primvar, which the Principled Shader's
        # tangents follow (without it Karma takes the polygons' parametric s, t).
        asset = {"p_tex": "sphere_st.usda", "p_disp": "sphere_st.usda", "p_aniso": "sphere_uv.usda"}.get(name, "sphere.usda")
        b += (f'    def Mesh "ball_{name}" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n'
              f'        prepend references = @./assets/{asset}@\n    )\n    {{\n'
              f"        {DICING.replace(chr(10), chr(10) + '        ')}\n"
              f"        rel material:binding = </World/Looks/{name}>\n"
              f"        double3 xformOp:translate = {f3(center)}\n"
              "        double3 xformOp:scale = (0.42, 0.42, 0.42)\n"
              '        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n    }\n')
    b += soft_sky()
    b += camera((0, 0, 5.2), (0, 0, 0), focal=35, aperture=(36, 18))
    return scene(b, (160, 80), 1, settings_extra="\n        float karma:object:reflectlimit = 0"
                                                 "\n        float karma:object:refractlimit = 0")


VB_PLASTIC = r'''#include "bsdf.h"

surface
vb_plastic(vector tint = {0.7, 0.3, 0.2}; float rough = 0.3; float freq = 3)
{
    vector nn = frontface(normalize(N), I);
    vector ng = frontface(normalize(Ng), I);
    vector base = tint * (0.4 + 0.6 * noise(P * freq));
    float energy = 0;
    bsdf spec = get_bsdf("ggx", ng, nn, normalize(I), normalize(dPds), normalize(dPdt), {1, 1, 1}, {1, 1, 1},
                         1.0 / 1.5, rough, 0, 0, 1, 0, 1, 1, 0, 0, energy, "reflect", "refract");
    F = 2.0 * diffuse(nn, "label", "diffuse") * base + spec;
}
'''

VB_METAL = r'''#include "bsdf.h"

surface
vb_metal(vector color = {0.95, 0.64, 0.54}; float rough = 0.25)
{
    vector nn = frontface(normalize(N), I);
    vector ng = frontface(normalize(Ng), I);
    float energy = 0;
    F = get_bsdf("ggx", ng, nn, normalize(I), normalize(dPds), normalize(dPdt), color, {1, 1, 1},
                 1.0 / 1.5, rough, 0, 0, 1, 0, 0, 1, 0, 0, energy, "reflect", "refract");
}
'''

VB_TEXTURE = r'''surface
vb_texture(string map = ""; vector2 st = {0, 0})
{
    vector nn = frontface(normalize(N), I);
    vector c = texture(map, st.x, st.y);
    F = 2.0 * diffuse(nn, "label", "diffuse") * c;
}
'''

VB_PRIMVAR = r'''surface
vb_primvar(vector tint = {1, 0, 1})
{
    vector nn = frontface(normalize(N), I);
    F = 2.0 * diffuse(nn, "label", "diffuse") * tint;
}
'''

VB_LAYERED = r'''#include <shaderlayer.h>

surface
vb_layered(vector tint = {0.2, 0.4, 0.8})
{
    ShaderLayer layer;
    init_layer(layer);
    vector nn = frontface(normalize(N), I);
    layer.F = 2.0 * diffuse(nn, "label", "diffuse") * tint;
    F = layer.F;
    Of = layer.Of;
}
'''

VB_ROUGHNOISE = r'''#include "bsdf.h"

surface
vb_roughnoise(vector color = {0.9, 0.9, 0.9}; float freq = 2)
{
    vector nn = frontface(normalize(N), I);
    vector ng = frontface(normalize(Ng), I);
    float rough = 0.1 + 0.5 * noise(P * freq);
    float energy = 0;
    F = get_bsdf("ggx", ng, nn, normalize(I), normalize(dPds), normalize(dPdt), color, {1, 1, 1},
                 1.0 / 1.5, rough, 0, 0, 1, 0, 0, 1, 0, 0, energy, "reflect", "refract");
}
'''

VB_DISPLACED = r'''surface
vb_displaced(vector tint = {0.6, 0.55, 0.4})
{
    vector nn = frontface(normalize(N), I);
    F = 2.0 * diffuse(nn, "label", "diffuse") * tint;
}
'''

VB_BUMPS = r'''displace
vb_bumps(float amp = 0.04; float freq = 3)
{
    P += normalize(N) * amp * (2 * noise(P * freq) - 1);
    N = computenormal(P);
}
'''


def vex_builder():
    """VEX Material Builder-style code through Nova's CVEX bridge (M5.4)
    against Karma CPU running the same code, direct light from the soft sky:
    noise-driven plastic, a Schlick metal, a texture through st, a parameter
    bound from a vertex primvar, init_layer, noise-driven roughness and a
    noise displacement (dicing quality 1 in both renderers). The code is
    Nova's own (Houdini-generated builder code is tested through hython)."""
    mats = [
        vex_material("vb_plastic", VB_PLASTIC),
        vex_material("vb_metal", VB_METAL),
        vex_material("vb_texture", VB_TEXTURE, {"map": ("asset", "./assets/normal_checks.png")}),
        vex_material("vb_primvar", VB_PRIMVAR),
        vex_material("vb_layered", VB_LAYERED),
        vex_material("vb_roughnoise", VB_ROUGHNOISE),
        vex_material("vb_displaced", VB_DISPLACED, displacement=VB_BUMPS, displacement_inputs={"amp": ("float", 0.05)}),
    ]
    names = ["vb_plastic", "vb_metal", "vb_texture", "vb_primvar", "vb_layered", "vb_roughnoise", "vb_displaced"]
    # The primvar sphere: tint per vertex of the 48 x 24 sphere (u, v, 0.5).
    nu, nv = 48, 24
    tints = ", ".join(f"({i / nu:.4g}, {j / nv:.4g}, 0.5)" for j in range(nv + 1) for i in range(nu + 1))
    b = looks(mats)
    for k, name in enumerate(names):
        center = (-1.5 + (k % 4), 0.55 if k < 4 else -0.55, 0.0)
        asset = "sphere_st.usda" if name == "vb_texture" else "sphere.usda"
        extra = (f'        color3f[] primvars:tint = [{tints}] (\n            interpolation = "vertex"\n        )\n'
                 if name == "vb_primvar" else "")
        if name == "vb_displaced":
            extra = f"        {DICING.replace(chr(10), chr(10) + '        ')}\n"
        b += (f'    def Mesh "ball_{name}" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n'
              f'        prepend references = @./assets/{asset}@\n    )\n    {{\n{extra}'
              f"        rel material:binding = </World/Looks/{name}>\n"
              f"        double3 xformOp:translate = {f3(center)}\n"
              "        double3 xformOp:scale = (0.42, 0.42, 0.42)\n"
              '        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n    }\n')
    b += soft_sky()
    b += camera((0, 0, 5.2), (0, 0, 0), focal=35, aperture=(36, 18))
    return scene(b, (160, 80), 1, settings_extra="\n        float karma:object:reflectlimit = 0"
                                                 "\n        float karma:object:refractlimit = 0")


# Karma's standard render vars (Karma Render Settings and Karma Standard
# Render Vars, docs/research/m6/karma-aovs.md) that aov_components compares:
# name -> light path expression. The beauty "C" comes first.
AOV_VARS = [
    ("C", "C.*[LO]"),
    ("C_key", "C.*<L.'key'>"),
    ("C_fill", "C.*<L.'fill'>"),
    ("C_emission", "C.*O"),
    ("beautyunshadowed", "unoccluded;C.*[LO]"),
    ("shadow", "shadow;C.*[LO]"),
    ("combineddiffuse", "C<RD>.*L"),
    ("directdiffuse", "C<RD>L"),
    ("indirectdiffuse", "C<RD>.+L"),
    ("directdiffuseunshadowed", "unoccluded;C<RD>L"),
    ("directdiffuseshadow", "shadow;C<RD>L"),
    ("combinedglossyreflection", "C<RG>.*L"),
    ("directglossyreflection", "C<RG>L"),
    ("indirectglossyreflection", "C<RG>.+L"),
    ("glossytransmission", "C<TG>.*L"),
    ("directemission", "CO"),
    ("indirectemission", "C.+O"),
    ("coat", "C<...'coat'>.*L"),
    ("metal_glossy", "C<RG'metal'>.*L"),
    ("albedo", "CDA"),
    ("albedoglossyreflection", "C<RG>A"),
    ("albedoglossytransmission", "C<TG>A"),
    ("albedotransmitteddiffuse", "C<TG>*DA"),
    ("alphatransmission", "!C<TG>*B"),
]


def aov_components():
    """Light path expression AOVs against Karma (M6.3): a coated plastic, a
    rough glass, a tagged metal and an emitter on a diffuse floor, lit by a
    tagged rect light ("key"), a tagged distant light ("fill") and an
    untagged constant dome; every var of AOV_VARS in one multi-part product."""
    mats = [
        mtlx_material("coat", SS, {"base_color": ("color3f", (0.7, 0.15, 0.1)), "specular_roughness": ("float", 0.35),
                                   "coat": ("float", 0.7), "coat_roughness": ("float", 0.1)}),
        mtlx_material("glass", SS, {"base": ("float", 0.0), "transmission": ("float", 1.0),
                                    "specular_roughness": ("float", 0.15)}),
        mtlx_material("metal", SS, {"base_color": ("color3f", (0.95, 0.8, 0.5)), "metalness": ("float", 1.0),
                                    "specular_roughness": ("float", 0.25)}),
        mtlx_material("emit", SS, {"base": ("float", 0.0), "specular": ("float", 0.0), "emission": ("float", 1.0),
                                   "emission_color": ("color3f", (4.0, 2.0, 0.8))}),
    ]
    b = looks(mats)
    b += quads("floor", [[(-6, 0, 6), (6, 0, 6), (6, 0, -6), (-6, 0, -6)]], (0.5, 0.5, 0.5))
    b += mat_sphere("ball_coat", (-1.5, 0.5, 0.0), 0.5, "coat")
    b += mat_sphere("ball_glass", (-0.35, 0.5, 0.45), 0.5, "glass")
    b += mat_sphere("ball_metal", (0.8, 0.5, 0.0), 0.5, "metal").replace(
        "        rel material:binding",
        '        string primvars:karma:object:lpetag = "metal" (interpolation = "constant")\n'
        "        rel material:binding", 1)
    b += mat_sphere("ball_emit", (1.75, 0.25, 0.65), 0.25, "emit")
    b += light("RectLight", "key", ["float inputs:intensity = 5", "float inputs:width = 2", "float inputs:height = 2",
                                    'string inputs:karma:light:lpetag = "key"'],
               translate=(0.4, 3.5, 1.2), rotate=(-90, 0, 0))
    b += light("DistantLight", "fill", ["float inputs:intensity = 0.8", "float inputs:angle = 5",
                                        'string inputs:karma:light:lpetag = "fill"'], rotate=(-40, 30, 0))
    b += light("DomeLight", "sky", ["float inputs:intensity = 0.25", "color3f inputs:color = (0.6, 0.7, 1)"])
    b += camera((0, 1.6, 4.6), (-15, 0, 0))
    var_paths = ",\n".join(f"                </Render/AovVars/{n}>" for n, _ in AOV_VARS)
    vars_text = ""
    for name, lpe in AOV_VARS:
        typ = "color4f" if name == "C" else "color3f"
        vars_text += f"""        def RenderVar "{name}"
        {{
            uniform token dataType = "{typ}"
            uniform string sourceName = "{lpe}"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "{typ}"
            string driver:parameters:aov:name = "{name}"
        }}
"""
    render_extra = f"""    def RenderProduct "aovs"
    {{
        token productName = "aov_components.exr"
        rel orderedVars = [
{var_paths}
        ]
    }}
    def Scope "AovVars"
    {{
{vars_text}    }}
"""
    return scene(b, (160, 90), 2, settings_extra="\n        rel products = [</Render/aovs>]",
                 render_extra=render_extra)


def main():
    out = sys.argv[1]
    hfs = sys.argv[sys.argv.index("--hfs") + 1] if "--hfs" in sys.argv else None
    os.makedirs(os.path.join(out, "assets"), exist_ok=True)
    files = {"assets/sphere.usda": sphere_asset(), "assets/sphere_st.usda": sphere_asset(st=True),
             "assets/sphere_uv.usda": sphere_asset(st=True, uv=True),
             "cornell_rect.usda": cornell_rect(), "dome_sky.usda": dome_sky(),
             "dome_plane.usda": dome_plane(), "furnace_rectbox.usda": furnace_rectbox(),
             "instancer.usda": instancer(), "subdiv.usda": subdiv(), "subdiv_karma.usda": subdiv(karma=True),
             "dof.usda": dof(),
             "motion_blur.usda": motion_blur(), "motion_velocity.usda": motion_velocity(),
             "mtl_spheres.usda": mtl_spheres(), "mtl_glass.usda": mtl_glass(),
             "furnace_materials.usda": furnace_materials(),
             "implicit_spheres.usda": implicit_spheres(),
             "implicit_spheres_karma.usda": implicit_spheres(karma=True), "mtl_nodes.usda": mtl_nodes(),
             "mtl_textured.usda": mtl_textured(), "mtl_normals.usda": mtl_normals(),
             "mtl_diffuse.usda": mtl_diffuse(), "mtl_kma_context.usda": mtl_kma_context(),
             "mtl_graphs.usda": mtl_graphs(), "disp_textured.usda": disp_textured(),
             "limits_caustics.usda": limits_caustics(), "vex_principled.usda": vex_principled(),
             "vex_builder.usda": vex_builder(), "aov_components.usda": aov_components()}
    files.update(light_scenes())
    for name, text in files.items():
        with open(os.path.join(out, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"{name:28s} {len(text):8d} bytes")
    if hfs:
        hoiiotool = os.path.join(hfs, "bin", "hoiiotool.exe")
        # Existing assets are kept: image files carry a creation time, and
        # the references hash their inputs (--force-assets rewrites them).
        assets = [("sky_soft.exr", SKY_SOFT, "-o"), ("rect_pattern.exr", RECT_PATTERN, "-o")] + TEXTURE_ASSETS
        for name, cmd, flag in assets:
            path = os.path.join(out, "assets", name)
            if os.path.exists(path) and "--force-assets" not in sys.argv:
                continue
            subprocess.run([hoiiotool, *cmd, flag, path], check=True)
            print(f"assets/{name}")


if __name__ == "__main__":
    main()
