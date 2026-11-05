"""M6 probe 1: which parameters the Karma Render Settings LOP has for AOVs.

hython, isolated (tests/houdini/run_hython.ps1). Writes a listing of every
parameter in the AOV-related folders of karmarendersettings (name, label,
type, default, menu, tooltip) and the node's internal network, so that probe 2
can switch every render var on.

    run_hython.ps1 -Script probe_krs_parms.py -Stage <stage> <out_dir>
"""
import json
import os
import sys

import hou

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

stage = hou.node('/stage')
krs = stage.createNode('karmarendersettings', 'krs')
print('type:', krs.type().name(), krs.type().definition().libraryFilePath() if krs.type().definition() else '-')

rows = []


def walk(templates, folders):
    for t in templates:
        if isinstance(t, hou.FolderParmTemplate):
            walk(t.parmTemplates(), folders + [t.label()])
            continue
        if isinstance(t, (hou.SeparatorParmTemplate, hou.LabelParmTemplate)):
            continue
        row = {
            'folder': ' > '.join(folders),
            'name': t.name(),
            'label': t.label(),
            'type': t.type().name(),
            'help': t.help(),
        }
        try:
            row['default'] = list(t.defaultValue()) if hasattr(t, 'defaultValue') else None
        except Exception:
            row['default'] = None
        if hasattr(t, 'menuItems'):
            try:
                row['menu'] = list(t.menuItems())
            except Exception:
                pass
        tags = t.tags()
        if tags:
            row['tags'] = dict(tags)
        rows.append(row)


walk(krs.parmTemplateGroup().parmTemplates(), [])
with open(os.path.join(OUT, 'krs_parms_all.json'), 'w', encoding='utf-8') as f:
    json.dump(rows, f, indent=1)

aov_rows = [r for r in rows if 'AOV' in r['folder'] or 'Render Var' in r['folder'] or 'Image Output' in r['folder']]
with open(os.path.join(OUT, 'krs_parms_aov.txt'), 'w', encoding='utf-8') as f:
    for r in aov_rows:
        f.write('%-60s | %-34s | %-28s | %-8s | %s | %s\n' % (r['folder'][-60:], r['name'], r['label'][:28], r['type'],
                                                            r.get('default'), (r.get('help') or '').replace('\n', ' ')[:200]))
print('parms total', len(rows), 'aov-ish', len(aov_rows))

# The internal network of the HDA (which LOPs author what).
with open(os.path.join(OUT, 'krs_children.txt'), 'w', encoding='utf-8') as f:
    def dump(node, depth):
        for c in node.children():
            f.write('%s%s (%s)\n' % ('  ' * depth, c.name(), c.type().name()))
            if c.children() and depth < 2:
                dump(c, depth + 1)
    dump(krs, 0)

# Default stage as authored with defaults (beauty only).
krs.cook(force=True)
layer = krs.stage().GetRootLayer()
with open(os.path.join(OUT, 'krs_default.usda'), 'w', encoding='utf-8') as f:
    f.write(krs.stage().ExportToString() if False else layer.ExportToString())
print('done')
