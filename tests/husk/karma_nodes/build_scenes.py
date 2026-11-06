"""Builds the Karma node scenes of tests/husk/check_karma_nodes.py with the
actual Karma LOPs in an isolated hython: a base stage (ground, ball, top
camera, sun, dome) from a Python LOP, then the Karma LOP under test, then a
Karma Render Settings LOP. Writes cleaned USDA (no customData, no Houdini
layer info) to argv[1]. The Background Plates read ./plate.pic (the check
copies Houdini's Mandril.pic there).

  tests/houdini/run_hython.ps1 -Script tests/husk/karma_nodes/build_scenes.py <out dir>
"""
import os
import sys

import hou
from pxr import Sdf, Usd

sys.stdout.reconfigure(encoding="utf-8")
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
stage_net = hou.node("/stage")

BASE = r'''
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade
stage = hou.pwd().editableStage()
g = UsdGeom.Mesh.Define(stage, "/World/ground")
g.CreatePointsAttr([(-6, 0, 6), (6, 0, 6), (6, 0, -6), (-6, 0, -6)])
g.CreateFaceVertexCountsAttr([4])
g.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
g.CreateNormalsAttr([(0, 1, 0)] * 4)
g.SetNormalsInterpolation("vertex")
g.CreateExtentAttr([(-6, 0, -6), (6, 0, 6)])
g.CreateSubdivisionSchemeAttr("none")
b = UsdGeom.Sphere.Define(stage, "/World/ball")
b.CreateRadiusAttr(1.0)
b.AddTranslateOp().Set(Gf.Vec3d(0, 1, 0))
m = UsdShade.Material.Define(stage, "/World/m/grey")
s = UsdShade.Shader.Define(stage, "/World/m/grey/s")
s.CreateIdAttr("ND_standard_surface_surfaceshader")
s.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.5, 0.5, 0.5))
s.CreateInput("specular", Sdf.ValueTypeNames.Float).Set(0.0)
m.CreateSurfaceOutput("mtlx").ConnectToSource(s.ConnectableAPI(), "out")
for p in ("/World/ground", "/World/ball"):
    UsdShade.MaterialBindingAPI.Apply(stage.GetPrimAtPath(p)).Bind(m)
c = UsdGeom.Camera.Define(stage, "/cam")
c.CreateProjectionAttr("orthographic")
c.CreateHorizontalApertureAttr(80.0)
c.CreateVerticalApertureAttr(80.0)
c.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100))
c.AddTranslateOp().Set(Gf.Vec3d(0, 10, 0))
c.AddRotateXYZOp().Set(Gf.Vec3f(-90, 0, 0))
sun = UsdLux.DistantLight.Define(stage, "/lights/sun")
sun.CreateAngleAttr(0.53)
sun.CreateIntensityAttr(3.0)
sun.CreateNormalizeAttr(True)
sun.AddRotateXYZOp().Set(Gf.Vec3f(-45, 0, 0))
dome = UsdLux.DomeLight.Define(stage, "/lights/dome")
dome.CreateIntensityAttr(0.3)
'''


def clean_export(lop, path):
    lop.cook(force=True)
    errs = lop.errors()
    if errs:
        print("ERRORS", path, errs, flush=True)
    flat = lop.stage().Flatten()
    layer = Sdf.Layer.CreateAnonymous(".usda")
    layer.TransferContent(flat)
    st = Usd.Stage.Open(layer)
    info = st.GetPrimAtPath("/HoudiniLayerInfo")
    if info:
        st.RemovePrim(info.GetPath())
    for prim in st.Traverse():
        prim.ClearCustomData()
        for prop in prim.GetProperties():
            prop.ClearCustomData()
        if prim.GetTypeName() == "RenderProduct":
            a = prim.GetAttribute("productName")
            a.Clear()
            a.Set("out.exr")
    layer.customLayerData = {}
    layer.documentation = ""
    layer.Export(path)
    print("wrote", path, flush=True)


def build(name, karma_node=None, setup=None, settings=None, exact=False):
    base = stage_net.createNode("pythonscript", name + "_base")
    base.parm("python").set(BASE)
    last = base
    if karma_node:
        n = stage_net.createNode(karma_node, name + "_karma", exact_type_name=exact)
        print(name, "node type", n.type().name())
        n.setInput(0, last)
        if setup:
            setup(n)
        last = n
    rs = stage_net.createNode("karmarendersettings", name + "_rs")
    rs.setInput(0, last)
    rs.parm("camera").set("/cam")
    # The prim the Background Plate authors its settings on (its default and
    # the Karma ROP's).
    rs.parm("primpath").set("/Render/rendersettings")
    rs.parm("picture").set("out.exr")
    items = rs.parm("res_mode").menuItems()
    print("res_mode items", items)
    for it in items:
        if it in ("manual", "exact", "specific", "none", "off") or "manual" in it:
            rs.parm("res_mode").set(it)
            break
    rs.parm("resolutionx").set(96)
    try:
        rs.parm("resolutiony").set(96)
    except hou.PermissionError:
        print("resolutiony locked; mode", rs.parm("res_mode").evalAsString())
    rs.parm("pathtracedsamples").set(256)
    rs.parm("samplesperpixel").set(256)
    rs.parm("oracle").set("uniform")
    rs.parm("pixelfilter").set("box")
    if settings:
        settings(rs)
    clean_export(rs, os.path.join(OUT, name + ".usda"))
    return rs


def sc_setup(block):
    def f(n):
        n.parm("primpattern").set("/World/ground")
        n.parm("blocklights").set(block)
    return f


def bp_setup(n):
    n.parm("primpattern").set("/World/ground")
    n.parm("plate").set("./plate.pic")


print([t for t in hou.lopNodeTypeCategory().nodeTypes() if "background" in t or "shadowcatch" in t])
build("shadowcatcher", "shadowcatcher", sc_setup(0))
build("shadowcatcher_block", "shadowcatcher", sc_setup(1))
build("backgroundplate2", "backgroundplate::2.0", bp_setup)
try:
    build("backgroundplate1", "backgroundplate", bp_setup, exact=True)
except Exception as e:  # noqa: BLE001
    print("no backgroundplate 1.0:", e)
build("rs_holdout_shadows", "shadowcatcher", sc_setup(0),
      lambda rs: (rs.parm("holdout_shadows").set(1), rs.parm("holdout_shadow_alpha").set(0.5)))

# The render settings dump (which properties the holdout switches author).
for name in ("shadowcatcher", "rs_holdout_shadows"):
    st = Usd.Stage.Open(os.path.join(OUT, name + ".usda"))
    for prim in st.Traverse():
        if prim.GetTypeName() in ("RenderSettings", "RenderVar", "RenderProduct"):
            for a in prim.GetAttributes():
                if a.HasAuthoredValue() and ("holdout" in a.GetName() or "background" in a.GetName()
                                             or "holdout" in str(a.Get())):
                    print(name, prim.GetPath(), a.GetName(), repr(a.Get())[:200])
