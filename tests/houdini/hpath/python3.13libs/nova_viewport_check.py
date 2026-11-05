"""Solaris viewport check of the Nova delegate in a separate Houdini session.

Builds /stage from tests/scenes/basic.usda, looks through /World/cam and
captures the viewport with Houdini GL, Nova (before and after an edit that
adds a prim, a live MaterialX material, a Nova Material Builder material with
Karma's AO, its displacement switched on live, a Principled Shader, and a VEX
Material Builder with an edit) and Karma CPU. Everything
goes to $NOVA_VIEWPORT_CHECK (PNGs and log.json); Houdini quits at the end
without saving.
"""

import json
import os
import time
import traceback

import hou

try:
    from PySide6 import QtCore
except ImportError:  # pragma: no cover
    from PySide2 import QtCore

OUT = os.environ.get("NOVA_VIEWPORT_CHECK", "")
# NOVA_VIEWPORT_CHECK_ONLY=rendersettings runs only the Nova Render Settings
# steps (setup, Houdini GL, Nova, the LOP, Karma); =pyro the volume steps
# (setup_pyro); =m9 hair, SSS, many lights and Nova's geometry properties
# (setup_m9); =xpu Nova XPU in the viewport (setup_xpu); =velocity velocity
# blur switched on and off live (setup_velocity).
ONLY = os.environ.get("NOVA_VIEWPORT_CHECK_ONLY", "")
SCENE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scenes", "basic.usda").replace("\\", "/")
# "started" (absolute) tells this run's log from an earlier one in the same folder.
LOG = {"version": hou.applicationVersionString(), "started": time.time(), "steps": []}
STATE = {}


def save():
    with open(os.path.join(OUT, "log.json"), "w") as f:
        json.dump(LOG, f, indent=2, default=str)


def log(step, **kw):
    LOG["steps"].append(dict(step=step, time=round(time.time() - STATE.get("t0", time.time()), 2), **kw))
    save()


def viewer():
    return hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)


def remove_stale(path):
    """A capture that fails must not leave an earlier run's image under its name
    (image_mean and the log would report it)."""
    try:
        os.remove(path)
    except OSError:
        pass


def guarded(fn):
    def run():
        try:
            fn()
        except Exception:
            log(fn.__name__ + " failed", error=traceback.format_exc())
    return run


def later(seconds, fn):
    QtCore.QTimer.singleShot(int(seconds * 1000), guarded(fn))


def set_renderer(name):
    v = viewer()
    available = list(v.hydraRenderers())
    match = next((n for n in available if n.lower() == name.lower()), None) or \
        next((n for n in available if name.lower() in n.lower()), None)
    if match:
        v.setHydraRenderer(match)
    log("renderer", requested=name, matched=match, current=v.currentHydraRenderer(), available=available)


def capture(name):
    v = viewer()
    path = os.path.join(OUT, name + ".png")
    remove_stale(path)
    settings = v.flipbookSettings().stash()
    frame = hou.frame()
    settings.frameRange((frame, frame))
    settings.output(path)
    v.flipbook(v.curViewport(), settings)
    found = [f for f in os.listdir(OUT) if f.startswith(name)]
    log("capture", name=name, files=found, renderer=v.currentHydraRenderer())


def setup():
    STATE["t0"] = time.time()
    stage = hou.node("/stage")
    sub = stage.createNode("sublayer", "basic")
    parm = sub.parm("filepath1")
    parm.set(SCENE)
    sub.setDisplayFlag(True)
    STATE["sub"] = sub
    v = viewer()
    v.setPwd(stage)
    vp = v.curViewport()
    try:
        vp.setCamera("/World/cam")
        cam = "/World/cam"
    except Exception as e:  # noqa: BLE001
        cam = f"failed: {e}"
    log("setup", stage_prims=[str(p.GetPath()) for p in sub.stage().Traverse()], camera=cam,
        viewport_size=list(vp.size()) if hasattr(vp, "size") else None,
        env={k: v for k, v in os.environ.items() if any(s in k.upper() for s in ("VEX", "PBR", "KARMA", "HOUDINI", "BRAY"))})
    set_renderer("Houdini GL")
    later(4, step_gl)


def step_gl():
    capture("1_gl")
    set_renderer("Nova CPU")
    later(25, step_rendersettings_before if ONLY == "rendersettings" else step_nova)


def render_prims(lop):
    out = {}
    for prim in lop.stage().Traverse():
        if prim.GetTypeName() in ("RenderSettings", "RenderProduct", "RenderVar"):
            attrs = {}
            for a in prim.GetAttributes():
                if a.HasAuthoredValue() and not a.GetName().startswith("nova:"):
                    attrs[a.GetName()] = str(a.Get())
            for r in prim.GetRelationships():
                attrs[r.GetName()] = [str(t) for t in r.GetTargets()]
            out[str(prim.GetPath())] = attrs
    return out


def show_parms(node, tab=None):
    """Shows node in the parameter editors (unpinned), on top tab `tab`."""
    for pt in hou.ui.paneTabs():
        if pt.type() == hou.paneTabType.Parm:
            pt.setPin(False)
            pt.setCurrentNode(node)
    if tab is not None and node.parm("tab_rendering") is not None:
        node.parm("tab_rendering").set(tab)


def grab(name):
    """The whole main window (parameter editor and network included)."""
    path = os.path.join(OUT, name + ".png")
    remove_stale(path)
    hou.qt.mainWindow().grab().save(path)
    log("grab", name=name)


def step_rendersettings_before():
    capture("2_nova")
    # Nova (Setup) after the scene, as the TAB tool builds it: the Nova
    # Render Settings LOP, a render pass and a USD Render ROP.
    import novautils
    settings, rpass, rop = novautils.createSetup(hou.node("/stage"), STATE["sub"])
    settings.setDisplayFlag(True)
    settings.setCurrent(True, clear_all_selected=True)
    show_parms(settings, 0)
    STATE["nova_rs"] = settings
    log("nova setup", nodes=[n.path() for n in (settings, rpass, rop)], errors=list(settings.errors()),
        renderer=rop.parm("renderer").eval(), renderpass=rop.parm("renderpass").eval(), prims=render_prims(settings))
    later(25, step_rendersettings)


def step_rendersettings():
    capture("12_nova_rendersettings")
    grab("ui_nova_setup")
    log("rendersettings means", nova=image_mean("2_nova"), with_lop=image_mean("12_nova_rendersettings"))
    # The all-in-one Nova LOP, as the Karma LOP.
    nova = hou.node("/stage").createNode("nova", "nova")
    nova.setInput(0, STATE["sub"])
    nova.moveToGoodPosition()
    nova.setDisplayFlag(True)
    nova.setCurrent(True, clear_all_selected=True)
    show_parms(nova, 0)
    STATE["nova"] = nova
    later(25, step_nova_lop)


def step_nova_lop():
    capture("14_nova_lop")
    grab("ui_nova_lop")
    show_parms(STATE["nova"], 1)
    later(3, step_nova_lop_aovs)


def step_nova_lop_aovs():
    grab("ui_nova_lop_aovs")
    show_parms(STATE["nova"], 2)
    later(3, step_nova_lop_advanced)


def step_nova_lop_advanced():
    grab("ui_nova_lop_advanced")
    # M7: 8 samples, noisy; then the viewport denoiser (the settings prim's
    # nova:viewportDenoise, as the viewport takes the stage's render settings).
    nova = STATE["nova"]
    nova.parm("samples").set(8)
    later(20, step_nova_noisy)


def step_nova_noisy():
    capture("15_nova_noisy")
    STATE["nova"].parm("viewportDenoise").set(1)
    later(20, step_nova_denoised)


def step_nova_denoised():
    capture("16_nova_denoised")
    log("denoise means", noisy=image_mean("15_nova_noisy"), denoised=image_mean("16_nova_denoised"))
    show_parms(STATE["nova"], 1)
    later(3, step_nova_filters)


def step_nova_filters():
    grab("ui_nova_lop_filters")
    log("nova lop means", with_lop=image_mean("14_nova_lop"))
    set_renderer("Karma CPU")
    later(40, step_rendersettings_karma)


def step_rendersettings_karma():
    capture("13_karma_rendersettings")
    set_renderer("Nova CPU")
    later(2, finish)


def step_nova():
    capture("2_nova")
    # IPR edit: a new prim must appear without restarting Houdini.
    sphere = hou.node("/stage").createNode("sphere", "added")
    sphere.setInput(0, STATE["sub"])
    for name, value in (("tx", 0.0), ("ty", 2.6), ("tz", 0.0)):
        if sphere.parm(name):
            sphere.parm(name).set(value)
    if sphere.parm("scale"):
        sphere.parm("scale").set(0.5)
    sphere.setDisplayFlag(True)
    log("edit", node=sphere.path(), prims=[str(p.GetPath()) for p in sphere.stage().Traverse()][:20])
    later(25, step_edit)


def step_edit():
    capture("3_nova_edit")
    # A MaterialX Standard Surface, assigned live to the added sphere.
    stage = hou.node("/stage")
    lib = stage.createNode("materiallibrary", "mats")
    lib.setInput(0, hou.node("/stage/added"))
    shader = lib.createNode("mtlxstandard_surface", "red")
    shader.parmTuple("base_color").set((0.8, 0.05, 0.05))
    shader.parm("specular_roughness").set(0.3)
    lib.layoutChildren()
    assign = stage.createNode("assignmaterial", "assign")
    assign.setInput(0, lib)
    assign.parm("primpattern1").set("/added")
    assign.parm("matspecpath1").set("/materials/red")
    assign.setDisplayFlag(True)
    STATE["shader"] = shader
    log("material", prims=[str(p.GetPath()) for p in assign.stage().Traverse()][:30])
    later(25, step_material)


def image_mean(name):
    try:
        from PySide6 import QtGui
    except ImportError:  # pragma: no cover
        from PySide2 import QtGui
    files = sorted(f for f in os.listdir(OUT) if f.startswith(name) and f.endswith(".png"))
    if not files:
        return None
    img = QtGui.QImage(os.path.join(OUT, files[0]))
    total, n = [0.0, 0.0, 0.0], 0
    for y in range(0, img.height(), 4):
        for x in range(0, img.width(), 4):
            c = img.pixelColor(x, y)
            total[0] += c.redF(); total[1] += c.greenF(); total[2] += c.blueF()
            n += 1
    return [round(t / max(n, 1), 4) for t in total]


def step_material():
    capture("5_nova_material")
    STATE["shader"].parmTuple("base_color").set((0.05, 0.1, 0.8))  # a value edit: no recompile
    later(20, step_material_edit)


def step_material_edit():
    capture("6_nova_material_edit")
    log("material means", before=image_mean("5_nova_material"), after=image_mean("6_nova_material_edit"))
    # M4.8: a Nova Material Builder (the package's tool module) with Karma's
    # AO driving the base color, assigned live.
    import novautils
    lib = hou.node("/stage/mats")
    builder = novautils.createBuilder(lib, "novamat")
    surface = builder.node("mtlxstandard_surface")
    ao = builder.createNode("kma_ao", "ao")
    ao.parm("maxdistance").set(1.0)
    surface.setNamedInput("base", ao, "visibility")
    surface.parmTuple("base_color").set((0.9, 0.7, 0.3))
    builder.layoutChildren()
    hou.node("/stage/assign").parm("matspecpath1").set("/materials/novamat")
    STATE["builder"] = builder
    log("builder", tool="vop_novamtlxsubnet" in hou.shelves.tools(), module=novautils.__file__,
        mask=builder.parm("tabmenumask").eval())
    later(25, step_builder)


def step_builder():
    capture("7_nova_builder")
    # M4.6: displacement switched on live re-dices the bound sphere.
    disp = STATE["builder"].node("mtlxdisplacement")
    disp.parm("displacement").set(0.15)
    later(30, step_displacement)


def step_displacement():
    capture("8_nova_displacement")
    log("displacement means", before=image_mean("7_nova_builder"), after=image_mean("8_nova_displacement"))
    # M5: a Principled Shader, assigned live (rendered natively).
    lib = hou.node("/stage/mats")
    principled = lib.createNode("principledshader::2.0", "principled")
    principled.parmTuple("basecolor").set((0.2, 0.5, 0.9))
    principled.parm("rough").set(0.3)
    hou.node("/stage/assign").parm("matspecpath1").set("/materials/principled")
    later(25, step_principled)


def step_principled():
    capture("9_nova_principled")
    # M5: a VEX Material Builder (Principled Shader Core with noise roughness),
    # assigned live (through the CVEX bridge).
    lib = hou.node("/stage/mats")
    builder = lib.createNode("materialbuilder", "vexmat")
    core = builder.createNode("principledshadercore", "core")
    core.parmTuple("basecolor").set((0.9, 0.4, 0.1))
    noise = builder.createNode("aanoise", "noise")
    noise.parmTuple("freq").set((3.0, 3.0, 3.0))
    noise.setNamedInput("pos", builder.node("surface_globals"), "P")
    core.setNamedInput("rough", noise, "noise")
    builder.node("surface_output").setNamedInput("F", core, "F")
    builder.node("surface_output").setNamedInput("Of", core, "Of")
    builder.layoutChildren()
    lib.cook(force=True)
    STATE["core"] = core
    log("vex builder cooked", code=len(builder.code() if hasattr(builder, "code") else ""))
    later(15, step_assign_vex)


def step_assign_vex():
    hou.node("/stage/assign").parm("matspecpath1").set("/materials/vexmat")
    log("vex builder assigned")
    later(30, step_vexbuilder)


def step_vexbuilder():
    capture("10_nova_vexbuilder")
    STATE["core"].parmTuple("basecolor").set((0.1, 0.8, 0.2))  # recompiles the builder's code
    later(30, step_vexbuilder_edit)


def step_vexbuilder_edit():
    capture("11_nova_vexbuilder_edit")
    log("vex means", principled=image_mean("9_nova_principled"), builder=image_mean("10_nova_vexbuilder"),
        edit=image_mean("11_nova_vexbuilder_edit"))
    set_renderer("Karma CPU")
    later(40, step_karma)


def step_karma():
    capture("4_karma")
    set_renderer("Nova CPU")
    later(2, finish)


PYRO_VDB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "data", "volumes",
                        "pyro.vdb").replace("\\", "/")


def setup_pyro():
    """M8: the research's pyro sim through an in-memory SOP Import (op:
    fields, as the viewport sees SOP volumes), its density breathing with the
    frame change), with the Karma Pyro Material from Houdini's recipe and a key light."""
    STATE["t0"] = time.time()
    geo = hou.node("/obj").createNode("geo", "pyro_src")
    vdb = geo.createNode("file", "vdb")
    vdb.parm("file").set(PYRO_VDB)
    breathe = geo.createNode("xform", "drift")
    breathe.setInput(0, vdb)
    breathe.parm("tx").setExpression("($F - 1) * 0.4")
    breathe.setDisplayFlag(True)
    breathe.setRenderFlag(True)
    stage = hou.node("/stage")
    imp = stage.createNode("sopimport", "pyro")
    imp.parm("soppath").set(breathe.path())
    lib = stage.createNode("materiallibrary", "pyromats")
    lib.setInput(0, imp)
    from hrecipes import rtypes, storage
    storage.recipeStorage().refForName("sidefx::recipe::vop::kma_pyromaterial",
                                       [rtypes.RecipeCategory.tool]).instanceInsideNode(lib)
    material = [c for c in lib.children() if c.type().name() != "output"][0]
    lib.parm("matnode1").set(material.name())
    volume = next(str(p.GetPath()) for p in imp.stage().Traverse() if p.GetTypeName() == "Volume")
    assign = stage.createNode("assignmaterial", "pyroassign")
    assign.setInput(0, lib)
    assign.parm("primpattern1").set(volume)
    assign.parm("matspecpath1").set("/materials/" + material.name())
    light = stage.createNode("distantlight", "key")
    light.setInput(0, assign)
    if light.parmTuple("r"):
        light.parmTuple("r").set((-30.0, 50.0, 0.0))
    light.setDisplayFlag(True)
    STATE["pyro_shader"] = material.node("kma_pyroshader1")
    v = viewer()
    v.setPwd(stage)
    v.curViewport().frameAll()
    log("pyro setup", volume=volume, material=material.path(), errors=list(assign.errors()),
        fields=[str(p.GetPath()) for p in light.stage().Traverse() if "Asset" in p.GetTypeName()])
    set_renderer("Nova CPU")
    later(30, step_pyro_smoke)


def pyro_view():
    # Parameter edits on the material can move the viewer into its network.
    viewer().setPwd(hou.node("/stage"))


def step_pyro_smoke():
    capture("20_pyro_smoke")
    STATE["pyro_shader"].parm("enablefire").set(1)  # a material edit: the volume's majorants rebuild
    later(25, step_pyro_fire)


def step_pyro_fire():
    pyro_view()
    capture("21_pyro_fire")
    STATE["frame"] = hou.frame()
    hou.setFrame(hou.frame() + 3)  # the SOP recooks: the fields reload
    later(25, step_pyro_frame)


def step_pyro_frame():
    pyro_view()
    capture("22_pyro_frame")
    log("pyro means", smoke=image_mean("20_pyro_smoke"), fire=image_mean("21_pyro_fire"),
        frame=image_mean("22_pyro_frame"), frames=[STATE["frame"], hou.frame()])
    set_renderer("Karma CPU")
    later(40, step_pyro_karma)


def step_pyro_karma():
    pyro_view()
    capture("23_pyro_karma")
    log("pyro karma", karma=image_mean("23_pyro_karma"))
    set_renderer("Nova CPU")
    later(2, finish)


def m9_scene():
    """M9's features in one hand-written stage: a grey ground, a ball of
    linear hair curves with Karma Hair, a subsurface sphere, an emissive quad,
    40 small coloured sphere lights and a dim dome."""
    import math
    import random
    rnd = random.Random(9)

    def fl(v):
        return "(%s)" % ", ".join("%.6g" % x for x in v)

    # Hair: 600 straight fibres out of a sphere at (-1.6, 0.9, 0), radius 0.45.
    pts, counts = [], []
    for _ in range(600):
        z, phi = 2.0 * rnd.random() - 1.0, 2.0 * math.pi * rnd.random()
        s = math.sqrt(max(0.0, 1.0 - z * z))
        n = (s * math.cos(phi), z, s * math.sin(phi))
        root = (-1.6 + 0.45 * n[0], 0.9 + 0.45 * n[1], 0.45 * n[2])
        pts += [root, tuple(root[a] + 0.35 * n[a] for a in range(3))]
        counts.append(2)
    # The subsurface sphere: a uv sphere at (0, 0.6, 0), radius 0.6.
    sp, si, sc = [], [], []
    nu, nv = 48, 24
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu + 1):
            ph = 2.0 * math.pi * i / nu
            sp.append((0.6 * math.sin(th) * math.cos(ph), 0.6 + 0.6 * math.cos(th), 0.6 * math.sin(th) * math.sin(ph)))
    for j in range(nv):
        for i in range(nu):
            a = j * (nu + 1) + i
            si += [a, a + 1, a + nu + 2, a + nu + 1]
            sc.append(4)
    lights = []
    for k in range(40):
        x, z = -3.0 + 6.0 * (k % 10) / 9.0, -1.5 + (k // 10)
        c = (0.4 + 0.6 * rnd.random(), 0.4 + 0.6 * rnd.random(), 0.4 + 0.6 * rnd.random())
        lights.append('''
    def SphereLight "l%d"
    {
        float inputs:radius = 0.03
        float inputs:intensity = 40
        color3f inputs:color = %s
        double3 xformOp:translate = (%g, 2.4, %g)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }''' % (k, fl(c), x, z))
    return '''#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)
def Xform "World"
{
    def Camera "cam"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 24
        double3 xformOp:translate = (0, 1.6, 6)
        float3 xformOp:rotateXYZ = (-10, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
    def Mesh "ground" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-6, 0, 6), (6, 0, 6), (6, 0, -6), (-6, 0, -6)]
        rel material:binding = </World/mtl/grey>
    }
    def BasisCurves "hair" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        uniform token type = "linear"
        int[] curveVertexCounts = [%s]
        point3f[] points = [%s]
        float[] widths = [0.008] (interpolation = "constant")
        rel material:binding = </World/mtl/hair>
    }
    def Mesh "skin" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [%s]
        int[] faceVertexIndices = [%s]
        point3f[] points = [%s]
        rel material:binding = </World/mtl/skin>
    }
    def Mesh "glow" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(1.2, 0.3, -0.4), (2.0, 0.3, -0.4), (2.0, 1.1, -0.4), (1.2, 1.1, -0.4)]
        rel material:binding = </World/mtl/glow>
    }
    def DomeLight "dome"
    {
        float inputs:intensity = 0.08
    }%s
    def Scope "mtl"
    {
        def Material "grey"
        {
            token outputs:mtlx:surface.connect = </World/mtl/grey/s.outputs:out>
            def Shader "s"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color = (0.5, 0.5, 0.5)
                float inputs:specular_roughness = 0.4
                token outputs:out
            }
        }
        def Material "hair"
        {
            token outputs:kma:surface.connect = </World/mtl/hair/h.outputs:surface>
            def Shader "h"
            {
                uniform token info:id = "kma_hair"
                token outputs:surface
            }
        }
        def Material "skin"
        {
            token outputs:mtlx:surface.connect = </World/mtl/skin/s.outputs:out>
            def Shader "s"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                float inputs:subsurface = 1
                color3f inputs:subsurface_color = (0.9, 0.5, 0.35)
                color3f inputs:subsurface_radius = (1, 0.35, 0.2)
                float inputs:subsurface_scale = 0.2
                token outputs:out
            }
        }
        def Material "glow"
        {
            token outputs:mtlx:surface.connect = </World/mtl/glow/s.outputs:out>
            def Shader "s"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                float inputs:base = 0
                float inputs:specular = 0
                float inputs:emission = 1
                color3f inputs:emission_color = (4, 2.5, 1)
                token outputs:out
            }
        }
    }
}
''' % (", ".join(str(c) for c in counts), ", ".join(fl(p) for p in pts), ", ".join(str(c) for c in sc),
       ", ".join(str(i) for i in si), ", ".join(fl(p) for p in sp), "".join(lights))


def geometry_setting(node, key, value):
    """Sets a Nova property on a Render Geometry Settings LOP's Nova tab
    (the generated HdNovaRendererPlugin_Geometry.ds): control "set", value."""
    name = hou.text.encodeParm("primvars:" + key)
    control = node.parm(hou.text.encodeParm("primvars:" + key + "_control"))
    parm = node.parm(name)
    if control is not None:
        control.set("set")
    if parm is not None:
        parm.set(value)
    return control is not None and parm is not None


def setup_m9():
    """M9: hair, SSS, many lights and an emissive quad, then Nova's geometry
    properties from the Render Geometry Settings LOP's Nova tab, live."""
    STATE["t0"] = time.time()
    path = os.path.join(OUT, "m9_scene.usda").replace("\\", "/")
    with open(path, "w") as f:
        f.write(m9_scene())
    stage = hou.node("/stage")
    sub = stage.createNode("sublayer", "m9")
    sub.parm("filepath1").set(path)
    glow = stage.createNode("rendergeometrysettings", "glow_settings")
    glow.setInput(0, sub)
    glow.parm("primpattern").set("/World/glow")
    hair = stage.createNode("rendergeometrysettings", "hair_settings")
    hair.setInput(0, glow)
    hair.parm("primpattern").set("/World/hair")
    hair.setDisplayFlag(True)
    STATE["glow"], STATE["hair"] = glow, hair
    v = viewer()
    v.setPwd(stage)
    vp = v.curViewport()
    try:
        vp.setCamera("/World/cam")
    except Exception:  # noqa: BLE001
        pass
    nova_tab = [p.name() for p in glow.parms() if "nova" in p.name().lower()]
    log("m9 setup", scene=path, nova_parms=len(nova_tab), errors=list(hair.errors()))
    set_renderer("Nova CPU")
    later(40, step_m9_nova)


def step_m9_nova():
    capture("30_m9_nova")
    ok = geometry_setting(STATE["glow"], "nova:object:lightSource", 1)
    prim = STATE["glow"].stage().GetPrimAtPath("/World/glow")
    attr = prim.GetAttribute("primvars:nova:object:lightSource") if prim else None
    log("m9 light source set", parms=ok, authored=attr.Get() if attr else None)
    later(30, step_m9_emitter)


def step_m9_emitter():
    capture("31_m9_emitter_light")
    ok = geometry_setting(STATE["hair"], "nova:object:curveStyle", 0)
    prim = STATE["hair"].stage().GetPrimAtPath("/World/hair")
    attr = prim.GetAttribute("primvars:nova:object:curveStyle") if prim else None
    log("m9 curve style set", parms=ok, authored=attr.Get() if attr else None)
    later(30, step_m9_ribbons)


def step_m9_ribbons():
    capture("32_m9_ribbons")
    log("m9 means", nova=image_mean("30_m9_nova"), emitter_light=image_mean("31_m9_emitter_light"),
        ribbons=image_mean("32_m9_ribbons"))
    set_renderer("Karma CPU")
    later(60, step_m9_karma)


def step_m9_karma():
    capture("33_m9_karma")
    log("m9 karma", karma=image_mean("33_m9_karma"))
    set_renderer("Nova CPU")
    later(2, finish)


def setup_xpu():
    """M10: the viewport with Nova XPU: the scene, the Nova LOP set to XPU
    with a sample count it does not reach (the viewport shows the GPU film
    while it renders), a prim added live, then Nova on the CPU."""
    STATE["t0"] = time.time()
    stage = hou.node("/stage")
    sub = stage.createNode("sublayer", "basic")
    sub.parm("filepath1").set(SCENE)
    sub.setDisplayFlag(True)
    STATE["sub"] = sub
    v = viewer()
    v.setPwd(stage)
    try:
        v.curViewport().setCamera("/World/cam")
    except Exception:  # noqa: BLE001
        pass
    set_renderer("Nova XPU")
    later(20, step_xpu)


def step_xpu():
    capture("40_xpu")
    nova = hou.node("/stage").createNode("nova", "nova")
    nova.setInput(0, STATE["sub"])
    nova.parm("engine").set("xpu")
    nova.parm("samples").set(100000)
    nova.setDisplayFlag(True)
    nova.setCurrent(True, clear_all_selected=True)
    show_parms(nova, 0)
    STATE["nova"] = nova
    prim = nova.stage().GetPrimAtPath("/Render/rendersettings")
    log("xpu lop", renderer=nova.node("render").parm("renderer").eval(),
        delegate=str(prim.GetAttribute("husk:default_delegate").Get()) if prim else None)
    later(15, step_xpu_progress)


def step_xpu_progress():
    capture("41_xpu_progress_a")
    grab("ui_xpu_lop")
    later(5, step_xpu_progress_b)


def step_xpu_progress_b():
    capture("42_xpu_progress_b")
    # A prim added live while the render goes on.
    sphere = hou.node("/stage").createNode("sphere", "added")
    sphere.setInput(0, STATE["sub"])
    for name, value in (("tx", 0.0), ("ty", 2.6), ("tz", 0.0)):
        if sphere.parm(name):
            sphere.parm(name).set(value)
    if sphere.parm("scale"):
        sphere.parm("scale").set(0.5)
    STATE["nova"].setInput(0, sphere)
    later(15, step_xpu_edit)


def step_xpu_edit():
    capture("43_xpu_edit")
    set_renderer("Nova CPU")
    later(30, step_xpu_cpu)


def step_xpu_cpu():
    capture("44_nova_cpu")
    log("xpu means", xpu=image_mean("40_xpu"), progress_a=image_mean("41_xpu_progress_a"),
        progress_b=image_mean("42_xpu_progress_b"), edit=image_mean("43_xpu_edit"), cpu=image_mean("44_nova_cpu"))
    later(2, finish)


def viewport_colour(name):
    """The share of the viewport's pixels in a grab that show the scene's
    orange sphere (a cleared buffer shows the grey background instead)."""
    try:
        from PySide6 import QtGui
    except ImportError:  # pragma: no cover
        from PySide2 import QtGui
    img = QtGui.QImage(os.path.join(OUT, name + ".png"))
    orange, n = 0, 0
    for y in range(0, img.height(), 3):
        for x in range(0, img.width(), 3):
            c = img.pixelColor(x, y)
            n += 1
            orange += c.redF() > c.greenF() + 0.2 and c.redF() > c.blueF() + 0.25
    return round(orange / max(n, 1), 5)


def setup_tumble():
    """The viewport while the camera moves (a Transform LOP on the camera,
    edited every 40 ms as a tumble would): grabs of the window during the
    motion must show the scene, not the cleared buffers."""
    STATE["t0"] = time.time()
    stage = hou.node("/stage")
    sub = stage.createNode("sublayer", "basic")
    sub.parm("filepath1").set(SCENE)
    xform = stage.createNode("xform", "orbit")
    xform.setInput(0, sub)
    xform.parm("primpattern").set("/World/cam")
    xform.setDisplayFlag(True)
    STATE["xform"] = xform
    v = viewer()
    v.setPwd(stage)
    try:
        v.curViewport().setCamera("/World/cam")
    except Exception:  # noqa: BLE001
        pass
    set_renderer(os.environ.get("NOVA_VIEWPORT_CHECK_RENDERER", "Nova CPU"))
    later(20, step_tumble)


def step_tumble():
    grab("50_still")
    STATE["tick"] = 0
    STATE["grabs"] = []
    timer = QtCore.QTimer()
    timer.setInterval(40)

    def tick():
        try:
            STATE["tick"] += 1
            angle = 0.4 * STATE["tick"]
            STATE["xform"].parmTuple("r").set((0.0, angle, 0.0))
            if STATE["tick"] in (40, 60, 80):
                name = "51_moving_%d" % STATE["tick"]
                grab(name)
                STATE["grabs"].append(name)
            if STATE["tick"] >= 90:
                timer.stop()
                later(10, step_tumble_after)
        except Exception:
            timer.stop()
            log("tumble failed", error=traceback.format_exc())
            later(1, finish)

    timer.timeout.connect(tick)
    STATE["timer"] = timer
    timer.start()


def step_tumble_after():
    grab("52_released")
    log("tumble", still=viewport_colour("50_still"),
        moving=[viewport_colour(n) for n in STATE["grabs"]], released=viewport_colour("52_released"),
        renderer=viewer().currentHydraRenderer())
    later(2, finish)


def sharp_share(name):
    """The share of a capture's pixels as dark as the grey quads themselves:
    a quad that blurs mixes with the white dome behind it and leaves it."""
    try:
        from PySide6 import QtGui
    except ImportError:  # pragma: no cover
        from PySide2 import QtGui
    img = QtGui.QImage(os.path.join(OUT, name + ".png"))
    values = [img.pixelColor(x, y).valueF() for y in range(0, img.height(), 2) for x in range(0, img.width(), 2)]
    if not values:
        return None
    lo, hi = min(values), max(values)
    return round(sum(v < lo + 0.25 * (hi - lo) for v in values) / len(values), 5)


def setup_velocity():
    """Velocity blur (docs/design/M8.md D19): tests/scenes/motion_velocity.usda
    through /World/cam, then the Render Geometry Settings LOP's Nova tab
    switches velocity blur on and off live for the quad without a setting."""
    STATE["t0"] = time.time()
    stage = hou.node("/stage")
    sub = stage.createNode("sublayer", "velocity")
    scene = os.path.join(os.path.dirname(SCENE), "motion_velocity.usda").replace("\\", "/")
    sub.parm("filepath1").set(scene)
    geo = stage.createNode("rendergeometrysettings", "velocity_settings")
    geo.setInput(0, sub)
    geo.parm("primpattern").set("/World/still")
    geo.setDisplayFlag(True)
    STATE["geo"] = geo
    hou.setFrame(1)
    v = viewer()
    v.setPwd(stage)
    try:
        v.curViewport().setCamera("/World/cam")
    except Exception:  # noqa: BLE001
        pass
    log("velocity setup", scene=scene, errors=list(geo.errors()), fps=hou.fps())
    set_renderer("Nova CPU")
    later(30, step_velocity_before)


def step_velocity_before():
    capture("60_velocity_before")
    ok = geometry_setting(STATE["geo"], "nova:object:velocityBlur", 1)
    prim = STATE["geo"].stage().GetPrimAtPath("/World/still")
    attr = prim.GetAttribute("primvars:nova:object:velocityBlur") if prim else None
    log("velocity blur on", parms=ok, authored=attr.Get() if attr else None)
    later(25, step_velocity_on)


def step_velocity_on():
    capture("61_velocity_on")
    STATE["geo"].parm(hou.text.encodeParm("primvars:nova:object:velocityBlur")).set(0)
    prim = STATE["geo"].stage().GetPrimAtPath("/World/still")
    attr = prim.GetAttribute("primvars:nova:object:velocityBlur") if prim else None
    log("velocity blur off", authored=attr.Get() if attr else None)
    later(25, step_velocity_off)


def step_velocity_off():
    capture("62_velocity_off")
    # Velocity blur on: the quad leaves the sharp pixels; off: it comes back.
    log("velocity sharp shares", before=sharp_share("60_velocity_before"), on=sharp_share("61_velocity_on"),
        off=sharp_share("62_velocity_off"))
    set_renderer("Karma CPU")
    later(45, step_velocity_karma)


def step_velocity_karma():
    capture("63_velocity_karma")
    log("velocity karma", karma=sharp_share("63_velocity_karma"))
    set_renderer("Nova CPU")
    later(2, finish)


def finish():
    log("done")
    hou.exit(exit_code=0, suppress_save_prompt=True)


def start():
    if not OUT:
        return
    os.makedirs(OUT, exist_ok=True)
    modes = {"pyro": setup_pyro, "m9": setup_m9, "xpu": setup_xpu, "tumble": setup_tumble,
             "velocity": setup_velocity}
    later(5, modes.get(ONLY, setup))
