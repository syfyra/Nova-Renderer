# M8 research probe (houdini-volumes.md): which volume-related Karma settings
# the Karma Render Settings LOP and the Render Geometry Settings LOP expose,
# their defaults, menus, and the USD attributes they author.
#
#   powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m8\probes\hv_karma_volume_settings.py
import re

import hou

PAT = re.compile(r'vol|screendoor|lightsource|uniform|steprate|emission|light_sampling|limit|isportal|samplingchannel|filterwidth', re.I)


def decoded(name):
    try:
        return hou.text.decode(name)
    except Exception:
        return name


def dump_type(lopnode):
    print('######', lopnode.type().name())
    rows = []
    for p in lopnode.parms():
        name = p.name()
        dn = decoded(name)
        if not PAT.search(dn) and not PAT.search(p.parmTemplate().label()):
            continue
        if dn.endswith('_control'):
            continue
        t = p.parmTemplate()
        menu = ''
        try:
            if t.menuItems():
                menu = ' menu=' + ','.join('%s:%s' % (a, b) for a, b in zip(t.menuItems(), t.menuLabels()))
        except Exception:
            pass
        rows.append('%-60s [%s] default=%r%s' % (dn, t.label(), p.eval(), menu))
    for r in rows:
        print('  ' + r)


stage = hou.node('/stage')
krs = stage.createNode('karmarendersettings', 'krs')
rgs = stage.createNode('rendergeometrysettings', 'rgs')
rgs.setInput(0, krs)
dump_type(krs)
dump_type(rgs)

# Author everything that has a *_control menu ("set") on the geometry settings
# and see which USD attributes come out, with their default values.
for node in (rgs,):
    for p in node.parms():
        if p.name().endswith('_control'):
            dn = decoded(p.name())
            if PAT.search(dn):
                try:
                    p.set('set')
                except Exception:
                    pass
st = rgs.stage()
print('###### authored attributes')
for prim in st.Traverse():
    for a in prim.GetAttributes():
        n = a.GetName()
        if a.HasAuthoredValue() and PAT.search(n):
            print('  %-30s %-60s %-10s %r' % (prim.GetPath(), n, a.GetTypeName(), a.Get()))
