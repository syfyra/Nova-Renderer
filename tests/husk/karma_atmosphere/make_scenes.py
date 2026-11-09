"""Builds the scenes of tests/husk/check_karma_atmosphere.py with Houdini's own
Karma LOPs (docs/design/M11-P7.md): the Karma Sky Atmosphere LOP with a sun
DistantLight, and the Karma Physical Sky LOP in atmosphere mode, plus a
measuring rig authored by a Python LOP:

  * cameras at the origin (100 m above the LOP's default sea level), square,
    with pixel-centre sampling (box filter of zero width): /rig/up (towards
    the zenith, 170 degrees), /rig/front (along -Z, 120 degrees), and per
    scene others (down, towards the sun, ...);
  * /rig/patch, a small white Lambert quad facing up below the front camera
    (its radiance is the irradiance over pi), and per scene black or white
    walls (aerial perspective) or an occluder (volumetric shadows);
  * /Render/<camera>: settings for Karma and Nova with SPP samples, no colour
    limit and the uniform pixel oracle.

The sun is a DistantLight of intensity 1 (normalized, angle 0.53) at the
scene's solar altitude and azimuth (0 = towards -Z, as the Physical Sky).

Run it in an isolated hython (never in a user's session):
  powershell -File tests/houdini/run_hython.ps1 -Script tests/husk/karma_atmosphere/make_scenes.py <outdir>
The generated .usda files are checked in next to this script.
"""

import json
import math
import os
import sys

import hou

sys.stdout.reconfigure(encoding="utf-8")
OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
os.makedirs(OUT, exist_ok=True)
VL = "xn__primvarskarmaobjectvolumelimit_zpbfg"
GROUND_VIS = "xn__primvarsgroundvisibility_7fb"

RIG = r'''
import math, json
from pxr import Gf, Sdf, Usd, UsdGeom, UsdRender, UsdShade, UsdLux
stage = hou.pwd().editableStage()
cfg = json.loads(CFGJSON)

def look(path, pos, d, fov):
    cam = UsdGeom.Camera.Define(stage, path)
    aperture = 2.0 * math.tan(math.radians(fov / 2.0)) * 10.0
    cam.CreateFocalLengthAttr(10.0)
    cam.CreateHorizontalApertureAttr(aperture)
    cam.CreateVerticalApertureAttr(aperture)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.01, 1.0e6))
    up = Gf.Vec3d(0, 1, 0) if abs(d[1]) < 0.99 else Gf.Vec3d(0, 0, -1)
    m = Gf.Matrix4d().SetLookAt(Gf.Vec3d(*pos), Gf.Vec3d(*pos) + Gf.Vec3d(*d), up).GetInverse()
    UsdGeom.Xformable(cam).AddTransformOp().Set(m)

for name, c in cfg["cams"].items():
    look("/rig/" + name, c["pos"], c["dir"], c["fov"])

if "sun" in cfg:
    alt, az = math.radians(cfg["sun"][0]), math.radians(cfg["sun"][1])
    s = Gf.Vec3d(math.cos(alt) * math.sin(az), math.sin(alt), -math.cos(alt) * math.cos(az))
    light = UsdLux.DistantLight.Define(stage, "/lights/sun")
    light.CreateIntensityAttr(1.0)
    light.CreateAngleAttr(0.53)
    light.CreateNormalizeAttr(True)
    if cfg.get("sunvisible"):
        light.GetPrim().CreateAttribute("inputs:karma:light:renderlightgeo", Sdf.ValueTypeNames.Bool).Set(True)
    up = Gf.Vec3d(0, 1, 0) if abs(s[1]) < 0.99 else Gf.Vec3d(1, 0, 0)
    z = s.GetNormalized(); x = Gf.Cross(up, z).GetNormalized(); y = Gf.Cross(z, x)
    m = Gf.Matrix4d(x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, 0, 0, 0, 1)
    UsdGeom.Xformable(light).AddTransformOp().Set(m)

def material(name, base):
    mtl = UsdShade.Material.Define(stage, "/rig/mtl/" + name)
    sh = UsdShade.Shader.Define(stage, "/rig/mtl/%s/s" % name)
    sh.CreateIdAttr("ND_standard_surface_surfaceshader")
    sh.CreateInput("base", Sdf.ValueTypeNames.Float).Set(base)
    sh.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(1.0, 1.0, 1.0))
    sh.CreateInput("specular", Sdf.ValueTypeNames.Float).Set(0.0)
    mtl.CreateSurfaceOutput("mtlx").ConnectToSource(sh.ConnectableAPI(), "out")
    return mtl

mats = {"white": material("white", 1.0), "black": material("black", 0.0)}

for i, g in enumerate(cfg.get("geo", [])):
    c, u, v = g["c"], g["u"], g["v"]
    pts = [Gf.Vec3f(*[c[k] - u[k] - v[k] for k in range(3)]), Gf.Vec3f(*[c[k] + u[k] - v[k] for k in range(3)]),
           Gf.Vec3f(*[c[k] + u[k] + v[k] for k in range(3)]), Gf.Vec3f(*[c[k] - u[k] + v[k] for k in range(3)])]
    mesh = UsdGeom.Mesh.Define(stage, "/rig/" + g.get("name", "geo%d" % i))
    mesh.CreatePointsAttr(pts)
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateDoubleSidedAttr(True)
    lo = Gf.Vec3f(*[min(p[k] for p in pts) for k in range(3)]); hi = Gf.Vec3f(*[max(p[k] for p in pts) for k in range(3)])
    mesh.CreateExtentAttr([lo, hi])
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mats[g.get("mtl", "white")])

for name in cfg["cams"]:
    rs = UsdRender.Settings.Define(stage, "/Render/" + name)
    rs.CreateCameraRel().SetTargets(["/rig/" + name])
    res = cfg.get("res", 64)
    rs.CreateResolutionAttr(Gf.Vec2i(res, res))
    prod = UsdRender.Product.Define(stage, "/Render/Products/" + name)
    prod.CreateProductNameAttr(name + ".exr")
    var = UsdRender.Var.Define(stage, "/Render/Products/Vars/C")
    var.CreateSourceNameAttr("C.*[LO]")
    var.CreateSourceTypeAttr("lpe")
    var.CreateDataTypeAttr("color4f")
    var.GetPrim().CreateAttribute("driver:parameters:aov:name", Sdf.ValueTypeNames.String).Set("C")
    var.GetPrim().CreateAttribute("driver:parameters:aov:format", Sdf.ValueTypeNames.Token).Set("color4f")
    prod.CreateOrderedVarsRel().SetTargets([var.GetPath()])
    rs.CreateProductsRel().SetTargets([prod.GetPath()])
    p = rs.GetPrim()
    spp = cfg.get("spp", 256)
    for attr, kind, value in (
            ("nova:samples", Sdf.ValueTypeNames.Int, spp),
            ("nova:pixelFilter", Sdf.ValueTypeNames.Token, "box"),
            ("nova:pixelFilterScale", Sdf.ValueTypeNames.Float, 0.0),
            ("nova:clampIndirect", Sdf.ValueTypeNames.Float, 0.0),
            ("karma:global:samplesperpixel", Sdf.ValueTypeNames.Int, spp),
            ("karma:global:pathtracedsamples", Sdf.ValueTypeNames.Int, spp),
            ("karma:global:pixeloracle", Sdf.ValueTypeNames.String, "uniform"),
            ("karma:global:pixelfilter", Sdf.ValueTypeNames.String, "box"),
            ("karma:global:pixelfilterscale", Sdf.ValueTypeNames.Float, 1.0e-6),
            ("karma:global:colorlimit", Sdf.ValueTypeNames.Float, 1.0e6),
            ("karma:global:indirectcolorlimit", Sdf.ValueTypeNames.Float, 1.0e6)):
        p.CreateAttribute(attr, kind).Set(value)
'''

STD = {"up": {"pos": [0, 0, 0], "dir": [0, 1, 0], "fov": 170},
       "front": {"pos": [0, 0, 0], "dir": [0, 0, -1], "fov": 120}}
PATCH = {"name": "patch", "c": [0, -1, -0.8], "u": [0.3, 0, 0], "v": [0, 0, 0.3]}


def sundir(alt, az):
    a, z = math.radians(alt), math.radians(az)
    return [math.cos(a) * math.sin(z), math.sin(a), -math.cos(a) * math.cos(z)]


WALLS = []
for i, dist in enumerate((1000, 3000, 10000, 30000)):
    x = (i - 1.5) * 0.3 * dist
    WALLS.append({"name": "wall%d" % i, "c": [x, 0, -dist], "u": [0.12 * dist, 0, 0], "v": [0, 0.05 * dist, 0],
                  "mtl": "black"})

# name -> configuration: "atmo" (Sky Atmosphere LOP parameters) with "sun"
# [altitude, azimuth], or "physical" (Physical Sky LOP parameters, sky mode
# atmosphere).
CONFIGS = {
    "sa_a30_vl0": {"sun": [30, 0], "atmo": {VL: 0}, "cams": STD, "geo": [PATCH]},
    "sa_a5_vl0": {"sun": [5, 0], "atmo": {VL: 0}, "cams": STD, "geo": [PATCH]},
    "sa_a30": {"sun": [30, 0], "atmo": {}, "cams": STD, "geo": [PATCH]},
    "sa_a5": {"sun": [5, 0], "atmo": {}, "cams": STD, "geo": [PATCH]},
    "sa_a20_vl4": {"sun": [20, 0], "atmo": {VL: 4}, "cams": STD, "geo": [PATCH]},
    "sa_twilight": {"sun": [-2, 0], "atmo": {VL: 0}, "cams": STD},
    "sa_ground": {"sun": [30, 0], "atmo": {VL: 0, GROUND_VIS: "*",
                                           "xn__primvarsgroundalbedo_p8a": (0.6, 0.4, 0.2)},
                  "cams": {"down": {"pos": [0, 0, 0], "dir": [0, -1, 0], "fov": 120}, "front": STD["front"]}},
    "sa_aerial": {"sun": [30, 180], "atmo": {VL: 0}, "res": 128,
                  "cams": {"front": {"pos": [0, 0, 0], "dir": [0, 0, -1], "fov": 90}}, "geo": WALLS},
    "sa_disc": {"sun": [10, 0], "sunvisible": True, "atmo": {VL: 0}, "res": 32,
                "cams": {"sun": {"pos": [0, 0, 0], "dir": sundir(10, 0), "fov": 2}}},
    "sa_shadow": {"sun": [10, 90], "atmo": {VL: 0},
                  "cams": {"front": {"pos": [0, 0, 0], "dir": [0, 0, -1], "fov": 90}},
                  "geo": [{"name": "occluder", "c": [3000, 5000, -20000], "u": [0, 0, 40000], "v": [0, 5000, 0],
                           "mtl": "black"}]},
    "sa_params": {"sun": [20, 30], "atmo": {VL: 0, "rayleighscale": 6e-5, "miescatterscale": 1.5e-5,
                                            "xn__primvarsmiephase_61a": 0.5, "extrascale": 0.0,
                                            "planetcentery": -1000.0, "sealevel": 999.9},
                  "cams": STD, "geo": [PATCH]},
    "ps_a5": {"physical": {"solar_altitude": 5.0, "solar_azimuth": 30.0}, "cams": STD, "geo": [PATCH]},
    "ps_a30": {"physical": {"solar_altitude": 30.0, "solar_azimuth": 30.0}, "cams": STD, "geo": [PATCH]},
    "ps_a60": {"physical": {"solar_altitude": 60.0, "solar_azimuth": 30.0}, "cams": STD, "geo": [PATCH]},
}


def set_parm(node, key, value):
    tup = node.parmTuple(key)
    if tup is not None:
        tup.set(value if isinstance(value, (tuple, list)) else (value,))
        return
    parm = node.parm(key)  # a component (planetcentery)
    if parm is None:
        raise RuntimeError("no parameter " + key)
    parm.set(value)


def build(name, cfg):
    stage = hou.node("/stage")
    for child in stage.children():
        child.destroy()
    if "physical" in cfg:
        sky = stage.createNode("karmaphysicalsky", name + "_sky")
        sky.parm("primpath").set("/lights/physicalsky")
        sky.parm("skymode").set(1)  # atmosphere
        for key, value in cfg["physical"].items():
            set_parm(sky, key, value)
        last = sky
    else:
        atm = stage.createNode("karmaskyatmosphere", name + "_atmo")
        atm.parm("primpath").set("/atmosphere/sky")
        for key, value in cfg.get("atmo", {}).items():
            set_parm(atm, key, value)
        last = atm
    rig = stage.createNode("pythonscript", name + "_rig")
    rig.parm("python").set(RIG.replace("CFGJSON", repr(json.dumps(cfg))))
    rig.setInput(0, last)
    rig.cook(force=True)
    errors = last.errors() + rig.errors()
    if errors:
        raise RuntimeError("%s: %s" % (name, errors))
    layer = rig.stage().Flatten()
    if layer.GetPrimAtPath("/HoudiniLayerInfo"):
        del layer.rootPrims["HoudiniLayerInfo"]

    def strip(spec):
        for item in [spec] + list(spec.properties):
            data = dict(item.customData)
            if data.pop("HoudiniPrimEditorNodes", None) is not None:
                if data:
                    item.customData = data
                else:
                    item.ClearInfo("customData")
        for child in spec.nameChildren:
            strip(child)

    for prim in layer.rootPrims:
        strip(prim)
    layer.documentation = "Built by tests/husk/karma_atmosphere/make_scenes.py (%s)" % name
    path = os.path.join(OUT, name + ".usda")
    layer.Export(path)
    print("wrote", path, flush=True)


ONLY = sys.argv[2].split(",") if len(sys.argv) > 2 else None
for name, cfg in CONFIGS.items():
    if ONLY is None or name in ONLY:
        build(name, cfg)
print("done")
