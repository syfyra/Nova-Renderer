"""M7 adaptive sampling probe 1: the sampling parameters of the Karma Render
Settings LOP and the RenderSettings attributes they author.

hython, isolated (tests/houdini/run_hython.ps1). Writes to <out_dir>:
  krs_sampling_parms.txt  every parameter whose name or folder concerns
                          sampling (name, label, type, default, menu);
  krs_sampling_attrs.txt  per variant, the authored karma:* sampling
                          attributes of the RenderSettings prim and the
                          primarysamples render var when it is switched on.

    run_hython.ps1 -Script as_probe_krs_sampling.py <out_dir>
"""
import json
import os
import re
import sys

import hou

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

stage = hou.node('/stage')
krs = stage.createNode('karmarendersettings', 'krs')

PAT = re.compile(r'oracle|sampl|variance|convergence|bucket|progressive|imagemode|seed|engine|percent|screendoor|'
                 r'noise|quality|guiding|pixelfilter|colorlimit', re.I)

rows = []


def walk(templates, folders):
    for t in templates:
        if isinstance(t, hou.FolderParmTemplate):
            walk(t.parmTemplates(), folders + [t.label()])
            continue
        if isinstance(t, (hou.SeparatorParmTemplate, hou.LabelParmTemplate)):
            continue
        folder = ' > '.join(folders)
        if not (PAT.search(t.name()) or PAT.search(t.label()) or 'Sampling' in folder):
            continue
        row = {'folder': folder, 'name': t.name(), 'label': t.label(), 'type': t.type().name()}
        try:
            row['default'] = list(t.defaultValue()) if hasattr(t, 'defaultValue') else None
        except Exception:
            row['default'] = None
        if hasattr(t, 'menuItems'):
            try:
                items = list(t.menuItems())
                labels = list(t.menuLabels())
                if items:
                    row['menu'] = list(zip(items, labels))
            except Exception:
                pass
        rows.append(row)


walk(krs.parmTemplateGroup().parmTemplates(), [])
with open(os.path.join(OUT, 'krs_sampling_parms.txt'), 'w', encoding='utf-8') as f:
    for r in rows:
        f.write('%-55s | %-34s | %-30s | %-6s | default %s%s\n' % (
            r['folder'][-55:], r['name'], r['label'][:30], r['type'][:6], r.get('default'),
            ('  menu ' + str(r['menu'])) if r.get('menu') else ''))

ATTR = re.compile(r'karma:(global|object):(.*(sampl|oracle|variance|convergence|bucket|progressive|imagemode|seed|'
                  r'engine|percent|screendoor|quality|pixelfilter|colorlimit).*)', re.I)


def settings_prim(st):
    for p in st.Traverse():
        if p.GetTypeName() == 'RenderSettings':
            return p
    return None


def dump(tag, f):
    krs.cook(force=True)
    st = krs.stage()
    p = settings_prim(st)
    f.write('== %s: %s\n' % (tag, p.GetPath() if p else None))
    if not p:
        return
    for a in sorted(p.GetAttributes(), key=lambda a: a.GetName()):
        if ATTR.match(a.GetName()) or a.GetName() in ('resolution',):
            f.write('   %-48s %-8s authored=%d  %r\n' % (a.GetName(), a.GetTypeName(), a.HasAuthoredValue(), a.Get()))
    for v in st.Traverse():
        if v.GetTypeName() == 'RenderVar' and 'sampl' in v.GetName().lower():
            f.write('   var %s\n' % v.GetPath())
            for a in sorted(v.GetAttributes(), key=lambda a: a.GetName()):
                if a.HasAuthoredValue():
                    f.write('      %-52s %r\n' % (a.GetName(), a.Get()))


def setp(name, value):
    p = krs.parm(name)
    if p is None:
        return False
    p.set(value)
    return True


with open(os.path.join(OUT, 'krs_sampling_attrs.txt'), 'w', encoding='utf-8') as f:
    dump('defaults', f)
    # Which oracle-related parameters exist (names found above).
    names = [r['name'] for r in rows]
    f.write('oracle parms: %s\n' % [n for n in names if 'oracle' in n])
    if setp('oracle', 'uniform'):
        dump('oracle=uniform', f)
        setp('oracle', 'variance')
    for n in ('convergence_mode', 'xn__karmaglobalconvergence_mode_0kbg'):
        if krs.parm(n) is not None:
            items = krs.parm(n).parmTemplate().menuItems()
            for it in items:
                krs.parm(n).set(it)
                dump('%s=%s' % (n, it), f)
            krs.parm(n).revertToDefaults()
            break
    for n in ('engine', 'xn__karmaglobalengine_q3ag'):
        if krs.parm(n) is not None:
            krs.parm(n).set('xpu')
            dump('%s=xpu' % n, f)
            krs.parm(n).revertToDefaults()
            break
    # Non-default oracle values, to see the JSON they produce.
    setp('oracle_minsamples', 4)
    setp('oracle_variance', 0.002)
    setp('oracle_plane', 'C foo')
    dump('oracle minsamples=4 variance=0.002 plane="C foo"', f)
    # The primarysamples render var (Extra Render Vars preset or a toggle).
    found = [n for n in names if 'primarysample' in n.lower()]
    f.write('primarysamples parms: %s\n' % found)
    for n in found:
        try:
            krs.parm(n).set(1)
        except Exception:
            pass
    dump('primarysamples on', f)

with open(os.path.join(OUT, 'krs_sampling_parms.json'), 'w', encoding='utf-8') as f:
    json.dump(rows, f, indent=1)
print('done', len(rows))
