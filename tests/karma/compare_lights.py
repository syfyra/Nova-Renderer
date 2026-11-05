"""Compares Nova's lighting against Karma CPU (the reference named in docs/PLAN.md).

For each light configuration a small USD scene is generated: a Lambertian
ground plane (albedo 0.5) seen from above, lit by the light under test. The
material is a MaterialX standard_surface without specular for Karma; Nova uses
primvars:displayColor until M3. Both renderers render direct light only
(Nova nova:maxDepth = 1, Karma diffuse limit 0), and the image averages are
compared with each other and with the analytic value. Karma's firefly clamp
(color limit, default 10) is disabled: it cuts BSDF samples that hit bright
dome texels and would bias the reference.

Usage:
    python compare_lights.py --hfs <Houdini dir> --stage <staged nova package> --out <dir> [--only name]

Needs a Karma license (husk with BRAY_HdKarma); not part of ctest.
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

ALBEDO = 0.5

SCENE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "World"
{{
    def Scope "materials"
    {{
        def Material "lambert"
        {{
            token outputs:mtlx:surface.connect = </World/materials/lambert/surface.outputs:out>

            def Shader "surface"
            {{
                uniform token info:id = "ND_standard_surface_surfaceshader"
                float inputs:base = 1
                color3f inputs:base_color = ({a}, {a}, {a})
                float inputs:specular = 0
                token outputs:out
            }}
        }}
    }}

    def Mesh "ground" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-100, 0, 100), (100, 0, 100), (100, 0, -100), (-100, 0, -100)]
        uniform token subdivisionScheme = "none"
        color3f[] primvars:displayColor = [({a}, {a}, {a})]
        rel material:binding = </World/materials/lambert>
    }}

{lights}

    def Camera "cam"
    {{
        float focalLength = 50
        float horizontalAperture = 20
        float verticalAperture = 20
        float2 clippingRange = (0.1, 1000)
        double3 xformOp:translate = (0, 10, 0)
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }}
}}

def Scope "Render"
{{
    def RenderSettings "settings"
    {{
        rel camera = </World/cam>
        int2 resolution = (64, 64)
        int nova:samples = {spp}
        int nova:maxDepth = 1
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = {spp}
        float karma:object:diffuselimit = 0
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
    }}
}}
"""


def distant(name, intensity, angle, normalize, tilt_deg, exposure=0.0, diffuse=1.0, color=(1, 1, 1),
            color_temperature=None):
    extra = ""
    if color_temperature:
        extra = f"""
        bool inputs:enableColorTemperature = 1
        float inputs:colorTemperature = {color_temperature}"""
    return f"""    def DistantLight "{name}"
    {{
        float inputs:intensity = {intensity}
        float inputs:exposure = {exposure}
        float inputs:angle = {angle}
        bool inputs:normalize = {int(normalize)}
        float inputs:diffuse = {diffuse}
        color3f inputs:color = ({color[0]}, {color[1]}, {color[2]}){extra}
        float3 xformOp:rotateXYZ = ({-90 + tilt_deg}, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }}"""


def dome(name, intensity, color=(1, 1, 1), exposure=0.0, texture=None, rotate=None):
    extra = ""
    if texture:
        # Karma's default MIS for textured domes is biased (up to -15% for
        # smooth gradients); misbias = -1 makes it converge to the exact value.
        extra += f"""
        asset inputs:texture:file = @{texture}@
        token inputs:texture:format = "latlong"
        float inputs:karma:light:misbias = -1"""
    if rotate:
        extra += "\n        " + _xform(None, rotate)
    return f"""    def DomeLight "{name}"
    {{
        float inputs:intensity = {intensity}
        float inputs:exposure = {exposure}
        color3f inputs:color = ({color[0]}, {color[1]}, {color[2]}){extra}
    }}"""


SKY_TEXTURE = "{sky}"  # replaced by the generated sky texture paths
DIM_SKY_TEXTURE = "{dimsky}"


SKY_SIZE = (256, 128)
SKY_COLOR = (0.3, 0.4, 0.5)
SUN_BLOCK = (100, 30, 8, 6)  # x, y, width, height in texels


def sky_ground_radiance(sun, intensity, color=(1, 1, 1)):
    """Exact outgoing radiance of the ground (normal = the dome's pole) under
    the generated sky texture, integrating texel by texel (piecewise constant;
    within 1e-4 of the bilinear lookup)."""
    w, h = SKY_SIZE
    sx, sy, sw, sh = SUN_BLOCK
    e = [0.0, 0.0, 0.0]
    for j in range(h // 2):  # upper hemisphere
        th0, th1 = math.pi * j / h, math.pi * (j + 1) / h
        row_weight = 0.5 * (math.sin(th1) ** 2 - math.sin(th0) ** 2) * 2 * math.pi / w  # cos sin dtheta dphi
        sun_texels = sw if sy <= j < sy + sh else 0
        for c in range(3):
            e[c] += row_weight * ((w - sun_texels) * SKY_COLOR[c] + sun_texels * sun[c])
    return [ALBEDO / math.pi * intensity * color[c] * e[c] for c in range(3)]


def make_sky_texture(hfs, path, sun=(200, 190, 170)):
    """Dim blue sky with a bright, warm sun patch at about 40 degrees elevation."""
    # --paste puts the first image (foreground) onto the second (background).
    sx, sy, sw, sh = SUN_BLOCK
    subprocess.run([os.path.join(hfs, "bin", "hoiiotool.exe"),
                    "--pattern", f"constant:color={sun[0]},{sun[1]},{sun[2]}", f"{sw}x{sh}", "3",
                    "--pattern", "constant:color={},{},{}".format(*SKY_COLOR), "{}x{}".format(*SKY_SIZE), "3",
                    "--paste", f"+{sx}+{sy}", "-d", "float", "-o", path], check=True)


def _xform(translate, rotate):
    ops, order = [], []
    if translate:
        ops.append(f"double3 xformOp:translate = ({translate[0]}, {translate[1]}, {translate[2]})")
        order.append('"xformOp:translate"')
    if rotate:
        ops.append(f"float3 xformOp:rotateXYZ = ({rotate[0]}, {rotate[1]}, {rotate[2]})")
        order.append('"xformOp:rotateXYZ"')
    ops.append(f"uniform token[] xformOpOrder = [{', '.join(order)}]")
    return "\n        ".join(ops)


def _shaping(shaping):
    """shaping: dict with cone_angle, cone_softness, focus, focus_tint."""
    if not shaping:
        return "", ""
    lines = []
    if "cone_angle" in shaping:
        lines.append(f"float inputs:shaping:cone:angle = {shaping['cone_angle']}")
    if "cone_softness" in shaping:
        lines.append(f"float inputs:shaping:cone:softness = {shaping['cone_softness']}")
    if "focus" in shaping:
        lines.append(f"float inputs:shaping:focus = {shaping['focus']}")
    if "focus_tint" in shaping:
        t = shaping["focus_tint"]
        lines.append(f"color3f inputs:shaping:focusTint = ({t[0]}, {t[1]}, {t[2]})")
    return ' (\n        prepend apiSchemas = ["ShapingAPI"]\n    )', "\n        " + "\n        ".join(lines)


def area_light(kind, name, intensity, translate, rotate=None, normalize=False, exposure=0.0, color=(1, 1, 1),
               shaping=None, **params):
    """kind: SphereLight, RectLight, DiskLight or CylinderLight; params are the shape inputs
    (radius, width, height, length) or the plain attributes treatAsPoint / treatAsLine."""
    attrs = []
    for key, value in params.items():
        if key in ("treatAsPoint", "treatAsLine"):
            attrs.append(f"bool {key} = {int(value)}")
        else:
            attrs.append(f"float inputs:{key} = {value}")
    api, shaping_attrs = _shaping(shaping)
    body = "\n        ".join(attrs)
    return f"""    def {kind} "{name}"{api}
    {{
        float inputs:intensity = {intensity}
        float inputs:exposure = {exposure}
        bool inputs:normalize = {int(normalize)}
        color3f inputs:color = ({color[0]}, {color[1]}, {color[2]})
        {body}{shaping_attrs}
        {_xform(translate, rotate)}
    }}"""


def distant_irradiance(intensity, angle_deg, normalize, tilt_deg, exposure=0.0):
    """UsdLux: radiance = intensity * 2^exposure, normalized by pi sin^2(half angle)."""
    radiance = intensity * 2.0 ** exposure
    half = math.radians(angle_deg) * 0.5
    projected = math.pi * math.sin(half) ** 2
    e_normal = radiance if (normalize or angle_deg == 0) else radiance * projected
    return e_normal * math.cos(math.radians(tilt_deg))


CASES = [
    # name, lights, expected outgoing radiance per channel (Lambert albedo 0.5)
    ("distant_normalized_default", distant("sun", 1.0, 0.53, True, 30),
     [ALBEDO / math.pi * distant_irradiance(1.0, 0.53, True, 30)] * 3),
    ("distant_unnormalized_radiance", distant("sun", 5000.0, 1.0, False, 30),
     [ALBEDO / math.pi * distant_irradiance(5000.0, 1.0, False, 30)] * 3),
    ("distant_exposure", distant("sun", 2.0, 0.53, True, 0, exposure=1.0),
     [ALBEDO / math.pi * distant_irradiance(2.0, 0.53, True, 0, exposure=1.0)] * 3),
    ("distant_diffuse_multiplier", distant("sun", 2.0, 0.53, True, 0, diffuse=0.25),
     [ALBEDO / math.pi * 2.0 * 0.25] * 3),
    ("distant_wide_normalized", distant("sun", 1.0, 40.0, True, 0),
     [ALBEDO / math.pi * 1.0] * 3),
    # Karma renders angles beyond 180 degrees like 180 (a hemisphere).
    ("distant_360_normalized", distant("sun", 1.0, 360.0, True, 0),
     [ALBEDO / math.pi * 1.0] * 3),
    ("distant_250_tilted", distant("sun", 1.0, 250.0, True, 45), None),
    ("distant_color_temperature", distant("sun", 1.0, 0.53, True, 0, color_temperature=3000), None),
    ("dome_colored", dome("sky", 0.5, color=(1.0, 0.8, 0.6)),
     [ALBEDO * 0.5 * c for c in (1.0, 0.8, 0.6)]),
    # Textured domes are checked against the exact integral: Karma's own
    # estimate is biased for small bright features (about -5% for this sun,
    # with either misbias setting), while it matches for smooth maps.
    ("dome_textured", dome("sky", 0.5, texture=SKY_TEXTURE), sky_ground_radiance((200, 190, 170), 0.5)),
    ("dome_textured_dimsun", dome("sky", 0.5, texture=DIM_SKY_TEXTURE), sky_ground_radiance((2, 1.9, 1.7), 0.5)),
    ("dome_textured_rotated", dome("sky", 0.5, color=(1.0, 0.9, 0.8), texture=SKY_TEXTURE, rotate=(0, 70, 0)),
     sky_ground_radiance((200, 190, 170), 0.5, (1.0, 0.9, 0.8))),
    # Area lights sit outside the camera frustum (which covers the ground
    # within |x|, |z| < 2), so the comparison is about the light they cast,
    # independent of how each renderer shows light geometry to the camera.
    ("sphere", area_light("SphereLight", "bulb", 10.0, (3, 2, 0), radius=0.25), None),
    ("sphere_normalized", area_light("SphereLight", "bulb", 10.0, (3, 2, 0), normalize=True, radius=0.5), None),
    ("sphere_point", area_light("SphereLight", "bulb", 10.0, (3, 2, 0), radius=0.5, treatAsPoint=True), None),
    ("rect_down", area_light("RectLight", "panel", 5.0, (3, 1.5, 0), (-90, 0, 0), width=2, height=1), None),
    ("rect_normalized", area_light("RectLight", "panel", 5.0, (3, 1.5, 0), (-90, 0, 0), normalize=True,
                                   width=2, height=1), None),
    ("rect_tilted", area_light("RectLight", "panel", 5.0, (3, 1.5, 0), (-60, 30, 0), width=1, height=2), None),
    ("disk_down", area_light("DiskLight", "disk", 5.0, (3, 1.5, 0), (-90, 0, 0), radius=0.5), None),
    ("cylinder", area_light("CylinderLight", "tube", 5.0, (3, 1, 0), length=2, radius=0.25), None),
    ("cylinder_normalized", area_light("CylinderLight", "tube", 5.0, (3, 1, 0), normalize=True, length=2,
                                       radius=0.25), None),
    # Spot lights: point lights shaped into a cone pointing down at the ground.
    ("spot_cone", area_light("SphereLight", "spot", 10.0, (0, 4, 0), (-90, 0, 0), radius=0.1, treatAsPoint=True,
                             shaping={"cone_angle": 20, "cone_softness": 0.3}), None),
    ("spot_focus", area_light("SphereLight", "spot", 10.0, (0, 4, 0), (-90, 0, 0), radius=0.1, treatAsPoint=True,
                              shaping={"cone_angle": 60, "focus": 5, "focus_tint": (0.2, 0.4, 0.8)}), None),
    # Karma blends the cone edge with a smoothstep in cos(theta) (as pbrt),
    # not in the angle as the UsdLux docs describe; a wide, fully soft cone
    # tells smoothstep and linear falloffs apart.
    ("spot_soft", area_light("SphereLight", "spot", 10.0, (0, 1.5, 0), (-90, 0, 0), radius=0.1, treatAsPoint=True,
                             shaping={"cone_angle": 45, "cone_softness": 1.0}), None),
    # Sidedness: a rect facing away from the ground.
    ("rect_up", area_light("RectLight", "panel", 5.0, (3, 1.5, 0), (90, 0, 0), width=2, height=1), None),
    # A sphere light sunk into the ground lights the ground inside it (Karma's
    # interior values vary by a few percent with the position; Nova's inside
    # is a uniform shell).
    ("sphere_sunk", area_light("SphereLight", "orb", 1.0, (0, 0.3, 0), radius=0.5), None),
    # Negative lights subtract light.
    ("point_negative", distant("sun", 1.0, 0.0, True, 0) + "\n"
     + area_light("SphereLight", "neg", -10.0, (0, 4, 0), radius=0.1, treatAsPoint=True), None),
    # Lights inside the frustum: does the camera see the light geometry?
    ("sphere_visible", area_light("SphereLight", "bulb", 10.0, (0, 3, 0), radius=0.3), None),
    ("rect_visible_front", area_light("RectLight", "panel", 5.0, (0, 3, 0), (90, 0, 0), width=1, height=1), None),
    ("rect_visible_back", area_light("RectLight", "panel", 5.0, (0, 3, 0), (-90, 0, 0), width=1, height=1), None),
]


def render(hfs, renderer, scene_path, out_image, env):
    cmd = [os.path.join(hfs, "bin", "husk.exe"), "--renderer", renderer, "--settings", "/Render/settings",
           "-o", out_image, scene_path]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout[-1500:], proc.stderr[-1500:])
    return proc.returncode == 0


def average(hfs, image):
    out = subprocess.run([os.path.join(hfs, "bin", "hoiiotool.exe"), image, "--printstats"],
                         capture_output=True, text=True).stdout
    m = re.search(r"Stats Avg:\s*([-0-9.e ]+)", out)
    return [float(v) for v in m.group(1).split()][:3] if m else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--spp", type=int, default=64)
    parser.add_argument("--only", default="")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    env = dict(os.environ)
    prefs = tempfile.mkdtemp(prefix="nova_hprefs_")
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(prefs, "houdini__HVER__").replace("\\", "/")
    for var in ("HOUDINI_PACKAGE_DIR", "HSITE", "PXR_PLUGINPATH_NAME", "HOUDINI_USD_DSO_PATH"):
        env.pop(var, None)
    env["HOUDINI_PACKAGE_SKIPLIST"] = "nova"
    env["HOUDINI_PATH"] = args.stage.replace("\\", "/") + ";&"

    sky = os.path.join(args.out, "sky.exr").replace("\\", "/")
    make_sky_texture(args.hfs, sky)
    dim_sky = os.path.join(args.out, "sky_dim.exr").replace("\\", "/")
    make_sky_texture(args.hfs, dim_sky, sun=(2, 1.9, 1.7))

    worst = 0.0
    worst_analytic = 0.0
    try:
        print(f"{'case':32s} {'nova':>26s} {'karma':>26s} {'analytic':>26s}  nova/karma  nova/analytic")
        for name, lights, expected in CASES:
            if args.only and not any(token and token in name for token in args.only.split(",")):
                continue
            scene = os.path.join(args.out, name + ".usda")
            with open(scene, "w", encoding="utf-8") as f:
                lights = lights.replace(SKY_TEXTURE, sky).replace(DIM_SKY_TEXTURE, dim_sky)
                f.write(SCENE.format(a=ALBEDO, lights=lights, spp=args.spp))
            results = {}
            for renderer, tag in (("Nova", "nova"), ("BRAY_HdKarma", "karma")):
                image = os.path.join(args.out, f"{name}_{tag}.exr")
                results[tag] = average(args.hfs, image) if render(args.hfs, renderer, scene, image, env) else None
            nova, karma = results["nova"], results["karma"]
            fmt = lambda v: "(" + ", ".join(f"{x:.5f}" for x in v) + ")" if v else "-"
            ratio = [n / k if k else float("nan") for n, k in zip(nova, karma)] if nova and karma else []
            to_analytic = [n / e if e else float("nan") for n, e in zip(nova, expected)] if nova and expected else []
            if to_analytic:
                # Analytic cases are judged against the exact value (Karma is
                # biased for some of them, see the dome cases).
                worst_analytic = max(worst_analytic, max(abs(r - 1.0) for r in to_analytic))
            elif ratio:
                worst = max(worst, max((abs(r - 1.0) for r in ratio if r == r), default=0.0))
            print(f"{name:32s} {fmt(nova):>26s} {fmt(karma):>26s} {fmt(expected):>26s}  "
                  + (" ".join(f"{r:.4f}" for r in ratio) if ratio else "-") + "  "
                  + (" ".join(f"{r:.4f}" for r in to_analytic) if to_analytic else "-"))
    finally:
        shutil.rmtree(prefs, ignore_errors=True)
    print(f"largest relative deviation Nova vs analytic: {worst_analytic:.4f}")
    print(f"largest relative deviation Nova vs Karma (cases without analytic value): {worst:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
