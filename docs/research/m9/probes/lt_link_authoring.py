"""M9 research probe (docs/research/m9/lights.md, section 4.1): what Houdini's
Light Linker LOP authors in USD. Isolated hython only
(tests/houdini/run_hython.ps1); builds its own LOP network at run time.

Scene: two spheres /World/sphA and /World/sphB, a ground cube, a cube
/World/other that no rule names, a rect light
/lights/key and a dome light /lights/dome. Light Linker rules:
  1. light link   key  -> includes /World/sphA            (light-centric)
  2. shadow link  key  -> excludes /World/sphB            (light-centric)
  3. light link   dome -> includes /World/ground, reversed (geometry-centric)
  4. light link   key2 -> includes "/World/sph*" as a USD path expression
Prints, for every light, the authored collection properties (lightLink,
shadowLink: includeRoot, includes, excludes, expansionRule,
membershipExpression) and what UsdCollectionAPI.ComputeMembershipQuery
reports for each geometry prim, after each rule.
Usage: hython lt_link_authoring.py <outfile>
"""

import sys

import hou
from pxr import Usd, UsdLux

OUT = sys.argv[1] if len(sys.argv) > 1 else None
LOG = []


def log(*args):
    line = ' '.join(str(a) for a in args)
    print(line, flush=True)
    LOG.append(line)


stage = hou.node('/stage')


def sphere(name, x):
    n = stage.createNode('sphere', name)
    n.parm('primpath').set('/World/' + name)
    n.parmTuple('t').set((x, 0, 0))
    return n


a = sphere('sphA', -1.5)
b = sphere('sphB', 1.5)
grid = stage.createNode('cube', 'ground')
grid.parm('primpath').set('/World/ground')
other = stage.createNode('cube', 'other')
other.parm('primpath').set('/World/other')
key = stage.createNode('light::2.0', 'key')
key.parm('primpath').set('/lights/key')
key.parm('lighttype').set('UsdLuxRectLight')
key2 = stage.createNode('light::2.0', 'key2')
key2.parm('primpath').set('/lights/key2')
dome = stage.createNode('domelight::3.0', 'dome')
dome.parm('primpath').set('/lights/dome')
merge = stage.createNode('merge')
for i, n in enumerate((a, b, grid, other, key, key2, dome)):
    merge.setInput(i, n)

linker = stage.createNode('lightlinker')
linker.setInput(0, merge)

GEOMS = ['/World/sphA', '/World/sphB', '/World/ground', '/World/other']


def dump(title):
    s = linker.stage()
    log('==', title)
    for lp in ('/lights/key', '/lights/key2', '/lights/dome'):
        prim = s.GetPrimAtPath(lp)
        log('  light %s (%s), applied schemas %s' % (lp, prim.GetTypeName(), list(prim.GetAppliedSchemas())))
        for prop in prim.GetProperties():
            n = prop.GetName()
            if not n.startswith('collection:'):
                continue
            if isinstance(prop, Usd.Relationship):
                log('    rel  %s = %s' % (n, [str(t) for t in prop.GetTargets()]))
            else:
                if prop.HasAuthoredValue():
                    log('    attr %s = %r' % (n, prop.Get()))
        light = UsdLux.LightAPI(prim)
        log('    resolved includeRoot: lightLink %r, shadowLink %r' % (
            light.GetLightLinkCollectionAPI().GetIncludeRootAttr().Get(),
            light.GetShadowLinkCollectionAPI().GetIncludeRootAttr().Get()))
        for cname, coll in (('lightLink', light.GetLightLinkCollectionAPI()),
                            ('shadowLink', light.GetShadowLinkCollectionAPI())):
            q = coll.ComputeMembershipQuery()
            members = ['%s:%d' % (g.rsplit('/', 1)[1], int(q.IsPathIncluded(g))) for g in GEOMS]
            log('    %s includes: %s' % (cname, ' '.join(members)))


dump('no rules')

linker.parm('num_links').set(4)
linker.parm('link_prim_1').set('/lights/key')
linker.parm('link_includes_1').set('/World/sphA')
linker.parm('link_type_1').set('light')
linker.parm('link_prim_2').set('/lights/key')
linker.parm('link_excludes_2').set('/World/sphB')
linker.parm('link_type_2').set('shadow')
linker.parm('link_prim_3').set('/World/ground')
linker.parm('link_includes_3').set('/lights/dome')
linker.parm('link_type_3').set('light')
linker.parm('link_reversed_3').set(1)
linker.parm('link_prim_4').set('/lights/key2')
linker.parm('link_includes_4').set('/World/sph*')
linker.parm('link_ispathexpression_4').set(1)
linker.parm('link_type_4').set('light')
dump('rules 1-4')

for i in range(1, 5):
    linker.parm('link_enabled_%d' % i).set(int(i == 2))
dump('rule 2 only (shadow link excludes sphB)')

for only in (1, 4):
    for i in range(1, 5):
        linker.parm('link_enabled_%d' % i).set(int(i == only))
    dump('rule %d only' % only)

if OUT:
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(LOG) + '\n')
