"""M5 spike, part 1: the VFL a VEX Material Builder writes to USD
(principled shader core + a noise pattern + a parameter)."""
import os
import sys

import hou

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

lib = hou.node('/stage').createNode('materiallibrary', 'matlib')
mb = lib.createNode('materialbuilder', 'vexbuilder')
out = mb.node('surface_output')
core = mb.createNode('principledshadercore', 'core')
print('core inputs:', core.inputNames())
print('core outputs:', core.outputNames())
tint = mb.createNode('parameter', 'tint')
tint.parm('parmname').set('tint')
tint.parm('parmtype').set('color')
tint.parmTuple('colordef').set((0.8, 0.3, 0.2)) if tint.parmTuple('colordef') else None
noise = mb.createNode('aanoise', 'noise')
glob = mb.node('surface_globals')
noise.setNamedInput('pos', glob, 'P')
mul = mb.createNode('multiply', 'mul')
mul.setInput(0, tint, 0)
mul.setInput(1, noise, 0)
core.setNamedInput('basecolor', mul, 0)
core.parm('rough').set(0.4) if core.parm('rough') else None
for name in ('F', 'bsdf'):
    if name in core.outputNames():
        out.setNamedInput('F', core, name)
        print('wired core.%s -> surface_output.F' % name)
        break
for name in ('Of', 'opacity'):
    if name in core.outputNames():
        out.setNamedInput('Of', core, name)
lib.cook(force=True)
stage = lib.stage()
for prim in stage.Traverse():
    a = prim.GetAttribute('info:sourceCode')
    if a and a.Get():
        code = a.Get()
        path = os.path.join(OUT, prim.GetName() + '.vfl')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(code)
        print('%s: %d chars -> %s' % (prim.GetPath(), len(code), path))
        for b in prim.GetAuthoredAttributes():
            if b.GetName().startswith('inputs:'):
                print('   input', b.GetTypeName(), b.GetName(), b.Get())
stage.Export(os.path.join(OUT, 'builder.usda'))
