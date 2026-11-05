# M8 research probe (houdini-volumes.md): loads hv_scene.hip (hv_build_scene.py)
# in an isolated hython and dumps what the LOPs author for volumes:
#   - every Volume / *FieldAsset prim: type, field relationships, authored
#     attributes with values, customData, and the layers that author it;
#   - the Karma Pyro Material network (shader ids, inputs, connections);
#   - the Render Geometry Settings attributes of the pyro stage;
#   - with "save", a USD ROP writes the merged stage to <outdir>/usd and the
#     script lists the written files and the saved field paths.
#
#   powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m8\probes\hv_dump_stage.py <scene dir> [save]
import json
import os
import sys

import hou
from pxr import Sdf, Usd, UsdShade, UsdVol

scene = os.path.abspath(sys.argv[1])
do_save = 'save' in sys.argv[2:]
hou.hipFile.load(os.path.join(scene, 'hv_scene.hip').replace('\\', '/'), suppress_save_prompt=True, ignore_load_warnings=True)
hou.setFrame(16)

FIELD_TYPES = ('OpenVDBAsset', 'HoudiniFieldAsset', 'Field3DAsset')


def val(v):
    if isinstance(v, Sdf.AssetPath):
        return {'asset': v.path, 'resolved': v.resolvedPath}
    if hasattr(v, '__len__') and not isinstance(v, str) and len(v) > 8:
        return '<%s len %d>' % (type(v).__name__, len(v))
    try:
        json.dumps(v)
        return v
    except TypeError:
        return str(v)


def dump_prim(prim, time):
    d = {'path': str(prim.GetPath()), 'type': str(prim.GetTypeName()),
         'apiSchemas': [str(s) for s in prim.GetAppliedSchemas()],
         'kind': str(Usd.ModelAPI(prim).GetKind() or ''),
         'customData': {k: val(v) for k, v in prim.GetCustomData().items()},
         'layers': sorted({spec.layer.identifier for spec in prim.GetPrimStack()})}
    attrs = {}
    for a in prim.GetAttributes():
        if not a.HasAuthoredValue() and a.GetName() not in ('filePath', 'fieldName', 'fieldIndex', 'fieldDataType', 'fieldClass', 'vectorDataRoleHint'):
            continue
        v = a.Get(time)
        e = {'typeName': str(a.GetTypeName()), 'value': val(v)}
        if a.GetNumTimeSamples():
            e['timeSamples'] = a.GetNumTimeSamples()
        if not a.HasAuthoredValue():
            e['fallback'] = True
        cd = a.GetCustomData()
        if cd:
            e['customData'] = {k: val(x) for k, x in cd.items()}
        attrs[a.GetName()] = e
    d['attributes'] = attrs
    rels = {}
    for r in prim.GetRelationships():
        t = r.GetTargets()
        if t:
            rels[r.GetName()] = [str(x) for x in t]
    d['relationships'] = rels
    return d


def dump_stage(lop, label):
    stage = lop.stage()
    t = Usd.TimeCode(hou.frame())
    out = {'node': lop.path(), 'errors': lop.errors(), 'warnings': lop.warnings(), 'prims': []}
    for prim in stage.Traverse():
        tn = str(prim.GetTypeName())
        if tn == 'Volume' or tn in FIELD_TYPES:
            out['prims'].append(dump_prim(prim, t))
    # Layer stack overview (sop layers have identifiers derived from op: paths)
    out['usedLayers'] = sorted({l.identifier for l in stage.GetUsedLayers()})
    # HoudiniLayerInfo on the root/sublayers carries volume file path lists
    infos = []
    for l in stage.GetUsedLayers():
        p = l.GetPrimAtPath('/HoudiniLayerInfo')
        if p:
            infos.append({'layer': l.identifier, 'customData': {k: val(v) for k, v in p.customData.items()}})
    out['HoudiniLayerInfo'] = infos
    return out


result = {}
result['merge_all'] = dump_stage(hou.node('/stage/merge_all'), 'merge_all')

# Pyro stage: volume prims + material network + geometry settings
pyro_out = hou.node('/stage/pyro_stage/OUT')
ps = pyro_out.stage()
pyro = dump_stage(pyro_out, 'pyro_stage')
mats = []
for prim in ps.Traverse():
    if prim.IsA(UsdShade.Material):
        m = {'path': str(prim.GetPath()), 'outputs': {}, 'shaders': []}
        mat = UsdShade.Material(prim)
        for o in mat.GetOutputs():
            m['outputs'][o.GetFullName()] = [str(s.source.GetPath()) + '.' + s.sourceName for s in o.GetConnectedSources()[0]]
        for sp in Usd.PrimRange(prim):
            if sp.IsA(UsdShade.Shader):
                sh = UsdShade.Shader(sp)
                e = {'path': str(sp.GetPath()), 'id': str(sh.GetShaderId() or ''),
                     'implementationSource': str(sh.GetImplementationSource()), 'inputs': {}, 'outputs': [o.GetBaseName() for o in sh.GetOutputs()]}
                for i in sh.GetInputs():
                    srcs = i.GetConnectedSources()[0]
                    if srcs:
                        e['inputs'][i.GetBaseName()] = {'connect': [str(s.source.GetPath()) + '.' + s.sourceName for s in srcs]}
                    else:
                        e['inputs'][i.GetBaseName()] = {'value': val(i.Get()), 'type': str(i.GetTypeName())}
                m['shaders'].append(e)
            elif sp.IsA(UsdShade.NodeGraph) and sp != prim:
                ng = UsdShade.NodeGraph(sp)
                ins = {}
                for i in ng.GetInputs():
                    srcs = i.GetConnectedSources()[0]
                    if srcs:
                        ins[i.GetBaseName()] = {'connect': [str(s.source.GetPath()) + '.' + s.sourceName for s in srcs]}
                    else:
                        ins[i.GetBaseName()] = {'value': val(i.Get()), 'type': str(i.GetTypeName())}
                m['shaders'].append({'path': str(sp.GetPath()), 'nodegraph': True, 'inputs': ins,
                                     'outputs': {o.GetBaseName(): [str(s.source.GetPath()) + '.' + s.sourceName for s in o.GetConnectedSources()[0]] for o in ng.GetOutputs()}})
        m['materialInputs'] = {i.GetBaseName(): val(i.Get()) for i in mat.GetInputs()}
        mats.append(m)
pyro['materials'] = mats
binds = {}
geo_settings = {}
for prim in ps.Traverse():
    b = UsdShade.MaterialBindingAPI(prim).GetDirectBinding()
    if b.GetMaterialPath():
        binds[str(prim.GetPath())] = str(b.GetMaterialPath())
    attrs = {a.GetName(): val(a.Get()) for a in prim.GetAttributes() if 'karma' in a.GetName() and a.HasAuthoredValue()}
    if attrs:
        geo_settings[str(prim.GetPath())] = attrs
pyro['bindings'] = binds
pyro['karmaAttributes'] = geo_settings
result['pyro_stage'] = pyro

if do_save:
    usddir = os.path.join(scene, 'usd').replace('\\', '/')
    os.makedirs(usddir, exist_ok=True)
    rops = {}
    for name, lop in (('merge_all', '/stage/merge_all'), ('pyro_stage', '/stage/pyro_stage/OUT')):
        rop = hou.node('/stage').createNode('usd_rop', 'save_' + name)
        rop.parm('loppath').set(lop)
        rop.parm('lopoutput').set('%s/%s.usda' % (usddir, name))
        rop.parm('trange').set('off')
        before = set()
        for r, ds, fs in os.walk(scene):
            for f in fs:
                before.add(os.path.join(r, f))
        rop.render()
        after = set()
        for r, ds, fs in os.walk(scene):
            for f in fs:
                after.add(os.path.join(r, f))
        written = sorted(after - before)
        st = Usd.Stage.Open('%s/%s.usda' % (usddir, name))
        fields = []
        for prim in st.Traverse():
            if str(prim.GetTypeName()) in FIELD_TYPES:
                # SOP-imported sims are time sampled: read at the current frame
                tc = Usd.TimeCode(hou.frame())
                fp = prim.GetAttribute('filePath').Get(tc)
                fields.append({'path': str(prim.GetPath()), 'type': str(prim.GetTypeName()),
                               'filePath': val(fp),
                               'timeSamples': prim.GetAttribute('filePath').GetNumTimeSamples(),
                               'fieldName': val(prim.GetAttribute('fieldName').Get(tc)),
                               'fieldIndex': val(prim.GetAttribute('fieldIndex').Get(tc)) if prim.GetAttribute('fieldIndex') else None})
        rops[name] = {'errors': rop.errors(), 'warnings': rop.warnings(),
                      'written': [os.path.relpath(w, scene).replace('\\', '/') + ' (%d bytes)' % os.path.getsize(w) for w in written],
                      'savedFields': fields}
    result['usd_rop'] = rops

print(json.dumps(result, indent=1))
