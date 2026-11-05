"""M6 probe 2: the predefined "Extra Render Vars" of the Karma Render Settings LOP.

hython, isolated (tests/houdini/run_hython.ps1). The Name parameter of an
extra render var has a menu of presets; choosing one runs the node's callback,
which fills Source Name / Source Type / Format / Data Type. This probe adds
one extra render var per preset, runs the callback, records the fields and
exports the authored RenderVar prims.

    run_hython.ps1 -Script probe_krs_presets.py -Stage <stage> <out_dir>
"""
import json
import os
import sys

import hou

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

stage = hou.node('/stage')
krs = stage.createNode('karmarendersettings', 'presets')
# Only the extra render vars: the beauty off.
if krs.parm('beauty'):
    krs.parm('beauty').set(0)


def contains(group, child):
    for t in group:
        if t.name() == child:
            return True
        if isinstance(t, hou.FolderParmTemplate) and contains(t.parmTemplates(), child):
            return True
    return False


def find_multiparm(group, child):
    for t in group:
        if isinstance(t, hou.FolderParmTemplate):
            if t.folderType() in (hou.folderType.MultiparmBlock, hou.folderType.ScrollingMultiparmBlock,
                                  hou.folderType.TabbedMultiparmBlock) and contains(t.parmTemplates(), child):
                return t.name()
            r = find_multiparm(t.parmTemplates(), child)
            if r:
                return r
    return None


multi = find_multiparm(krs.parmTemplateGroup().parmTemplates(), 'name#')
print('extra render var multiparm:', multi)
krs.parm(multi).set(1)
menu = krs.parm('name1').menuItems()
labels = krs.parm('name1').menuLabels()
print('presets:', len(menu))

krs.parm(multi).set(len(menu))
fields = ('name', 'format', 'dataType', 'sourceName', 'sourceType', 'filter', 'cryptomatte', 'split',
          'utilitypathexpression')
presets = []
for i, (token, label) in enumerate(zip(menu, labels), start=1):
    p = krs.parm('name%d' % i)
    p.set(token)
    try:
        p.pressButton()  # runs the callback (setAOV)
        ok = True
    except Exception as e:  # noqa: BLE001
        ok = 'callback failed: %s' % e
    row = {'menuToken': token, 'menuLabel': label, 'callback': ok}
    for f in fields:
        q = krs.parm('%s%d' % (f, i))
        if q is not None:
            row[f] = q.eval()
    presets.append(row)

with open(os.path.join(OUT, 'krs_extra_presets.json'), 'w', encoding='utf-8') as f:
    json.dump(presets, f, indent=1)
with open(os.path.join(OUT, 'krs_extra_presets.txt'), 'w', encoding='utf-8') as f:
    for r in presets:
        f.write('%-28s %-34s %-10s %-10s %-26s %-9s %s\n' % (
            r['menuToken'][:28], r['menuLabel'][:34], r.get('format', ''), r.get('dataType', ''),
            r.get('sourceName', ''), r.get('sourceType', ''), r.get('filter', '')))

krs.cook(force=True)
with open(os.path.join(OUT, 'krs_extra_presets.usda'), 'w', encoding='utf-8') as f:
    f.write(krs.stage().GetRootLayer().ExportToString())
print('done')
