"""M5 probe: how Solaris writes VEX materials to USD (hython, isolated)."""
import os
import sys

import hou

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)


def show(lop, title):
    print('\n==== %s' % title)
    lop.cook(force=True)
    for prim in lop.stage().Traverse():
        if prim.GetTypeName() not in ('Material', 'Shader', 'NodeGraph'):
            continue
        cd = prim.GetCustomData()
        print('\n%s (%s)%s' % (prim.GetPath(), prim.GetTypeName(), (' customData=%s' % cd) if cd else ''))
        for a in prim.GetAuthoredAttributes():
            v = a.Get()
            s = repr(v)
            if len(s) > 240:
                s = s[:240] + '...(%d chars)' % len(s)
            c = a.GetConnections()
            md = a.GetAllMetadata().get('customData')
            print('   %s %s = %s%s%s' % (a.GetTypeName(), a.GetName(), s, (' <- %s' % [str(x) for x in c]) if c else '',
                                       '  [customData]' if md else ''))


stage = hou.node('/stage')
lib = stage.createNode('materiallibrary', 'matlib')
lib.parm('genpreviewshaders').set(1) if lib.parm('genpreviewshaders') else None

# 1. Principled shader with a value, and with displacement enabled.
ps = lib.createNode('principledshader::2.0', 'principled')
ps.parmTuple('basecolor').set((0.8, 0.1, 0.1))
ps.parm('rough').set(0.35)
for name in ('dispInput_enable', 'dispTex_enable', 'dispNoise_enable'):
    if ps.parm(name):
        print('principled has', name)
if ps.parm('dispTex_enable'):
    ps.parm('dispTex_enable').set(1)

# 2. A VEX Material Builder.
mb = lib.createNode('materialbuilder', 'vexbuilder')
print('builder children:', [(c.name(), c.type().name()) for c in mb.children()])
for c in mb.children():
    print('  ', c.name(), 'inputs', c.inputNames()[:12], 'outputs', c.outputNames()[:8])
print('builder spare parms:', [p.name() for p in mb.spareParms()])

show(lib, 'principled + default builder')

# 3. Builder: a parameter VOP and a bind (primvar) into a principled shader core inside.
coll = None
for c in mb.children():
    if c.type().name() in ('collect', 'output', 'suboutput'):
        coll = c
core = None
try:
    core = mb.createNode('principledshadercore', 'core')
except hou.Error as e:
    print('core:', e)
try:
    p = mb.createNode('parameter', 'tint')
    p.parm('parmname').set('tint')
    p.parm('parmtype').set('color')
    if core is not None:
        core.setNamedInput('basecolor', p, 0)
except hou.Error as e:
    print('parameter:', e)
try:
    snip = mb.createNode('snippet', 'snip')
    snip.parm('code').set('rough = 0.3 + 0.2 * sin(P.x * 10);')
    print('snippet outputs', snip.outputNames())
except hou.Error as e:
    print('snippet:', e)
print('builder children now:', [(c.name(), c.type().name()) for c in mb.children()])
for c in mb.children():
    if c.type().name() in ('collect', 'output', 'surfaceoutput'):
        print('  output node', c.name(), c.inputNames())
if core is not None:
    # Wire the core into the builder's output/collect.
    for c in mb.children():
        names = c.inputNames()
        if 'surface' in names:
            c.setNamedInput('surface', core, 'surface') if 'surface' in core.outputNames() else None
            print('wired core -> %s.surface' % c.name(), core.outputNames()[:6])
show(lib, 'builder with core, parameter, snippet')

# 4. The builder as an embedded HDA.
try:
    hda = mb.createDigitalAsset(name='probe_vexmat', hda_file_name='Embedded', description='Probe VEX material')
    show(lib, 'builder as embedded HDA')
except hou.Error as e:
    print('embedded HDA:', e)

usd = os.path.join(OUT, 'vex_materials.usda')
lib.stage().Export(usd)
print('\nexported', usd)
