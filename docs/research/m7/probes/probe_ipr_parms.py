"""M7 probe 3: Karma's IPR/denoise-related parameters on the generic Render
Settings LOP and the Karma Render Properties LOP, with defaults, and the
attribute each one authors.

hython, isolated (tests/houdini/run_hython.ps1):

    run_hython.ps1 -Script probe_ipr_parms.py -Stage <stage> <out_dir>

Writes <out_dir>/ipr_parms.txt.
"""
import os
import re
import sys

import hou

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
pat = re.compile(r'denois|imagefilter|ipr|oracle|filter', re.I)
stage = hou.node('/stage')

with open(os.path.join(OUT, 'ipr_parms.txt'), 'w', encoding='utf-8') as f:
    for tname in ('rendersettings', 'karmarenderproperties', 'karmarendersettings'):
        try:
            n = stage.createNode(tname)
        except Exception as e:
            f.write('%s: cannot create (%s)\n' % (tname, e))
            continue
        f.write('=== %s\n' % tname)
        for p in n.parms():
            t = p.parmTemplate()
            if not (pat.search(p.name()) or pat.search(t.label())):
                continue
            try:
                v = p.eval()
            except Exception:
                v = '?'
            menu = ''
            if hasattr(t, 'menuItems'):
                try:
                    items = t.menuItems()
                    if items:
                        menu = ' menu=%s' % list(items)[:12]
                except Exception:
                    pass
            f.write('  %-48s %-40r label=%r%s\n' % (p.name(), v, t.label(), menu))
        n.destroy()
print('done')
