"""M6 probe 3: what the Karma Render Settings LOP authors for every AOV, on a
small test scene that exercises the beauty components.

hython, isolated (tests/houdini/run_hython.ps1):

    run_hython.ps1 -Script probe_karma_aovs.py -Stage <stage> <probe_dir>

Writes into <probe_dir>:
  karma_aovs_scene.usda      the flattened stage (scene + /Render), for husk
  render_prims_dump.txt      every prim under /Render and every material
                             render var: type, API schemas, authored attributes
  ksrv_dump.txt              the same for a Karma Standard Render Vars LOP with
                             every toggle on (comparison)
  krs_toggles.json           which toggles were switched on

The scene (built with the USD API in a Python Script LOP, so it does not
depend on LOP parameter names):
  ground plane      Karma Material Builder (kma), grey, with a Karma AOV VOP
                    (kma_aov "myaov" fed by mtlxposition)
  coat sphere       Standard Surface, red base + coat 1
  glass sphere      Standard Surface, transmission 1
  sss sphere        Standard Surface, subsurface 1
  metal sphere      Standard Surface, metalness 1 (geometry LPE tag "metal")
  emitter           small Standard Surface sphere, emission 1
  moving cube       translate samples at frames 0..2 (motion vectors/velocity)
  lights            sphere light "key" (LPE tag key), distant light "fill"
                    (LPE tag fill), dome "env" (untagged)
"""
import json
import os
import sys

import hou
import voptoolutils

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

SCENE_CODE = r'''
from pxr import Gf, Sdf, UsdGeom, UsdLux, UsdShade

node = hou.pwd()
stage = node.editableStage()

cam = UsdGeom.Camera.Define(stage, "/cam")
cam.CreateFocalLengthAttr(35.0)
cam.CreateHorizontalApertureAttr(36.0)
cam.CreateVerticalApertureAttr(20.25)
cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
cam.CreateShutterOpenAttr(-0.25)
cam.CreateShutterCloseAttr(0.25)
xc = UsdGeom.Xformable(cam.GetPrim())
xc.AddTranslateOp().Set(Gf.Vec3d(0.0, 1.6, 8.0))
xc.AddRotateXOp().Set(-8.0)

UsdGeom.Scope.Define(stage, "/World")


def material(name, **inputs):
    mat = UsdShade.Material.Define(stage, "/World/mtl/" + name)
    sh = UsdShade.Shader.Define(stage, "/World/mtl/%s/ss" % name)
    sh.CreateIdAttr("ND_standard_surface_surfaceshader")
    for k, v in inputs.items():
        if isinstance(v, tuple):
            sh.CreateInput(k, Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*v))
        else:
            sh.CreateInput(k, Sdf.ValueTypeNames.Float).Set(float(v))
    out = sh.CreateOutput("out", Sdf.ValueTypeNames.Token)
    mat.CreateSurfaceOutput("mtlx").ConnectToSource(out)
    return mat


def bind(prim, path):
    UsdShade.MaterialBindingAPI.Apply(prim)
    prim.CreateRelationship("material:binding").SetTargets([Sdf.Path(path)])


def sphere(name, pos, radius, mat):
    s = UsdGeom.Sphere.Define(stage, "/World/geo/" + name)
    s.CreateRadiusAttr(radius)
    UsdGeom.Xformable(s.GetPrim()).AddTranslateOp().Set(Gf.Vec3d(*pos))
    bind(s.GetPrim(), mat.GetPath())
    return s


ground = UsdGeom.Mesh.Define(stage, "/World/geo/ground")
ground.CreatePointsAttr([(-10, 0, 10), (10, 0, 10), (10, 0, -10), (-10, 0, -10)])
ground.CreateFaceVertexCountsAttr([4])
ground.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
ground.CreateSubdivisionSchemeAttr("none")
st = UsdGeom.PrimvarsAPI(ground.GetPrim()).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                                                         UsdGeom.Tokens.faceVarying)
st.Set([(0, 0), (1, 0), (1, 1), (0, 1)])
bind(ground.GetPrim(), "/materials/kma_ground")

coat = material("coat", base_color=(0.8, 0.05, 0.05), coat=1.0, specular_roughness=0.5, coat_roughness=0.05)
glass = material("glass", base=0.0, transmission=1.0, specular_roughness=0.0)
sss = material("sss", subsurface=1.0, subsurface_color=(0.9, 0.6, 0.4), subsurface_radius=(1.0, 0.5, 0.25),
               subsurface_scale=0.3)
metal = material("metal", base_color=(0.95, 0.8, 0.5), metalness=1.0, specular_roughness=0.25)
emit = material("emit", base=0.0, emission=1.0, emission_color=(4.0, 1.5, 0.3))
grey = material("grey", base_color=(0.5, 0.5, 0.5))

sphere("coat", (-2.4, 0.7, 0.0), 0.7, coat)
sphere("glass", (-0.8, 0.7, 0.6), 0.7, glass)
sphere("sss", (0.8, 0.7, 0.0), 0.7, sss)
m = sphere("metal", (2.4, 0.7, 0.0), 0.7, metal)
UsdGeom.PrimvarsAPI(m.GetPrim()).CreatePrimvar("karma:object:lpetag", Sdf.ValueTypeNames.String,
                                              UsdGeom.Tokens.constant).Set("metal")
sphere("emitter", (0.0, 0.25, 2.2), 0.25, emit)

cube = UsdGeom.Cube.Define(stage, "/World/geo/mover")
cube.CreateSizeAttr(0.8)
op = UsdGeom.Xformable(cube.GetPrim()).AddTranslateOp()
for f in (0.0, 1.0, 2.0):
    op.Set(Gf.Vec3d(-0.5 + 0.6 * f, 0.4, -1.8), f)
bind(cube.GetPrim(), grey.GetPath())

key = UsdLux.SphereLight.Define(stage, "/World/lights/key")
key.CreateRadiusAttr(0.5)
key.CreateIntensityAttr(100.0)
UsdGeom.Xformable(key.GetPrim()).AddTranslateOp().Set(Gf.Vec3d(3.0, 5.0, 4.0))
key.GetPrim().CreateAttribute("inputs:karma:light:lpetag", Sdf.ValueTypeNames.String).Set("key")

fill = UsdLux.DistantLight.Define(stage, "/World/lights/fill")
fill.CreateIntensityAttr(0.8)
fill.CreateAngleAttr(5.0)
xf = UsdGeom.Xformable(fill.GetPrim())
xf.AddRotateYOp().Set(-35.0)
xf.AddRotateXOp().Set(-50.0)
fill.GetPrim().CreateAttribute("inputs:karma:light:lpetag", Sdf.ValueTypeNames.String).Set("fill")

env = UsdLux.DomeLight.Define(stage, "/World/lights/env")
env.CreateIntensityAttr(0.3)
env.CreateColorAttr(Gf.Vec3f(0.6, 0.7, 1.0))
'''

root = hou.node('/stage')
scene = root.createNode('pythonscript', 'scene')
scene.parm('python').set(SCENE_CODE)

# Karma Material Builder for the ground, with a Karma AOV VOP.
lib = root.createNode('materiallibrary', 'materials')
kb = lib.createNode('subnet', 'kma_ground')
kb.setShaderLanguageName('MaterialX')
voptoolutils._setupMtlXBuilderSubnet(subnet_node=kb, mask=voptoolutils.KARMAMTLX_TAB_MASK,
                                     folder_label='Karma Material Builder', render_context='kma')
print('builder children:', [(c.name(), c.type().name()) for c in kb.children()])
ss = kb.node('mtlxstandard_surface')
ss.parmTuple('base_color').set((0.5, 0.5, 0.5))
out_node = [c for c in kb.children() if c.type().name() in ('subnetconnector', 'suboutput', 'output')
            or 'Output' in c.name()]
print('output candidates:', [(c.name(), c.type().name(), c.inputNames()) for c in out_node])
aov = kb.createNode('kma_aov', 'myaov')
print('kma_aov type:', aov.type().name(), 'inputs', aov.inputNames(), 'outputs', aov.outputNames(),
      'parms', [p.name() for p in aov.parms()])
pos = kb.createNode('mtlxposition', 'pos')
aov.setInput(0, pos, 0)
connected = False
for c in out_node:
    names = c.inputNames()
    for i, n in enumerate(names):
        if 'next' in n.lower() and c.input(i) is None:
            c.setInput(i, aov, 0)
            connected = True
            print('kma_aov connected to %s input %d (%s)' % (c.name(), i, n))
            break
    if connected:
        break
print('kma_aov connected:', connected)

# Karma Cryptomatte LOP upstream of the render settings (its documented default use).
merge = root.createNode('merge', 'merge')
merge.setInput(0, scene)
merge.setInput(1, lib)
crypto = root.createNode('karmacryptomatte', 'crypto')
crypto.setInput(0, merge)
crypto_toggles = []
for t in crypto.parms():
    tmpl = t.parmTemplate()
    if isinstance(tmpl, hou.ToggleParmTemplate):
        crypto_toggles.append((t.name(), tmpl.label(), t.eval()))
print('cryptomatte toggles (name, label, default):', crypto_toggles)
for name in ('doprimcrypto', 'domtlcrypto'):
    if crypto.parm(name):
        crypto.parm(name).set(1)

krs = root.createNode('karmarendersettings', 'krs')
krs.setInput(0, crypto)
krs.parm('camera').set('/cam')
# Resolution mode stays "autoheight": the height (resolutiony) is a locked
# expression of the camera aspect (36 x 20.25 mm -> 270).
krs.parm('resolutionx').set(480)
krs.parm('samplesperpixel').set(16)
krs.parm('picture').set('karma_aovs.exr')

# Every component and utility toggle on; "Split per LPE Tag" only for two.
EXCLUDE = {'doomitlpes', 'dooutputcs', 'channel_lower_rgb', 'hitPzfarclip'}
SPLIT = {'beautyperlpe', 'combineddiffuseperlpe'}
on = []
for p in krs.parms():
    tmpl = p.parmTemplate()
    if not isinstance(tmpl, hou.ToggleParmTemplate):
        continue
    folders = ' > '.join(p.containingFolders())
    if 'Component Level Output' not in folders and 'Utility' not in folders:
        continue
    if p.name() in EXCLUDE or (p.name().endswith('perlpe') and p.name() not in SPLIT):
        continue
    p.set(1)
    on.append((p.name(), tmpl.label(), folders))

# A few extra render vars: presets of each source type plus custom LPEs with tags.
PRESETS = ['st', 'albedodiffuse', 'albedoglossyreflection', 'albedoglossytransmission', 'albedotransmitteddiffuse',
           'basecolor', 'alphatransmission', 'holdout', 'hitdist', 'primarysamples', 'cputime', 'N_camera',
           'Ng_camera', 'CryptoObject', 'CryptoMaterial']
CUSTOM = [  # name, source name, format
    ('metal_glossy', "C<RG'metal'>.*[LO]", 'color3f'),
    ('key_diffuse', "C<RD>.*<L.'key'>", 'color3f'),
    ('fill_all', "C.*<L.'fill'>", 'color3f'),
]
krs.parm('extrarendervars').set(len(PRESETS) + len(CUSTOM))
i = 0
for token in PRESETS:
    i += 1
    krs.parm('name%d' % i).set(token)
    krs.parm('name%d' % i).pressButton()
for name, source, fmt in CUSTOM:
    i += 1
    krs.parm('name%d' % i).set(name)
    krs.parm('sourceType%d' % i).set('lpe')
    krs.parm('sourceName%d' % i).set(source)
    krs.parm('format%d' % i).set(fmt)
    krs.parm('dataType%d' % i).set('color3f')
with open(os.path.join(OUT, 'krs_toggles.json'), 'w', encoding='utf-8') as f:
    json.dump({'toggles_on': on, 'extra_presets': PRESETS, 'custom': CUSTOM,
               'crypto_toggles': crypto_toggles}, f, indent=1)

krs.cook(force=True)
for e in krs.errors() + krs.warnings():
    print('krs message:', e)
usd = krs.stage()


def fmt_value(v):
    s = repr(v)
    return s if len(s) < 400 else s[:400] + '...(%d chars)' % len(s)


def dump_prims(stage, roots, path):
    with open(path, 'w', encoding='utf-8') as f:
        for prim in stage.Traverse():
            p = str(prim.GetPath())
            if not any(p == r or p.startswith(r + '/') for r in roots) and '/Vars/' not in p + '/':
                continue
            f.write('\n%s  (%s)  schemas=%s\n' % (p, prim.GetTypeName(), list(prim.GetAppliedSchemas())))
            for a in prim.GetAuthoredAttributes():
                f.write('    %s %s = %s\n' % (a.GetTypeName(), a.GetName(), fmt_value(a.Get())))
            for r in prim.GetAuthoredRelationships():
                f.write('    rel %s = %s\n' % (r.GetName(), [str(t) for t in r.GetTargets()]))


dump_prims(usd, ['/Render'], os.path.join(OUT, 'render_prims_dump.txt'))
flat = usd.Flatten()
flat.Export(os.path.join(OUT, 'karma_aovs_scene.usda'))

# Comparison: the Karma Standard Render Vars LOP with every toggle on.
ksrv = root.createNode('karmastandardrendervars', 'ksrv')
for p in ksrv.parms():
    if isinstance(p.parmTemplate(), hou.ToggleParmTemplate) and not p.name().endswith('perlpe') \
            and p.name() not in EXCLUDE:
        p.set(1)
ksrv.cook(force=True)
dump_prims(ksrv.stage(), ['/Render'], os.path.join(OUT, 'ksrv_dump.txt'))
print('toggles on:', len(on), 'extras:', i)
print('done')
