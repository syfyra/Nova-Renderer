"""M6 probe 4: "Split Render Product by Render Var" on the Karma Render
Settings LOP (one product per render var) - what it authors.

    run_hython.ps1 -Script probe_krs_split.py -Stage <stage> <out_dir>
"""
import os
import sys

import hou

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

krs = hou.node('/stage').createNode('karmarendersettings', 'split')
krs.parm('picture').set('$HIP/render/split.exr')
for name in ('directdiffuse', 'hitPz', 'hitN'):
    krs.parm(name).set(1)
krs.parm('splitrendervars').set(1)
krs.cook(force=True)
stage = krs.stage()
with open(os.path.join(OUT, 'krs_split_dump.txt'), 'w', encoding='utf-8') as f:
    for prim in stage.Traverse():
        if prim.GetTypeName() not in ('RenderSettings', 'RenderProduct'):
            continue
        f.write('\n%s (%s)\n' % (prim.GetPath(), prim.GetTypeName()))
        for a in prim.GetAuthoredAttributes():
            if a.GetName().startswith('karma:'):
                continue
            f.write('    %s %s = %r\n' % (a.GetTypeName(), a.GetName(), a.Get()))
        for r in prim.GetAuthoredRelationships():
            f.write('    rel %s = %s\n' % (r.GetName(), [str(t) for t in r.GetTargets()]))
print(open(os.path.join(OUT, 'krs_split_dump.txt'), encoding='utf-8').read())
