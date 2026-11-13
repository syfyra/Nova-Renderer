"""Nova's Solaris UI (docs/design/M4.md 3.7, M4.7), in hython in an isolated
environment (see run_hython.ps1; ctest runs it through check_solaris_ui.py):

  * the Nova tabs exist on the Render Settings and Render Geometry Settings
    LOPs, and every delegate setting descriptor has a parameter with the
    same default;
  * Nova XPU is a renderer too, with its own viewport file; husk renders
    with it;
  * the committed .ds files are what gen_nova_ds.py makes of the table;
  * authoring is sparse (Do Nothing authors nothing), per-object
    properties are constant primvars;
  * the viewport file parses, and its names are table keys;
  * the Nova Render Settings LOP authors every setting and a complete render
    setup that husk renders with Nova; its AOV toggles author the render vars
    of Karma's Standard Render Vars LOP (M6.6) and the cryptomattes;
  * its layout follows the Karma Render Settings LOP (M6.7); the Nova LOP
    (settings and render buttons in one node, as the Karma LOP) renders to
    disk with Nova; the Nova (Setup) tool builds settings, render pass and a
    USD Render ROP set to Nova, as Karma (Setup) does;
  * Nova's LOPs and tools show Nova's icon (the package's NOVA_logo), not
    Karma's.

Usage: hython test_solaris_ui.py <repo> <outdir>
"""

import json
import os
import subprocess
import sys
import tempfile

import hou
from pxr import Sdf, UsdGeom, UsdImagingGL

REPO, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
enc, dec = hou.text.encodeParm, hou.text.decodeParm
FAILURES = []


def check(cond, message):
    print(('ok   ' if cond else 'FAIL ') + message)
    if not cond:
        FAILURES.append(message)


with open(os.path.join(REPO, 'houdini', 'tools', 'nova_settings.json')) as f:
    TABLE = json.load(f)['settings']
BY_KEY = {s['key']: s for s in TABLE}

# ---- The delegate is there, its parameter files come first.
check('HdNovaRendererPlugin' in hou.lop.availableRendererNames(), 'Nova is an available renderer')
check('HdNovaXpuRendererPlugin' in hou.lop.availableRendererNames(), 'Nova XPU is an available renderer')
soho = hou.houdiniPath('HOUDINI_SOHO_PATH')
check(bool(soho) and soho[0].replace('\\', '/').endswith('/nova/soho'), 'the staged soho directory comes first: %s' % soho[:1])

stage = hou.node('/stage')
cube = stage.createNode('cube')
geo = stage.createNode('rendergeometrysettings')
geo.setInput(0, cube)
rs = stage.createNode('rendersettings')
rs.setInput(0, geo)
check(rs.parmTemplateGroup().findFolder('Nova') is not None, 'the Render Settings LOP has a Nova tab')
check(geo.parmTemplateGroup().findFolder('Nova') is not None, 'the Render Geometry Settings LOP has a Nova tab')

# ---- Every descriptor: a parameter with the delegate's default (UsdImagingGL
# lists int, float, bool and string descriptors; tokens come from the table).
params = UsdImagingGL.Engine.Parameters()
params.gpuEnabled = False
engine = UsdImagingGL.Engine(params)
check(engine.SetRendererPlugin('HdNovaRendererPlugin'), 'UsdImagingGL loads Nova')
descriptors = engine.GetRendererSettingsList()
check(len(descriptors) >= 20, '%d descriptors listed' % len(descriptors))
for d in descriptors:
    parm = rs.parm(enc(d.key))
    check(parm is not None, 'Render Settings LOP parameter for %s' % d.key)
    if parm is None:
        continue
    dv = parm.parmTemplate().defaultValue()
    value = dv if isinstance(dv, bool) else dv[0]
    want = d.defValue
    if isinstance(want, float):
        check(abs(float(value) - want) < 1e-6, '%s default %s == %s' % (d.key, value, want))
    else:
        check(value == want, '%s default %r == %r' % (d.key, value, want))
    check(d.key in BY_KEY, '%s is in the settings table' % d.key)
for s in TABLE:
    if 'global' in s['scopes']:
        # A colour is a tuple of three parameters under the key's name.
        tup = rs.parmTuple(enc(s['key']))
        check(tup is not None and len(tup) == (3 if s['type'] == 'color' else 1), 'table key %s has a parameter' % s['key'])
        if tup is not None and s['type'] == 'color':
            check(all(abs(a - b) < 1e-6 for a, b in zip(tup.parmTemplate().defaultValue(), s['default'])),
                  '%s default %s' % (s['key'], s['default']))
    if 'geometry' in s['scopes']:
        check(geo.parmTuple(enc('primvars:' + s['key'])) is not None, 'table key %s has a per-object parameter' % s['key'])

# ---- The committed files are the generator's output.
tmp = tempfile.mkdtemp(prefix='nova_ds_')
gen = os.path.join(REPO, 'houdini', 'tools', 'gen_nova_ds.py')
sys.argv = [gen, tmp]
with open(gen) as f:
    exec(compile(f.read(), gen, 'exec'), {'__name__': '__main__', '__file__': gen})
for fn in ['HdNovaRendererPlugin_%s.ds' % name for name in ('Global', 'Geometry', 'Viewport')] + [
        'HdNovaXpuRendererPlugin_Viewport.ds']:
    with open(os.path.join(tmp, fn)) as a, open(os.path.join(REPO, 'houdini', 'package', 'soho', 'parameters', fn)) as b:
        check(a.read() == b.read(), '%s is up to date with the table' % fn)

# ---- Sparse authoring: only "Set" controls author.
rs.parm(enc('nova:samples_control')).set('set')
rs.parm(enc('nova:samples')).set(16)
rs.parm(enc('nova:pixelFilter_control')).set('set')
rs.parm(enc('nova:pixelFilter')).set('box')
rs.parm(enc('nova:maxDepth')).set(3)  # control "Do Nothing"
prim = rs.stage().GetPrimAtPath('/Render/rendersettings')
check(prim.GetAttribute('nova:samples').Get() == 16, 'nova:samples authored')
check(str(prim.GetAttribute('nova:pixelFilter').GetTypeName()) == 'token', 'nova:pixelFilter is a token')
check(not prim.GetAttribute('nova:maxDepth').HasAuthoredValue(), 'a Do Nothing control authors nothing')

# ---- Per-object properties: constant primvars.
geo.parm('primpattern').set('/cube1')
for key, value in (('nova:subdivLevel', 2), ('nova:object:diffuseLimit', 2.5), ('nova:dicingQuality', 1.0)):
    geo.parm(enc('primvars:' + key + '_control')).set('set')
    geo.parm(enc('primvars:' + key)).set(value)
api = UsdGeom.PrimvarsAPI(geo.stage().GetPrimAtPath('/cube1'))
for key, value in (('nova:subdivLevel', 2), ('nova:object:diffuseLimit', 2.5), ('nova:dicingQuality', 1.0)):
    pv = api.GetPrimvar(key)
    check(pv.IsDefined() and pv.Get() == value and pv.GetInterpolation() == 'constant',
          'primvars:%s = %s (constant)' % (key, value))

# ---- The viewport file parses; its names are table keys with the viewport scope.
with open(os.path.join(REPO, 'houdini', 'package', 'soho', 'parameters', 'HdNovaRendererPlugin_Viewport.ds')) as f:
    text = f.read()
group = hou.ParmTemplateGroup()
group.setToDialogScript(text)
names = []
for t in group.entriesWithoutFolders() if hasattr(group, 'entriesWithoutFolders') else []:
    names.append(t.name())
if not names:
    def walk(entries):
        for t in entries:
            if isinstance(t, hou.FolderParmTemplate):
                walk(t.parmTemplates())
            else:
                names.append(t.name())
    walk(group.entries())
check(len(names) == len([s for s in TABLE if 'viewport' in s['scopes']]), 'viewport parameters: %d' % len(names))
for n in names:
    key = dec(n)
    check(key in BY_KEY and 'viewport' in BY_KEY[key]['scopes'], 'viewport parameter %s is a viewport setting' % key)

# ---- The Nova Render Settings LOP.
check(hou.nodeType(hou.lopNodeTypeCategory(), 'nova_rendersettings') is not None, 'the Nova Render Settings LOP exists')
cam = stage.createNode('camera')
nova = stage.createNode('nova_rendersettings')
nova.setInput(0, cam)
cube2 = stage.createNode('cube', 'box')
merge = stage.createNode('merge')
merge.setInput(0, nova)
merge.setInput(1, cube2)
nova.parm('camera').set('/cameras/camera1')  # the camera LOP's default path
nova.parmTuple('resolution').set((64, 48))
nova.parm('samples').set(4)
nova.parm('object_diffuseLimit').set(2.0)
picture = os.path.join(OUT, 'nova_hda.exr').replace('\\', '/')
nova.parm('picture').set(picture)
# M6.6: by default the beauty only.
prod = merge.stage().GetPrimAtPath('/Render/Products/nova')
check([str(t) for t in prod.GetRelationship('orderedVars').GetTargets()] == ['/Render/Products/Vars/beauty'],
      'by default the product has the beauty only')
# AOV toggles: the same render vars as Karma's Standard Render Vars LOP.
AOVS = ('directdiffuse', 'combinedglossyreflection', 'shadow', 'hitP', 'primid', 'motionvectors')
for name in AOVS + ('cryptoprimitives',):
    nova.parm(name).set(1)
ksrv = stage.createNode('karmastandardrendervars', 'ksrv_reference')
for parm in ksrv.parms():
    if parm.parmTemplate().type() == hou.parmTemplateType.Toggle and not parm.name().endswith(('outputcs', 'perlpe')):
        parm.set(1 if parm.name() in AOVS + ('beauty',) else 0)
ksrv.parm('beautyprecision').set('color4f')
ref = ksrv.stage()
st = merge.stage()
for name in AOVS:
    mine, karma = st.GetPrimAtPath('/Render/Products/Vars/' + name), ref.GetPrimAtPath('/Render/Products/Vars/' + name)
    same = mine.IsValid() and karma.IsValid() and all(
        mine.GetAttribute(attr.GetName()).Get() == attr.Get() for attr in karma.GetAttributes() if attr.HasAuthoredValue())
    check(same, "the %s var equals Karma's" % name)
targets = [str(t) for t in st.GetPrimAtPath('/Render/Products/nova').GetRelationship('orderedVars').GetTargets()]
check(targets[:1] == ['/Render/Products/Vars/beauty'] and '/Render/Products/Vars/CryptoPrimitives' in targets and
      all('/Render/Products/Vars/' + n in targets for n in AOVS), 'the product has the beauty first and the AOVs: %s' % targets)
crypto = st.GetPrimAtPath('/Render/Products/Vars/CryptoPrimitives')
check(crypto.IsValid() and crypto.GetAttribute('driver:parameters:aov:karma:cryptomatte').Get() == 1 and
      crypto.GetAttribute('sourceName').Get() == '__name', "the cryptomatte var is Karma's")
p = st.GetPrimAtPath('/Render/rendersettings')
check(p.IsValid(), 'the HDA authors /Render/rendersettings')
if p.IsValid():
    check(p.GetAttribute('nova:samples').Get() == 4, 'nova:samples = 4')
    check(abs(p.GetAttribute('nova:object:diffuseLimit').Get() - 2.0) < 1e-6, 'nova:object:diffuseLimit = 2')
    check(p.GetAttribute('nova:maxDepth').Get() == 64, 'nova:maxDepth authored explicitly (64)')
    check(p.GetAttribute('nova:pixelFilter').Get() == 'gauss', 'nova:pixelFilter = gauss')
    check(not p.GetAttribute('nova:device').HasAuthoredValue(), 'nova:device is left to the renderer (hda: false)')
    check(p.GetAttribute('husk:default_delegate').Get() == 'HdNovaRendererPlugin', 'husk:default_delegate: Nova')
    check(tuple(p.GetAttribute('resolution').Get()) == (64, 48), 'resolution 64 x 48')
    check([str(t) for t in p.GetRelationship('camera').GetTargets()] == ['/cameras/camera1'], 'camera /cameras/camera1')
    check([str(t) for t in p.GetRelationship('products').GetTargets()] == ['/Render/Products/nova'], 'products')
prod = st.GetPrimAtPath('/Render/Products/nova')
check(prod.IsValid() and prod.GetAttribute('productName').Get() == picture, 'the product writes %s' % picture)

# husk renders the HDA's stage with Nova.
usd = os.path.join(OUT, 'nova_hda.usda')
st.Export(usd)
hfs = os.environ['HFS']


def remove_stale(path):
    """OUT persists between ctest runs: an image left by an earlier run would
    pass the checks below although this run wrote nothing."""
    if os.path.exists(path):
        os.remove(path)


remove_stale(picture)
try:
    proc = subprocess.run([os.path.join(hfs, 'bin', 'husk.exe'), '--renderer', 'Nova', '--settings', '/Render/rendersettings',
                           usd], capture_output=True, text=True, cwd=OUT, timeout=240)
except subprocess.TimeoutExpired:
    proc = subprocess.CompletedProcess([], -1, '', 'timeout')
check(proc.returncode == 0, 'husk renders the HDA stage (exit %d)' % proc.returncode)
if proc.returncode != 0:
    print(proc.stdout[-2000:], proc.stderr[-2000:])
check(os.path.exists(picture), 'husk wrote the product')
# Nova XPU through husk: the GPU, or the CPU with a warning without one.
picture_xpu = os.path.join(OUT, 'nova_hda_xpu.exr').replace('\\', '/')
remove_stale(picture_xpu)
try:
    proc_xpu = subprocess.run([os.path.join(hfs, 'bin', 'husk.exe'), '--renderer', 'Nova XPU', '--settings',
                               '/Render/rendersettings', '-o', picture_xpu, usd], capture_output=True, text=True,
                              cwd=OUT, timeout=240)
except subprocess.TimeoutExpired:
    proc_xpu = subprocess.CompletedProcess([], -1, '', 'timeout')
log = proc_xpu.stdout + proc_xpu.stderr
check(proc_xpu.returncode == 0 and os.path.exists(picture_xpu), 'husk renders with Nova XPU (exit %d)' % proc_xpu.returncode)
check('[Nova] XPU: rendering on' in log or '[Nova] XPU: no usable GPU' in log, 'Nova XPU names its device: %s' %
      [l for l in log.splitlines() if 'XPU' in l][:2])
if os.path.exists(picture):
    info = subprocess.run([os.path.join(hfs, 'bin', 'iinfo.exe'), '-v', picture], capture_output=True, text=True).stdout
    check('64 x 48' in info, 'the image is 64 x 48')
    check('samples (type int):= 4' in info, 'rendered with 4 samples')
    import OpenImageIO as oiio
    image = oiio.ImageInput.open(picture)
    parts, sub = [], 0
    while image.seek_subimage(sub, 0):
        parts.append(image.spec().getattribute('name'))
        sub += 1
    image.close()
    want = ['C', 'directdiffuse', 'combinedglossyreflection', 'shadow', 'P', 'primid', 'motionvectors', 'CryptoPrimitives',
            'CryptoPrimitives00', 'CryptoPrimitives01', 'CryptoPrimitives02']
    check(all(w in parts for w in want) and parts[:1] == ['C'], 'the EXR parts %s include %s' % (parts, want))

# ---- 0.2.0: the parameter kinds that came with the new settings author their
# USD types: a colour, a text field with a menu, a token menu, an int menu.
kinds = stage.createNode('nova_rendersettings', 'nova_kinds')
kinds.parmTuple('outlineColor').set((0.1, 0.2, 0.3))
kinds.parm('object_renderVisibility').set('-primary')
kinds.parm('lightOverride').set('headlight')
kinds.parm('object_velocityBlur').set(2)
kinds.parm('timeLimit').set(1.5)
kp = kinds.stage().GetPrimAtPath('/Render/rendersettings')


def authored(key, type_name, value):
    attr = kp.GetAttribute(key)
    got = attr.Get() if attr else None
    same = got is not None and (all(abs(a - b) < 1e-6 for a, b in zip(got, value)) if isinstance(value, tuple) else got == value)
    check(bool(attr) and str(attr.GetTypeName()) == type_name and same,
          'the Nova LOP authors %s %s = %s (%s %s)' % (type_name, key, value, attr.GetTypeName() if attr else None, got))


authored('nova:outlineColor', 'color3f', (0.1, 0.2, 0.3))
authored('nova:fogColor', 'color3f', (0.0, 0.0, 0.0))
authored('nova:object:renderVisibility', 'string', '-primary')
authored('nova:lightOverride', 'token', 'headlight')
authored('nova:object:velocityBlur', 'int', 2)
authored('nova:object:curveStyle', 'int', 1)
authored('nova:timeLimit', 'float', 1.5)
authored('nova:simplifiedShading', 'bool', False)
menu = kinds.parm('object_renderVisibility').parmTemplate()
check(menu.menuType() == hou.menuType.StringReplace and '-primary' in menu.menuItems(),
      'Render Visibility is a text field with a menu of presets')
check(kinds.parm('object_velocityBlur').parmTemplate().menuLabels() == ('No Velocity Blur', 'Velocity Blur', 'Acceleration Blur'),
      'Velocity Blur is a menu')
# The output groups of the Karma LOP: framing, husk's EXR options, a deep
# camera map and checkpoints as products of their own.
kproduct = kinds.stage().GetPrimAtPath('/Render/Products/nova')
check([str(t) for t in kp.GetRelationship('products').GetTargets()] == ['/Render/Products/nova'],
      'by default the settings render the picture\'s product alone')
check(kproduct.GetAttribute('driver:parameters:husk:OpenEXR:compression').Get() == 'zips' and
      kp.GetAttribute('aspectRatioConformPolicy').Get() == 'expandAperture' and
      tuple(kp.GetAttribute('dataWindowNDC').Get()) == (0.0, 0.0, 1.0, 1.0) and kp.GetAttribute('pixelAspectRatio').Get() == 1.0,
      'the output defaults are husk\'s and UsdRender\'s')
kinds.parm('image_exr_compression').set('dwaa')
kinds.parm('image_dwa_compression').set(45)
kinds.parm('image_artist').set('nova')
kinds.parm('aspectRatioConformPolicy').set('cropAperture')
kinds.parmTuple('dataWindowNDC').set((-0.1, -0.1, 1.1, 1.1))
kinds.parm('pixelAspectRatio').set(2.0)
kinds.parm('dcm').set(1)
kinds.parm('dcmfilename').set('deep.exr')
kinds.parm('dcmcompression').set(7)
kinds.parm('dcmofsize').set(1)
kinds.parm('deepUseHitDistance').set(1)
kinds.parm('outputcheckpoint').set(1)
kinds.parm('checkpointfile').set('frame.checkpoint')
kinds.parm('savefrequency').set(12.5)
kinds.parm('resume').set(0)
kst = kinds.stage()
kp, kproduct = kst.GetPrimAtPath('/Render/rendersettings'), kst.GetPrimAtPath('/Render/Products/nova')
kdeep, kcheck = kst.GetPrimAtPath('/Render/Products/nova_deep'), kst.GetPrimAtPath('/Render/Products/nova_checkpoint')
check(kproduct.GetAttribute('driver:parameters:husk:OpenEXR:compression').Get() == 'dwaa' and
      kproduct.GetAttribute('driver:parameters:husk:OpenEXR:dwa_compression').Get() == 45 and
      kproduct.GetAttribute('driver:parameters:husk:artist').Get() == 'nova', 'Metadata: husk\'s EXR options on the product')
check(kp.GetAttribute('aspectRatioConformPolicy').Get() == 'cropAperture' and
      all(abs(a - b) < 1e-6 for a, b in zip(kp.GetAttribute('dataWindowNDC').Get(), (-0.1, -0.1, 1.1, 1.1))) and
      kp.GetAttribute('pixelAspectRatio').Get() == 2.0, 'Aspect Ratio: conform policy, data window and pixel aspect')
check(sorted(str(t) for t in kp.GetRelationship('products').GetTargets()) ==
      ['/Render/Products/nova', '/Render/Products/nova_checkpoint', '/Render/Products/nova_deep'],
      'the settings render the picture, the deep and the checkpoint product: %s' % kp.GetRelationship('products').GetTargets())
check(kdeep.IsValid() and kdeep.GetAttribute('productType').Get() == 'karma:deep' and
      kdeep.GetAttribute('productName').Get() == 'deep.exr' and kdeep.GetAttribute('karma:product:dcmcompression').Get() == 7 and
      kdeep.GetAttribute('karma:product:dcmofsize').Get() == 1 and
      abs(kdeep.GetAttribute('karma:product:dcmzbias').Get() - 0.001) < 1e-9 and
      [str(t) for t in kdeep.GetRelationship('orderedVars').GetTargets()] == ['/Render/Products/Vars/beauty'],
      'Deep Output: a karma:deep product with the picture\'s vars')
check(kp.GetAttribute('nova:deepUseHitDistance').Get() is True and kp.GetAttribute('nova:deepExcludeHoldouts').Get() is False,
      'Deep Output: Nova\'s two switches on the settings')
check(kcheck.IsValid() and kcheck.GetAttribute('productType').Get() == 'karma:checkpoint' and
      kcheck.GetAttribute('productName').Get() == 'frame.checkpoint' and
      abs(kcheck.GetAttribute('saveFrequency').Get() - 12.5) < 1e-6 and kcheck.GetAttribute('resume').Get() is False,
      'Checkpointing: a karma:checkpoint product with its save frequency and resume switch')
header_groups = [t.label() for t in kinds.parmTemplateGroup().entries()
                 if t.type() == hou.parmTemplateType.Folder and t.folderType() == hou.folderType.Collapsible]
check('Simplified Shading' in header_groups, 'Simplified Shading has its group above the tabs: %s' % header_groups)

# ---- M6.7: the layout of the Karma Render Settings LOP.
def folders(entries, folder_type=None):
    return [t for t in entries if t.type() == hou.parmTemplateType.Folder
            and (folder_type is None or t.folderType() == folder_type)]


group = nova.parmTemplateGroup()
tabs = folders(group.entries(), hou.folderType.Tabs)
check([t.label() for t in tabs] == ['Rendering', 'Image Output', 'Advanced'], 'top tabs: %s' % [t.label() for t in tabs])
if tabs:
    subtabs = [t.label() for t in folders(tabs[0].parmTemplates())]
    check(subtabs == ['Sampling', 'Limits', 'Camera Effects', 'Lighting Effects', 'Geometry and Shading', 'Outlines'],
          'Rendering sub-tabs: %s' % subtabs)
    aov_tab = folders(tabs[1].parmTemplates())
    groups = [t.label() for t in folders(aov_tab[0].parmTemplates())] if aov_tab else []
    check(groups == ['Component Level Outputs', 'Utility', 'Cryptomatte'], 'AOV groups: %s' % groups)
header = [t.name() for t in group.entries()[:6]]
check(header[:5] == ['renderprims', 'picture', 'camera', 'resolution', 'sepheader1'], 'header as Karma: %s' % header)
check(st.GetPrimAtPath('/Render/rendersettings').GetAttribute('disableMotionBlur').Get() == 0 and
      st.GetPrimAtPath('/Render/rendersettings').GetAttribute('disableDepthOfField').Get() == 0,
      'motion blur and depth of field on by default')

# ---- The Nova LOP: settings and render buttons in one node.
check(hou.nodeType(hou.lopNodeTypeCategory(), 'nova') is not None, 'the Nova LOP exists')
allinone = stage.createNode('nova', 'nova_all')
allinone.setInput(0, cam)
for name in ('execute', 'renderpreview', 'executebackground', 'renderdialog', 'trange', 'f1'):
    check(allinone.parm(name) is not None, 'the Nova LOP has %s' % name)
check(allinone.node('render').parm('renderer').eval() == 'HdNovaRendererPlugin', 'its ROP renders with Nova')
# The Rendering Engine menu (docs/design/M10.md D1), as the Karma LOPs' engine.
allinone.parm('engine').set('xpu')
check(allinone.node('render').parm('renderer').eval() == 'HdNovaXpuRendererPlugin', 'XPU: its ROP renders with Nova XPU')
check(allinone.stage().GetPrimAtPath('/Render/rendersettings').GetAttribute('husk:default_delegate').Get() ==
      'HdNovaXpuRendererPlugin', 'XPU: husk:default_delegate is Nova XPU')
allinone.parm('engine').set('cpu')
merge2 = stage.createNode('merge')
merge2.setInput(0, allinone)
merge2.setInput(1, cube2)
allinone.parm('camera').set('/cameras/camera1')
allinone.parmTuple('resolution').set((48, 32))
allinone.parm('samples').set(2)
allinone.parm('directdiffuse').set(1)
picture2 = os.path.join(OUT, 'nova_lop.exr').replace('\\', '/')
allinone.parm('picture').set(picture2)
rs2 = allinone.stage().GetPrimAtPath('/Render/rendersettings')
check(rs2.IsValid() and rs2.GetAttribute('nova:samples').Get() == 2 and tuple(rs2.GetAttribute('resolution').Get()) == (48, 32),
      'the Nova LOP authors its settings')
check('/Render/Products/Vars/directdiffuse' in
      [str(t) for t in allinone.stage().GetPrimAtPath('/Render/Products/nova').GetRelationship('orderedVars').GetTargets()],
      'the Nova LOP authors its AOVs')
check(allinone.stage().GetPrimAtPath('/Render/Products/nova').GetAttribute('productName').Get() == picture2,
      'the Nova LOP product writes %s' % picture2)
# Render to Disk, as the button does; the ROP renders its input stage. The
# button is a parameter callback that raises nothing when the render fails, so
# only a file this run wrote proves it.
deep2 = os.path.join(OUT, 'nova_lop_deep.exr').replace('\\', '/')
allinone.parm('dcm').set(1)
allinone.parm('dcmfilename').set(deep2)
allinone.parm('image_exr_compression').set('piz')
remove_stale(picture2)
remove_stale(deep2)
allinone.node('render').parm('execute').pressButton()
check(os.path.exists(picture2), 'Render to Disk wrote %s' % picture2)
check(os.path.exists(deep2), 'Render to Disk wrote the deep camera map %s' % deep2)
if os.path.exists(deep2):
    info = subprocess.run([os.path.join(hfs, 'bin', 'iinfo.exe'), '-v', deep2], capture_output=True, text=True).stdout
    check('deep' in info.lower(), 'the deep camera map is a deep image')
if os.path.exists(picture2):
    info = subprocess.run([os.path.join(hfs, 'bin', 'iinfo.exe'), '-v', picture2], capture_output=True, text=True).stdout
    check('compression (type string):= "piz"' in info, 'the picture is PIZ-compressed (EXR Compression)')
if os.path.exists(picture2):
    info = subprocess.run([os.path.join(hfs, 'bin', 'iinfo.exe'), '-v', picture2], capture_output=True, text=True).stdout
    check('48 x 32' in info and 'samples (type int):= 2' in info, 'the Nova LOP rendered 48 x 32 with 2 samples (Nova)')

# ---- Nova (Setup), as Karma (Setup): settings -> render pass -> USD Render ROP.
import novautils
settings3, rpass3, rop3 = novautils.createSetup(stage, cube2)
check(rpass3.inputs()[0] == settings3 and rop3.inputs()[0] == rpass3, 'Nova (Setup) wires settings, pass and ROP')
check(rop3.parm('renderer').eval() == 'HdNovaRendererPlugin', 'the setup ROP renders with Nova')
settings3.parm('engine').set('xpu')
check(rop3.parm('renderer').eval() == 'HdNovaXpuRendererPlugin', 'the setup ROP follows the Rendering Engine')
settings3.parm('engine').set('cpu')
pass_prim = rop3.parm('renderpass').eval()
pass_stage = rop3.stage() if hasattr(rop3, 'stage') else rpass3.stage()
source = pass_stage.GetPrimAtPath(pass_prim).GetRelationship('renderSource').GetTargets() if pass_prim else []
check([str(t) for t in source] == ['/Render/rendersettings'], 'the render pass %s renders %s' % (pass_prim, source))
tool = hou.shelves.tools().get('lop_nova_setup')
check(tool is not None and 'createSetupTool' in tool.script(), 'the Nova (Setup) tool is on the shelf')
for t in ('nova', 'nova_rendersettings'):
    d = hou.nodeType(hou.lopNodeTypeCategory(), t).definition()
    check('Rendering' in d.sections()['Tools.shelf'].contents(), 'the TAB menu lists %s under Rendering' % t)

# ---- Nova's icon, not Karma's: the package's config/Icons/NOVA_logo.svg, by
# name for the shelf tools, embedded in the HDAs (the node and its TAB entry).
try:
    icon_file = hou.findFile('config/Icons/NOVA_logo.svg')
except hou.OperationFailed:
    icon_file = None
check(icon_file is not None, 'the package has Nova\'s icon: %s' % icon_file)
with open(os.path.join(REPO, 'houdini', 'package', 'config', 'Icons', 'NOVA_logo.svg'), encoding='utf-8') as f:
    svg = f.read()
for t in ('nova', 'nova_rendersettings'):
    d = hou.nodeType(hou.lopNodeTypeCategory(), t).definition()
    section = d.sections().get('IconSVG')
    check(d.icon() == 'opdef:/Lop/%s?IconSVG' % t and section is not None and section.contents() == svg,
          'the %s LOP shows Nova\'s icon (%s, the package\'s SVG)' % (t, d.icon()))
nova_tools = {name: tool.icon() for name, tool in hou.shelves.tools().items() if 'Nova' in tool.label()}
check(nova_tools.get('lop_nova_setup') == 'NOVA_logo' and nova_tools.get('vop_novamtlxsubnet') == 'NOVA_logo',
      'the Nova (Setup) and Nova Material Builder tools show Nova\'s icon')
check(len(nova_tools) >= 4 and not any('karma' in icon.lower() for icon in nova_tools.values()),
      'no Nova tool shows a Karma icon: %s' % nova_tools)

print('%d failure(s)' % len(FAILURES))
sys.exit(1 if FAILURES else 0)
