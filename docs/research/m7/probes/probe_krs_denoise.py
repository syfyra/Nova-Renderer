"""M7 probe 1: what the Karma Render Settings LOP authors for its denoiser and
COP image filter options, and what the Image Filter LOP authors.

hython, isolated (tests/houdini/run_hython.ps1):

    run_hython.ps1 -Script probe_krs_denoise.py -Stage <stage> <out_dir>

Writes into <out_dir>:
  krs_filter_parms.txt   every karmarendersettings parameter in a folder whose
                         label mentions Filter/Denois, plus every parameter whose
                         name mentions denoise/imagefilter/tonemap/cop
  krs_denoise_dump.txt   authored attributes of /Render prims for several
                         denoiser configurations (diffed against the default)
  imagefilter_parms.txt  parameters of the Image Filter LOP
  imagefilter_dump.txt   prims the Image Filter LOP authors
"""
import json
import os
import sys

import hou

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

stage_net = hou.node('/stage')


def walk(templates, folders, rows):
    for t in templates:
        if isinstance(t, hou.FolderParmTemplate):
            walk(t.parmTemplates(), folders + [t.label()], rows)
            continue
        if isinstance(t, (hou.SeparatorParmTemplate, hou.LabelParmTemplate)):
            continue
        row = {
            'folder': ' > '.join(folders),
            'name': t.name(),
            'label': t.label(),
            'type': t.type().name(),
        }
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


def dump_parms(node, path, keep):
    rows = []
    walk(node.parmTemplateGroup().parmTemplates(), [], rows)
    with open(path, 'w', encoding='utf-8') as f:
        f.write('node type: %s\n' % node.type().name())
        for r in rows:
            if keep(r):
                f.write(json.dumps(r) + '\n')
    return rows


def prim_attrs(stage, root='/Render'):
    """Authored attributes/relationships of all prims below root."""
    out = {}
    rp = stage.GetPrimAtPath(root)
    if not rp:
        return out
    from pxr import Usd
    for prim in Usd.PrimRange(rp):
        d = {'@type': str(prim.GetTypeName()),
             '@api': [str(s) for s in prim.GetAppliedSchemas()]}
        for a in prim.GetAuthoredAttributes():
            try:
                v = a.Get()
            except Exception as e:
                v = '<%s>' % e
            d[a.GetName()] = repr(v)
        for r in prim.GetAuthoredRelationships():
            d['rel ' + r.GetName()] = [str(t) for t in r.GetTargets()]
        out[str(prim.GetPath())] = d
    return out


def diff(a, b):
    lines = []
    for path in sorted(set(a) | set(b)):
        pa, pb = a.get(path), b.get(path)
        if pa is None:
            lines.append('+ prim %s %s' % (path, json.dumps(pb, indent=None)))
            continue
        if pb is None:
            lines.append('- prim %s' % path)
            continue
        for k in sorted(set(pa) | set(pb)):
            if pa.get(k) != pb.get(k):
                lines.append('  %s.%s: %s -> %s' % (path, k, pa.get(k), pb.get(k)))
    return lines


krs = stage_net.createNode('karmarendersettings', 'krs')


def keep_krs(r):
    f = r['folder'].lower()
    n = r['name'].lower()
    return ('filter' in f or 'denois' in f or 'denois' in n or 'imagefilter' in n
            or 'tonemap' in n or n.startswith('cop') or 'ipr' in n or 'oracle' in n)


rows = dump_parms(krs, os.path.join(OUT, 'krs_filter_parms.txt'), keep_krs)
names = {r['name'] for r in rows}

base = prim_attrs(krs.stage())
with open(os.path.join(OUT, 'krs_default_render.txt'), 'w', encoding='utf-8') as f:
    for path, d in base.items():
        f.write(path + '\n')
        for k, v in d.items():
            f.write('    %s = %s\n' % (k, v))

configs = []
denoise_parm = None
for cand in ('denoise', 'denoiser'):
    if cand in names:
        denoise_parm = cand
        break
if denoise_parm:
    menu = krs.parm(denoise_parm).menuItems()
    for item in menu:
        configs.append(('%s=%s' % (denoise_parm, item), {denoise_parm: item}))
    for item in menu:
        if 'optix' in item.lower():
            extra = {denoise_parm: item}
            for p in ('usealbedo', 'useninput', 'denoise_separate_aovs', 'denoise_cpu_only'):
                if p in names:
                    extra[p] = 1
            if 'denoise_aovs' in names:
                extra['denoise_aovs'] = 'C diffuse'
            configs.append(('optix+all toggles', extra))
for p in ('cop_createaovs',):
    if p in names:
        configs.append(('%s=1' % p, {p: 1}))

with open(os.path.join(OUT, 'krs_denoise_dump.txt'), 'w', encoding='utf-8') as f:
    f.write('denoise parm: %s\n' % denoise_parm)
    for label, values in configs:
        n = stage_net.createNode('karmarendersettings', 'krs_' + str(len(label)))
        b0 = prim_attrs(n.stage())
        ok = []
        for k, v in values.items():
            try:
                parm = n.parm(k)
                if isinstance(v, str):
                    parm.set(v)
                else:
                    parm.set(v)
                ok.append(k)
            except Exception as e:
                ok.append('%s FAILED %s' % (k, e))
        b1 = prim_attrs(n.stage())
        f.write('\n=== %s  (%s)\n' % (label, ', '.join(ok)))
        for line in diff(b0, b1):
            f.write(line + '\n')
        n.destroy()

# Image Filter LOP
try:
    imf = stage_net.createNode('imagefilter', 'imf')
    dump_parms(imf, os.path.join(OUT, 'imagefilter_parms.txt'), lambda r: True)
    with open(os.path.join(OUT, 'imagefilter_dump.txt'), 'w', encoding='utf-8') as f:
        for path, d in prim_attrs(imf.stage(), '/').items():
            if 'ImageFilter' in d.get('@type', '') or 'Filter' in path:
                f.write(path + '\n')
                for k, v in d.items():
                    f.write('    %s = %s\n' % (k, v))
        f.write('\nchildren of the LOP: %s\n' % [c.path() + ':' + c.type().name() for c in imf.allSubChildren()])
except Exception as e:
    with open(os.path.join(OUT, 'imagefilter_dump.txt'), 'w', encoding='utf-8') as f:
        f.write('imagefilter failed: %s\n' % e)

print('done')
