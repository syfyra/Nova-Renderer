"""Builds the scenes of tests/husk/check_karma_sky.py with Houdini's own Karma
LOPs (docs/design/M11-P3.md): the Karma Sky Dome Light and the Karma Physical
Sky (sky mode) at several solar altitudes, azimuths and turbidities, plus a
measuring rig authored by a Python LOP:

  * /rig/up: a camera looking at the zenith with a 170 degree field of view
    (every azimuth down to about 5 degrees of altitude);
  * /rig/front: a camera looking horizontally along -Z with a 120 degree
    field of view (horizon, horizon blur and ground below it), which also
    sees /rig/patch, a small white Lambert quad facing up below the camera
    (its radiance is the irradiance of the upper hemisphere over pi);
  * /Render/up and /Render/front: settings for Karma and Nova with
    pixel-centre sampling (box filter of zero width), no colour limit and
    SPP samples per pixel.

Run it in an isolated hython (never in a user's session):
  powershell -File tests/houdini/run_hython.ps1 -Script tests/husk/karma_sky/make_scenes.py <outdir>
The generated .usda files are checked in next to this script.
"""

import os
import sys

import hou

sys.stdout.reconfigure(encoding="utf-8")
OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
os.makedirs(OUT, exist_ok=True)
SPP = 1024

RIG = r'''
import math
from pxr import Gf, Sdf, Usd, UsdGeom, UsdRender, UsdShade
stage = hou.pwd().editableStage()

def camera(path, fov, rotate):
    cam = UsdGeom.Camera.Define(stage, path)
    aperture = 2.0 * math.tan(math.radians(fov / 2.0)) * 10.0
    cam.CreateFocalLengthAttr(10.0)
    cam.CreateHorizontalApertureAttr(aperture)
    cam.CreateVerticalApertureAttr(aperture)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.01, 1.0e6))
    UsdGeom.XformCommonAPI(cam).SetRotate(Gf.Vec3f(*rotate))

camera("/rig/up", 170.0, (90.0, 0.0, 0.0))
camera("/rig/front", 120.0, (0.0, 0.0, 0.0))

mtl = UsdShade.Material.Define(stage, "/rig/mtl/white")
sh = UsdShade.Shader.Define(stage, "/rig/mtl/white/s")
sh.CreateIdAttr("ND_standard_surface_surfaceshader")
sh.CreateInput("base", Sdf.ValueTypeNames.Float).Set(1.0)
sh.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(1.0, 1.0, 1.0))
sh.CreateInput("specular", Sdf.ValueTypeNames.Float).Set(0.0)
mtl.CreateSurfaceOutput("mtlx").ConnectToSource(sh.ConnectableAPI(), "out")

patch = UsdGeom.Mesh.Define(stage, "/rig/patch")
h, y, z = 0.3, -1.0, -0.8
patch.CreatePointsAttr([(-h, y, z + h), (h, y, z + h), (h, y, z - h), (-h, y, z - h)])
patch.CreateFaceVertexCountsAttr([4])
patch.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
patch.CreateExtentAttr([(-h, y, z - h), (h, y, z + h)])
UsdShade.MaterialBindingAPI.Apply(patch.GetPrim()).Bind(mtl)

def settings(name, cam, res):
    rs = UsdRender.Settings.Define(stage, "/Render/" + name)
    rs.CreateCameraRel().SetTargets([cam])
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
    for attr, kind, value in (
            ("nova:samples", Sdf.ValueTypeNames.Int, SPP),
            ("nova:pixelFilter", Sdf.ValueTypeNames.Token, "box"),
            ("nova:pixelFilterScale", Sdf.ValueTypeNames.Float, 0.0),
            ("nova:clampIndirect", Sdf.ValueTypeNames.Float, 0.0),
            ("karma:global:samplesperpixel", Sdf.ValueTypeNames.Int, SPP),
            ("karma:global:pathtracedsamples", Sdf.ValueTypeNames.Int, SPP),
            ("karma:global:pixeloracle", Sdf.ValueTypeNames.String, "uniform"),
            ("karma:global:pixelfilter", Sdf.ValueTypeNames.String, "box"),
            ("karma:global:pixelfilterscale", Sdf.ValueTypeNames.Float, 1.0e-6),
            ("karma:global:colorlimit", Sdf.ValueTypeNames.Float, 1.0e6),
            ("karma:global:indirectcolorlimit", Sdf.ValueTypeNames.Float, 1.0e6)):
        p.CreateAttribute(attr, kind).Set(value)

settings("up", "/rig/up", 160)
settings("front", "/rig/front", 160)
'''

# name -> (LOP type, parameters). Sky Dome Light parameters are the USD
# input names; the Physical Sky's are its own parameter names.
SKYDOME = "karmaskydomelight"
PHYSICAL = "karmaphysicalsky"
CONFIGS = {
    "sd_default": (SKYDOME, {}),
    "sd_a5_t2": (SKYDOME, {"solar_altitude": 5.0, "turbidity": 2.0}),
    "sd_a30_t6_az90": (SKYDOME, {"solar_altitude": 30.0, "turbidity": 6.0, "solar_azimuth": 90.0}),
    "sd_a60_t3_az200": (SKYDOME, {"solar_altitude": 60.0, "solar_azimuth": 200.0}),
    "sd_a15_t10_alb0": (SKYDOME, {"solar_altitude": 15.0, "turbidity": 10.0, "ground_albedo": (0.0, 0.0, 0.0)}),
    "sd_a85_t1_alb1": (SKYDOME, {"solar_altitude": 85.0, "turbidity": 1.0, "ground_albedo": (1.0, 1.0, 1.0)}),
    "sd_light": (SKYDOME, {"intensity": 2.0, "exposure": 0.5, "color": (1.0, 0.7, 0.4), "normalize": 1}),
    "sd_ground": (SKYDOME, {"solar_altitude": 20.0, "horizon_blur": 0.05, "ground_albedo": (0.8, 0.3, 0.1),
                            "ground_color": (0.3, 0.2, 0.1)}),
    "sd_noblur": (SKYDOME, {"solar_altitude": 20.0, "horizon_blur": 0.0, "ground_color": (0.3, 0.2, 0.1)}),
    "sd_rotated": (SKYDOME, {"solar_altitude": 30.0, "rx": 20.0, "ry": 60.0}),
}
for alt in (5, 30, 60):
    for turb in (2, 3, 6):
        CONFIGS["ps_a%d_t%d" % (alt, turb)] = (PHYSICAL, {"solar_altitude": float(alt), "turbidity": float(turb),
                                                          "solar_azimuth": 30.0})


def set_skydome(node, params):
    node.parm("primpath").set("/lights/sky")
    for key, value in params.items():
        if key in ("rx", "ry", "rz"):
            node.parm(key).set(value)
            continue
        name = hou.text.encodeParm("inputs:" + key)
        tup = node.parmTuple(name)
        if tup is None:
            raise RuntimeError("no parameter for " + key)
        tup.set(value if isinstance(value, tuple) else (value,))
    # The Karma tab's Render Light Geometry (its parameter names carry a
    # suffix of the tab, so find them by prefix).
    geo = [p for p in node.parms() if p.name().startswith("xn__inputskarmalightrenderlightgeo_")]
    for p in geo:
        p.set("set" if "_control_" in p.name() else 1)
    if len(geo) != 2:
        raise RuntimeError("Render Light Geometry parameters not found: %s" % [p.name() for p in geo])


def build(name, kind, params):
    stage = hou.node("/stage")
    if kind == SKYDOME:
        sky = stage.createNode(SKYDOME, name + "_sky")
        set_skydome(sky, params)
    else:
        sky = stage.createNode(PHYSICAL, name + "_sky")
        sky.parm("primpath").set("/lights/physicalsky")
        for key, value in params.items():
            sky.parm(key).set(value)
    rig = stage.createNode("pythonscript", name + "_rig")
    rig.parm("python").set(RIG.replace("SPP", str(SPP)))
    rig.setInput(0, sky)
    rig.cook(force=True)
    errors = sky.errors() + rig.errors()
    if errors:
        raise RuntimeError("%s: %s" % (name, errors))
    layer = rig.stage().Flatten()
    # Drop Houdini's editor bookkeeping; the scene itself is unchanged.
    root = layer.GetPrimAtPath("/HoudiniLayerInfo")
    if root:
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
    layer.documentation = "Built by tests/husk/karma_sky/make_scenes.py (%s, %s)" % (kind, name)
    path = os.path.join(OUT, name + ".usda")
    layer.Export(path)
    print("wrote", path, flush=True)


for name, (kind, params) in CONFIGS.items():
    build(name, kind, params)
print("done")
