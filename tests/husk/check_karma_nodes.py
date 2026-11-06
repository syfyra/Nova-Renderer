"""husk checks of the Karma Shadow Catcher and Background Plate LOPs and of the
ray visibility and holdout primvars they author (docs/design/M11-P2.md)
against Karma CPU's measured values (Houdini 22.0.466, 256 samples, box
filter, measured once with an isolated husk; research scripts in the M11 P2
scratch folder).

Two kinds of scenes:
- Probes written here (plain USDA, as check_lights.py): an orthographic camera
  looking down -Y over 8 x 8 units (image top = -Z), a grey ground, a ball, a
  45 degree sun and an optional dome ("top"); a 45 degree mirror sending every
  camera ray onto vertical cards ("mirror": what reflections see); a wall lit
  by a grazing sun next to the ground ("bounce": what diffuse rays see); a
  background box shadowing part of the ground ("bgocc", bgoccludeshadow).
- The real LOPs' output (tests/husk/karma_nodes/*.usda, built by isolated
  hython: a Python LOP base stage, the Karma LOP, a Karma Render Settings
  LOP; customData stripped): the Shadow Catcher with and without "Blocks
  Lighting", the Karma Render Settings' "Add Holdout Shadows To Beauty", the
  Background Plate 1.0 and 2.0. Their plate (./plate.pic) is Houdini's
  Mandril.pic, copied next to the scene at run time.

Region means (R channel, A for alpha) must be within 3 % + 2e-3 of Karma's;
alpha and exact zeros within 1e-6.

$HFS/python313/python.exe -I check_karma_nodes.py --hfs ... --stage ... --out ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

HERE = os.path.dirname(os.path.abspath(__file__))
RES = 96
SPP = 256

AOVS = [
    ("C", "C.*[LO]", "lpe", "color4f"),
    ("HS", "holdouts;shadow;CDL", "lpe", "color3f"),
    ("HD", "holdouts;CD.+L", "lpe", "color3f"),
    ("HB", "holdout;C.*[LO]", "lpe", "color3f"),
    ("DI", "C<RD>L", "lpe", "color3f"),
]

MATERIALS = '''
def Scope "m"
{
    def Material "grey"
    {
        token outputs:mtlx:surface.connect = </m/grey/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.5, 0.5, 0.5)
            float inputs:specular = 0
            token outputs:out
        }
    }
    def Material "dark"
    {
        token outputs:mtlx:surface.connect = </m/dark/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.2, 0.2, 0.2)
            float inputs:specular = 0
            token outputs:out
        }
    }
    def Material "red"
    {
        token outputs:mtlx:surface.connect = </m/red/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.8, 0.1, 0.1)
            float inputs:specular = 0
            token outputs:out
        }
    }
    def Material "spec"
    {
        token outputs:mtlx:surface.connect = </m/spec/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.5, 0.5, 0.5)
            float inputs:specular_roughness = 0.3
            token outputs:out
        }
    }
    def Material "mirror"
    {
        token outputs:mtlx:surface.connect = </m/mirror/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 1
            color3f inputs:base_color = (1, 1, 1)
            float inputs:metalness = 1
            float inputs:specular_roughness = 0
            token outputs:out
        }
    }
}
'''


def prim_attrs(holdout=None, vis=None):
    s = ""
    if holdout is not None:
        s += '\n        int primvars:karma:object:holdoutmode = %d (interpolation = "constant")' % holdout
    if vis is not None:
        s += '\n        string primvars:karma:object:rendervisibility = "%s" (interpolation = "constant")' % vis
    return s


def quad(name, pts, normal, material, attrs=""):
    p = ", ".join("(%g, %g, %g)" % q for q in pts)
    n = ", ".join(["(%g, %g, %g)" % normal] * 4)
    lo = tuple(min(q[i] for q in pts) for i in range(3))
    hi = tuple(max(q[i] for q in pts) for i in range(3))
    return '''
    def Mesh "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [%s]
        normal3f[] normals = [%s] (interpolation = "vertex")
        float3[] extent = [(%g, %g, %g), (%g, %g, %g)]
        uniform token subdivisionScheme = "none"
        rel material:binding = </m/%s>%s
    }''' % (name, p, n, *lo, *hi, material, attrs)


def sphere(name, center, radius, material, attrs=""):
    return '''
    def Sphere "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        double radius = %g
        rel material:binding = </m/%s>
        double3 xformOp:translate = (%g, %g, %g)
        uniform token[] xformOpOrder = ["xformOp:translate"]%s
    }''' % (name, radius, material, *center, attrs)


def box(name, lo, hi, material, attrs=""):
    c = tuple((a + b) / 2 for a, b in zip(lo, hi))
    s = tuple((b - a) / 2 for a, b in zip(lo, hi))
    return '''
    def Cube "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        double size = 2
        rel material:binding = </m/%s>
        double3 xformOp:translate = (%g, %g, %g)
        float3 xformOp:scale = (%g, %g, %g)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]%s
    }''' % (name, material, *c, *s, attrs)


def sun(rotate, intensity, extra=""):
    return '''
    def DistantLight "sun"
    {
        float inputs:angle = 0.53
        bool inputs:normalize = 1
        float inputs:intensity = %g%s
        float3 xformOp:rotateXYZ = (%g, %g, %g)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }''' % (intensity, extra, *rotate)


ILLUM = "\n        bool inputs:karma:light:illumbackground = 1"


def scene(kind, ground=None, ball=None, target=None, other=None, sun_intensity=3.0, sun_extra="", dome_i=0.0,
          settings="", caster=False):
    ground, ball, target, other = ground or {}, ball or {}, target or {}, other or {}
    geo = lights = ""
    if kind in ("top", "bgocc"):
        geo += quad("ground", [(-6, 0, 6), (6, 0, 6), (6, 0, -6), (-6, 0, -6)], (0, 1, 0), "grey", prim_attrs(**ground))
        geo += sphere("ball", (0, 1, 0), 1.0, "grey", prim_attrs(**ball))
        if kind == "bgocc":
            geo += box("wall", (0.2, 0, -1.2), (2.0, 1.5, -0.8), "grey", prim_attrs(**other))
        if sun_intensity is not None:
            lights += sun((-45, 0, 0), sun_intensity, sun_extra)
    elif kind == "mirror":
        geo += quad("mirror", [(-5, -5, 5), (5, -5, 5), (5, 5, -5), (-5, 5, -5)], (0, 0.70710678, 0.70710678), "mirror")
        geo += quad("cardL", [(-6, 6, 6), (0, 6, 6), (0, -6, 6), (-6, -6, 6)], (0, 0, -1),
                    target.pop("material", "grey"), prim_attrs(**target))
        geo += quad("cardR", [(0, 6, 6), (6, 6, 6), (6, -6, 6), (0, -6, 6)], (0, 0, -1), "grey")
        geo += quad("back", [(-8, 8, 8), (8, 8, 8), (8, -8, 8), (-8, -8, 8)], (0, 0, -1), "red")
        if caster:
            geo += sphere("caster", (-2, 2.4, 4.2), 0.8, "grey")
        lights += sun((-53.1301, 180, 0), sun_intensity, sun_extra)
    elif kind == "bounce":
        geo += quad("ground", [(-6, 0, 6), (6, 0, 6), (6, 0, -6), (-6, 0, -6)], (0, 1, 0), "grey", prim_attrs(**ground))
        geo += quad("wall", [(0, 0, 6), (0, 0, -6), (0, 4, -6), (0, 4, 6)], (1, 0, 0), "grey", prim_attrs(**target))
        lights += sun((0, 90, 0), sun_intensity, sun_extra)
    if dome_i > 0:
        lights += '''
    def DomeLight "dome"
    {
        float inputs:intensity = %g
    }''' % dome_i
    vdefs = "".join('''
        def RenderVar "%s"
        {
            uniform token dataType = "%s"
            uniform string sourceName = "%s"
            uniform token sourceType = "%s"
            token driver:parameters:aov:format = "%s"
            string driver:parameters:aov:name = "%s"
        }''' % (n, d, s, t, d, n) for n, s, t, d in AOVS)
    order = ", ".join("</Render/Vars/%s>" % a[0] for a in AOVS)
    return '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{%s
}
def Xform "lights"
{%s
}
def Camera "cam"
{
    token projection = "orthographic"
    float horizontalAperture = 80
    float verticalAperture = 80
    float2 clippingRange = (0.1, 100)
    double3 xformOp:translate = (0, 10, 0)
    float3 xformOp:rotateXYZ = (-90, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
}
%s
def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </cam>
        int2 resolution = (%d, %d)
        rel products = [</Render/Products/p>]
        token nova:pixelFilter = "box"
        float nova:pixelFilterScale = 0
        int nova:samples = %d
        float nova:clampIndirect = 0%s
    }
    def Scope "Products"
    {
        def RenderProduct "p"
        {
            token productName = "out.exr"
            rel orderedVars = [%s]
        }
    }
    def Scope "Vars"
    {%s
    }
}
''' % (geo, lights, MATERIALS, RES, RES, SPP, settings, order, vdefs)


# Regions (x, z, half size) of the top view, the mirror's (image x, z) and the bounce scene's.
REGIONS = {
    "ball": (0, 0, 0.3), "shadow": (0, -1.8, 0.2), "open": (2.8, 2.8, 0.3), "near": (1.3, 0.0, 0.12),
    "L": (-2.0, 0.0, 0.4), "R": (2.0, 0.0, 0.4), "Lopen": (-2.0, 3.0, 0.3),
    "b1": (0.4, 0.0, 0.2), "b2": (1.0, 0.0, 0.3),
    "both": (0.5, -1.6, 0.12), "ball_only": (-0.4, -1.6, 0.15), "wall_only": (1.6, -2.2, 0.15),
}


def region(name):
    x, z, h = REGIONS[name]
    px = RES / 8.0
    c0, c1 = int(round((x - h + 4) * px)), int(round((x + h + 4) * px))
    r0, r1 = int(round((z - h + 4) * px)), int(round((z + h + 4) * px))
    return slice(r0, max(r1, r0 + 1)), slice(c0, max(c1, c0 + 1))


# name -> (kind, scene keywords, [(layer, channel, region, Karma's mean)]);
# channel 0 = R, 3 = alpha.
PROBES = {
    # Matte: cut out for the camera (colour, alpha 0), its light in holdout;,
    # still a shadow caster.
    "matte_ball": ("top", dict(dome_i=0.3, ball=dict(holdout=1)),
                   [("C", 0, "ball", 0.0), ("C", 3, "ball", 0.0), ("HB", 0, "ball", 0.4744),
                    ("C", 0, "shadow", 0.1379), ("C", 3, "open", 1.0)]),
    # Background ground: alpha 0 and no colour; the shadow fraction in
    # holdouts;shadow;, the holdout's own light in holdout;.
    "bg_ground": ("top", dict(dome_i=0.3, ground=dict(holdout=2)),
                  [("C", 0, "open", 0.0), ("C", 3, "open", 0.0), ("C", 3, "shadow", 0.0),
                   ("HS", 0, "shadow", 0.7235), ("HB", 0, "open", 0.4871), ("HB", 0, "shadow", 0.1379),
                   ("C", 0, "ball", 0.4744)]),
    # The Shadow Catcher's visibility (primary only) and "Blocks Lighting".
    "catcher_ground": ("top", dict(dome_i=0.3, ground=dict(holdout=2, vis="primary")),
                       [("C", 3, "open", 0.0), ("HS", 0, "shadow", 0.7235), ("HB", 0, "shadow", 0.1427)]),
    "catcher_block": ("top", dict(dome_i=0.3, ground=dict(holdout=2, vis="primary|shadow")),
                      [("C", 3, "open", 0.0), ("HS", 0, "shadow", 0.7235), ("HB", 0, "shadow", 0.1379)]),
    # Sun only: the umbra is shadow fraction 1, the open ground 0.
    "bg_ground_sun": ("top", dict(ground=dict(holdout=2)),
                      [("HS", 0, "shadow", 1.0), ("HS", 0, "open", 0.0), ("HB", 0, "open", 0.3388)]),
    # illumbackground: the light's shadows leave the holdout shadow fraction.
    "illum_both": ("top", dict(dome_i=0.3, ground=dict(holdout=2), sun_extra=ILLUM),
                   [("HS", 0, "open", 0.0132), ("HB", 0, "open", 0.4871)]),
    # Ray visibility: a phantom ball keeps its shadow; without shadow rays
    # the ground under it takes the whole sun.
    "phantom_ball": ("top", dict(dome_i=0.3, ball=dict(vis="-primary")),
                     [("C", 3, "ball", 1.0), ("C", 0, "shadow", 0.1379)]),
    "no_shadow_ball": ("top", dict(dome_i=0.3, ball=dict(vis="-shadow")),
                       [("C", 0, "shadow", 0.4910), ("C", 0, "ball", 0.4746)]),
    # What reflections see: a background card is pre-lit (its albedo times
    # the light that reaches it, whatever the light's strength), plus the
    # illumbackground lights' light.
    "mirror_bg": ("mirror", dict(target=dict(holdout=2)), [("C", 0, "L", 0.4957)]),
    "mirror_bg_weak_sun": ("mirror", dict(target=dict(holdout=2), sun_intensity=1.0), [("C", 0, "L", 0.4957)]),
    "mirror_bg_dark": ("mirror", dict(target=dict(holdout=2, material="dark")), [("C", 0, "L", 0.1983)]),
    "mirror_bg_spec": ("mirror", dict(target=dict(holdout=2, material="spec")), [("C", 0, "L", 0.4855)]),
    "mirror_bg_metal": ("mirror", dict(target=dict(holdout=2, material="mirror")), [("C", 0, "L", 0.0)]),
    "mirror_bg_illum": ("mirror", dict(target=dict(holdout=2), sun_extra=ILLUM), [("C", 0, "L", 0.7797)]),
    "mirror_bg_shadowed": ("mirror", dict(target=dict(holdout=2), caster=True),
                           [("C", 0, "L", 0.0), ("C", 0, "Lopen", 0.4957)]),
    "mirror_bg_shadowed_dome": ("mirror", dict(target=dict(holdout=2), caster=True, dome_i=0.3),
                                [("C", 0, "L", 0.0767), ("C", 0, "Lopen", 0.3880)]),
    "mirror_bg_shadowed_illum": ("mirror", dict(target=dict(holdout=2), caster=True, sun_extra=ILLUM),
                                 [("C", 0, "L", 0.4957)]),
    # What diffuse rays see: a background wall pre-lit (the same whatever
    # the sun's strength), with illumbackground also lit.
    "bounce_bg": ("bounce", dict(target=dict(holdout=2)), [("C", 0, "b1", 0.1125), ("C", 0, "b2", 0.0935)]),
    "bounce_bg_weak_sun": ("bounce", dict(target=dict(holdout=2), sun_intensity=1.0), [("C", 0, "b1", 0.1122)]),
    "bounce_bg_illum": ("bounce", dict(target=dict(holdout=2), sun_extra=ILLUM), [("C", 0, "b1", 0.2208)]),
    "bounce_matte": ("bounce", dict(target=dict(holdout=1)), [("C", 0, "b1", 0.1078)]),
    "bounce_no_diffuse": ("bounce", dict(target=dict(vis="-diffuse")), [("C", 0, "b1", 0.0005)]),
    # Background holdouts never shadow background points; with
    # bgoccludeshadow a point they occlude keeps its light.
    "bgocc_off": ("bgocc", dict(ground=dict(holdout=2), other=dict(holdout=2)),
                  [("HS", 0, "both", 1.0), ("HS", 0, "wall_only", 0.0), ("HS", 0, "ball_only", 1.0),
                   ("HB", 0, "wall_only", 0.3376)]),
    "bgocc_on": ("bgocc", dict(ground=dict(holdout=2), other=dict(holdout=2),
                               settings="\n        bool karma:global:bgoccludeshadow = 1"),
                 [("HS", 0, "both", 0.0), ("HS", 0, "wall_only", 0.0), ("HS", 0, "ball_only", 1.0),
                  ("HB", 0, "both", 0.3376)]),
    "bgocc_cg_wall": ("bgocc", dict(ground=dict(holdout=2)), [("HS", 0, "wall_only", 1.0), ("HB", 0, "wall_only", 0.0)]),
}

# The Karma LOPs' scenes (tests/husk/karma_nodes; settings /Render/rendersettings):
# [(layer, channel, region, Karma's mean)].
LOPS = {
    "shadowcatcher": [("C", 0, "ball", 0.4740), ("C", 3, "ball", 1.0), ("C", 3, "shadow", 0.0), ("C", 3, "open", 0.0),
                      ("C", 0, "shadow", 0.0)],
    "shadowcatcher_block": [("C", 0, "ball", 0.4739), ("C", 3, "shadow", 0.0), ("C", 3, "near", 0.0)],
    "rs_holdout_shadows": [("C", 3, "shadow", 0.3620), ("C", 3, "open", 0.0020), ("C", 3, "ball", 1.0),
                           ("C", 0, "shadow", 0.0), ("holdout_shadow", 0, "shadow", 0.7240),
                           ("holdout_shadow", 0, "open", 0.0040)],
    # holdout_shadows in the umbra: Karma 0.6660, Nova 0.733 (the plate
    # material's coat, docs/design/M11-P2.md 5); left out.
    "backgroundplate2": [("C", 0, "ball", 0.4740), ("C", 3, "shadow", 0.0), ("C", 3, "open", 0.0),
                         ("holdout_shadows", 0, "open", 0.0035), ("holdout_diffuse", 0, "shadow", 0.0011),
                         ("holdout_diffuse", 0, "open", 0.0006)],
    "backgroundplate1": [("C", 0, "ball", 0.4739), ("C", 3, "shadow", 0.0), ("C", 3, "open", 0.0)],
}

LOP_OVERRIDE = '''#usda 1.0
(
    subLayers = [@./%s.usda@]
)

over "Render"
{
    over "rendersettings"
    {
        token nova:pixelFilter = "box"
        float nova:pixelFilterScale = 0
        int nova:samples = %d
        float nova:clampIndirect = 0
    }
}
'''


def read_layers(np, oiio, exr):
    image = oiio.ImageInput.open(exr)
    if not image:
        return {}
    layers = {}
    part = 0
    while image.seek_subimage(part, 0):
        spec = image.spec()
        px = np.asarray(image.read_image(part, 0, 0, spec.nchannels, oiio.FLOAT)).reshape(spec.height, spec.width, -1)
        layers[spec.getattribute("name") or str(part)] = px
        part += 1
    image.close()
    return layers


def compare(name, layers, expected):
    for layer, channel, where, karma in expected:
        plane = layers.get(layer)
        if plane is None:
            check(False, f"{name}: {layer} missing")
            continue
        r, c = region(where)
        value = float(plane[r, c, channel].mean())
        exact = karma == 0.0 or channel == 3 and karma == 1.0
        tol = 1e-6 if exact and channel == 3 else 0.03 * abs(karma) + 2e-3
        what = "alpha" if channel == 3 else "R"
        check(abs(value - karma) <= tol, f"{name} {layer} {what} {where}: Nova {value:.5f}, Karma {karma:.5f}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--renderer", default="Nova")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)
    for name, (kind, kw, expected) in PROBES.items():
        usd = os.path.join(args.out, name + ".usda")
        with open(usd, "w", encoding="utf-8", newline="\n") as f:
            f.write(scene(kind, **{k: (dict(v) if isinstance(v, dict) else v) for k, v in kw.items()}))
        exr = os.path.join(args.out, name + ".exr")
        proc = run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s", "/Render/s",
                    "-o", exr, usd], env, args.out)
        check(proc.returncode == 0, f"husk {name}.usda exits with 0")
        compare(name, read_layers(np, oiio, exr), expected)

    # The LOPs' scenes, with their plate.
    shutil.copyfile(os.path.join(args.hfs, "houdini", "pic", "Mandril.pic"), os.path.join(args.out, "plate.pic"))
    for name, expected in LOPS.items():
        shutil.copyfile(os.path.join(HERE, "karma_nodes", name + ".usda"), os.path.join(args.out, name + ".usda"))
        layer = os.path.join(args.out, name + "_nova.usda")
        with open(layer, "w", encoding="utf-8", newline="\n") as f:
            f.write(LOP_OVERRIDE % (name, SPP))
        exr = os.path.join(args.out, name + ".exr")
        proc = run([tool(args, "husk.exe"), "--renderer", args.renderer, "--headlight", "none", "-s",
                    "/Render/rendersettings", "-o", exr, layer], env, args.out)
        check(proc.returncode == 0, f"husk {name}.usda exits with 0")
        compare(name, read_layers(np, oiio, exr), expected)
    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
