# M9 research probe (curves-hair.md, section 1): what Houdini's LOPs author for
# SOP curves, grooms and points. Builds every SOP network at run time in an
# isolated hython (nothing generated is committed), imports it with SOP Import
# LOPs, saves the stage and prints the authored BasisCurves / NurbsCurves /
# Points attributes.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m9\probes\ch_author_curves.py <outdir>
#
# Writes <outdir>/ch_authored.usda (the flattened stage, input of the Hydra
# probe and of ch_karma_scenes.py's "authored" scene).
import os
import sys

import hou
from pxr import Usd, UsdGeom, Sdf

outdir = os.path.abspath(sys.argv[1]).replace('\\', '/')
os.makedirs(outdir, exist_ok=True)
hou.hipFile.clear(suppress_save_prompt=True)


def log(*a):
    print('[ch_author]', *a, flush=True)


geo = hou.node('/obj').createNode('geo', 'curves')

# A wavy open polyline of 6 points along +x; attributes are added per variant.
POLY = '''
int pts[];
for (int i = 0; i < 6; i++) {
    int p = addpoint(0, set(i * 0.4, 0.2 * sin(i), 0));
    append(pts, p);
}
addprim(0, "polyline", pts);
'''


def wrangle(name, code, cls='detail', inp=None):
    w = geo.createNode('attribwrangle', name)
    w.parm('class').set({'detail': 0, 'primitive': 1, 'point': 2, 'vertex': 3}[cls])
    w.parm('snippet').set(code)
    if inp is not None:
        w.setInput(0, inp)
    return w


base = wrangle('poly', POLY)
variants = {}
variants['poly_nowidth'] = base
variants['poly_ptwidth'] = wrangle('ptw', 'f@width = 0.02 + 0.01 * @ptnum; v@Cd = set(@ptnum / 5.0, 0.5, 1 - @ptnum / 5.0); v@uv = set(@ptnum / 5.0, 0, 0);', 'point', base)
variants['poly_primwidth'] = wrangle('primw', 'f@width = 0.05; v@Cd = {1, 0, 0};', 'primitive', base)
variants['poly_detailwidth'] = wrangle('detw', 'f@width = 0.07;', 'detail', base)
variants['poly_pscale'] = wrangle('pscale', 'f@pscale = 0.03;', 'point', base)
variants['poly_vtxuv'] = wrangle('vtxuv', 'v@uv = set(@vtxnum / 5.0, 0.25, 0);', 'vertex', base)
variants['poly_N'] = wrangle('ptN', 'v@N = {0, 0, 1}; f@width = 0.05;', 'point', base)
variants['poly_closed'] = wrangle('closed', 'setprimintrinsic(0, "closed", 0, 1);', 'detail', base)
# Polygon -> NURBS / Bezier curves (Convert SOP), order 4.
for name, totype in (('nurbs', 'nurbCurve'), ('bezier', 'bezCurve')):
    c = geo.createNode('convert', 'to_' + name)
    # A cubic Bezier needs 3n+1 points: 7 for the Bezier variant.
    c.setInput(0, variants['poly_ptwidth'] if name == 'nurbs' else
               wrangle('poly7', POLY.replace('i < 6', 'i < 7') + 'for (int i = 0; i < 7; i++) setpointattrib(0, "width", i, 0.02 + 0.01 * i);'))
    menu = c.parm('totype').menuItems()
    c.parm('totype').set(list(menu).index(totype) if totype in menu else 0)
    variants[name] = c
    log(name, 'convert menu', menu, '->', c.parm('totype').evalAsString())

# Groom: Hair Generate on a sphere (no guides), and Guide Groom guides.
sph = geo.createNode('sphere', 'skin')
sph.parm('type').set(1)  # polygon
sph.parmTuple('rad').set((0.5, 0.5, 0.5))
hg = geo.createNode('hairgen', 'hairgen')
hg.setInput(0, sph)
for pn, v in (('density', 200.0),):
    if hg.parm(pn):
        hg.parm(pn).set(v)
variants['hairgen'] = hg
# Closed (periodic) NURBS and Bezier curves: Circle SOP, order 4.
for name, ctype in (('circle_nurbs', 'nurbs'), ('circle_bezier', 'bezier')):
    ci = geo.createNode('circle', name)
    menu = ci.parm('type').menuItems()
    ci.parm('type').set(list(menu).index(ctype) if ctype in menu else 0)
    ci.parm('divs').set(8)
    variants[name] = ci
    log(name, 'circle type menu', menu, '->', ci.parm('type').evalAsString())

# Points: a particle-like cloud with pscale, and one with width.
pts = wrangle('points', 'for (int i = 0; i < 5; i++) { int p = addpoint(0, set(i * 0.3, 0, 0)); setpointattrib(0, "pscale", p, 0.05 + 0.01 * i); setpointattrib(0, "Cd", p, set(1, i / 4.0, 0)); }')
variants['points_pscale'] = pts
ptsw = wrangle('pointsw', 'for (int i = 0; i < 5; i++) { int p = addpoint(0, set(i * 0.3, 0.5, 0)); setpointattrib(0, "width", p, 0.1); }')
variants['points_width'] = ptsw

for name, node in variants.items():
    g = node.geometry()
    types = sorted(set(p.type().name() for p in g.prims()))
    attrs = {k: [a.name() for a in getattr(g, k)()] for k in ('pointAttribs', 'vertexAttribs', 'primAttribs', 'globalAttribs')}
    log('SOP', name, 'prims', len(g.prims()), types, 'points', len(g.points()), attrs,
        [(p.intrinsicValue('order') if 'order' in p.intrinsicNames() else None,
          p.intrinsicValue('closed') if 'closed' in p.intrinsicNames() else None,
          p.intrinsicValue('knots') if 'knots' in p.intrinsicNames() else None) for p in g.prims()][:1])
    if node.errors():
        log('  errors', node.errors())

# ---------------------------------------------------------------- LOPs
stage_net = hou.node('/stage')
merge = stage_net.createNode('merge', 'merge_all')
imports = {}
for i, (name, node) in enumerate(variants.items()):
    si = stage_net.createNode('sopimport', 'imp_' + name)
    si.parm('soppath').set(node.path())
    si.parm('pathprefix').set('/curves/' + name)
    merge.setInput(i, si)
    imports[name] = si
# NURBS handling variants
si_menu = imports['nurbs'].parm('nurbscurvehandling').menuItems()
log('nurbscurvehandling menu', si_menu, 'default', imports['nurbs'].parm('nurbscurvehandling').evalAsString())
log('setmissingwidths default', imports['nurbs'].parm('setmissingwidths').eval(),
    'polygonsassubd default', imports['nurbs'].parm('polygonsassubd').eval())
extra = len(variants)
for tok in si_menu:
    if tok == imports['nurbs'].parm('nurbscurvehandling').evalAsString():
        continue
    si = stage_net.createNode('sopimport', 'imp_nurbs_' + tok)
    si.parm('soppath').set(variants['nurbs'].path())
    si.parm('pathprefix').set('/curves/nurbs_' + tok)
    si.parm('enable_nurbscurvehandling').set(1)
    si.parm('nurbscurvehandling').set(tok)
    merge.setInput(extra, si)
    extra += 1
# Polygons as subdivision: open polylines become B-splines (Karma's hint for oriented ribbons).
si = stage_net.createNode('sopimport', 'imp_poly_subd')
si.parm('soppath').set(variants['poly_N'].path())
si.parm('pathprefix').set('/curves/poly_N_subd')
si.parm('enable_polygonsassubd').set(1)
si.parm('polygonsassubd').set(1)
merge.setInput(extra, si)
extra += 1

stage = merge.stage()
out = outdir + '/ch_authored.usda'
stage.Flatten().Export(out)
log('saved', out)

# ---------------------------------------------------------------- dump
def short(v, n=8):
    if v is None:
        return None
    try:
        lst = list(v)
    except TypeError:
        return v
    s = [tuple(round(float(c), 4) for c in x) if hasattr(x, '__len__') else (round(float(x), 4) if isinstance(x, float) else x)
         for x in lst[:n]]
    return '%s%s (len %d)' % (s, '...' if len(lst) > n else '', len(lst))


st = Usd.Stage.Open(out)
for prim in st.Traverse():
    t = prim.GetTypeName()
    if t not in ('BasisCurves', 'NurbsCurves', 'Points', 'HermiteCurves'):
        continue
    log('PRIM', prim.GetPath(), t)
    for a in prim.GetAttributes():
        if not a.HasAuthoredValue():
            continue
        n = a.GetName()
        v = a.Get(Usd.TimeCode.Default()) if a.GetNumTimeSamples() == 0 else a.Get(a.GetTimeSamples()[0])
        interp = a.GetMetadata('interpolation')
        log('   %-34s %-12s %s%s' % (n, str(a.GetTypeName()), short(v) if not isinstance(v, (str, bool)) else v,
                                    ('  interp=' + interp) if interp else ''))
    if t == 'BasisCurves':
        bc = UsdGeom.BasisCurves(prim)
        log('   widths interpolation (schema):', bc.GetWidthsInterpolation(), ' normals interpolation:', bc.GetNormalsInterpolation())
