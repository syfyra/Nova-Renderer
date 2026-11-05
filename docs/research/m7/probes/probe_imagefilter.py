"""M7 probe 2: what the Image Filter LOP authors for a Denoise AI (OptiX)
filter, and the scene layer that husk needs to run it.

hython, isolated (tests/houdini/run_hython.ps1):

    run_hython.ps1 -Script probe_imagefilter.py -Stage <stage> <out_dir>

Writes into <out_dir>:
  filterlist_info.txt     the COP filter list network, the denoiseai COP's
                          parameters and the HoudiniImageFilter prims
  imagefilter_layer.usda  the /Render/ImageFilter prims (the Image Filter
                          LOP's output, flattened), for use with husk
"""
import json
import os
import sys

import hou
from pxr import Usd

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
log = open(os.path.join(OUT, 'filterlist_info.txt'), 'w', encoding='utf-8')


def p(*a):
    print(*a, file=log)
    log.flush()


stage_net = hou.node('/stage')
imf = stage_net.createNode('imagefilter', 'denoise_optix')
fl = imf.node('filterlist1')
p('filter list node:', fl.path(), fl.type().name(), fl.type().category().name())
p('filter list parms:', [(x.name(), x.eval()) for x in fl.parms()][:60])
p('filter list node types (COP, containing denoise):',
  sorted(n for n in hou.copNodeTypeCategory().nodeTypes() if 'denois' in n))
for c in fl.children():
    p('existing child', c.path(), c.type().name())

dn = None
for tname in ('denoiseai', 'Cop/denoiseai'):
    try:
        dn = fl.createNode(tname, 'denoiseai1')
        break
    except Exception as e:
        p('create', tname, 'failed', e)
if dn is not None:
    p('denoiseai type:', dn.type().name(), 'inputs:', dn.inputNames() if hasattr(dn, 'inputNames') else '?')
    for parm in dn.parms():
        t = parm.parmTemplate()
        menu = None
        if hasattr(t, 'menuItems'):
            try:
                menu = list(t.menuItems())
            except Exception:
                menu = None
        p('  parm %-24s = %-12r %s' % (parm.name(), parm.eval(), menu if menu else ''))
    try:
        dn.parm('denoiser').set('optix')
    except Exception as e:
        p('set denoiser failed', e)
    # wire it to the list's output if the list uses wiring
    outs = [c for c in fl.children() if c.type().name() in ('output', 'null', 'block_end')]
    p('outputs in list:', [o.path() + ':' + o.type().name() for o in outs])
    fl.layoutChildren()

imf.cook(force=True)
stage = imf.stage()
for prim in Usd.PrimRange(stage.GetPrimAtPath('/Render')):
    p(str(prim.GetPath()), prim.GetTypeName(), [str(s) for s in prim.GetAppliedSchemas()])
    for a in prim.GetAuthoredAttributes():
        try:
            v = a.Get()
        except Exception as e:
            v = '<%s>' % e
        p('    %s %s = %r' % (a.GetTypeName(), a.GetName(), v))
    for r in prim.GetAuthoredRelationships():
        p('    rel %s = %s' % (r.GetName(), [str(t) for t in r.GetTargets()]))

# Write the /Render/ImageFilter part as a layer usable by husk.
layer = imf.stage().Flatten()
layer.Export(os.path.join(OUT, 'imagefilter_flat.usda'))

# Karma render settings that reference the filter list (default parms)
krs = stage_net.createNode('karmarendersettings', 'krs')
krs.setInput(0, imf)
try:
    krs.parm('cop_filter_1').set('/Render/ImageFilter/denoise_optix')
except Exception as e:
    p('cop_filter_1 failed', e)
for name in ('cop_createaovs',):
    try:
        p('krs %s = %r' % (name, krs.parm(name).eval()))
    except Exception as e:
        p(name, e)
krs.cook(force=True)
st = krs.stage()
rs = st.GetPrimAtPath('/Render/krs')
for a in rs.GetAuthoredAttributes():
    if 'filter' in a.GetName().lower() or 'husk' in a.GetName().lower():
        p('krs attr %s = %r' % (a.GetName(), a.Get()))
for r in rs.GetAuthoredRelationships():
    p('krs rel %s = %s' % (r.GetName(), [str(t) for t in r.GetTargets()]))
for prim in Usd.PrimRange(st.GetPrimAtPath('/Render/krs')):
    if prim.GetTypeName() == 'RenderVar':
        p('var', prim.GetPath(), prim.GetAttribute('sourceName').Get(),
          prim.GetAttribute('driver:parameters:aov:husk:name').Get())
for prim in Usd.PrimRange(st.GetPrimAtPath('/Render')):
    if prim.GetTypeName() == 'RenderVar' and not str(prim.GetPath()).startswith('/Render/krs'):
        p('other var', prim.GetPath(), [(a.GetName(), a.Get()) for a in prim.GetAuthoredAttributes()])
    if prim.GetTypeName() == 'RenderProduct':
        p('product', prim.GetPath(), [str(t) for t in prim.GetRelationship('orderedVars').GetTargets()])
st.Flatten().Export(os.path.join(OUT, 'krs_with_filter_flat.usda'))
log.close()
print('done')
