"""A stock VEX Material Builder (docs/design/M5.md, M5.4), generated at run
time in hython in an isolated environment (run_hython.ps1; ctest
houdini_vex_builder):

  * a Material Builder with a Principled Shader Core whose roughness comes
    from an anti-aliased noise of P, displaced along the normal by another
    noise, exports both functions' VFL as info:sourceCode;
  * husk renders it with Nova through the CVEX bridge from the usda alone
    (no hip file), surface and displacement, without compile warnings;
  * Karma CPU renders the same material, and the sphere's mean colour and
    displaced coverage agree.

Houdini-generated builder code inlines SideFX's VOP code, so it is generated
here and never committed.

Usage: hython test_vex_builder.py <outdir>
"""

import os
import subprocess
import sys

import hou

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
FAILURES = []


def check(cond, message):
    print(('ok   ' if cond else 'FAIL ') + message)
    if not cond:
        FAILURES.append(message)


stage = hou.node('/stage')
lib = stage.createNode('materiallibrary', 'materials')
builder = lib.createNode('materialbuilder', 'vexmat')
core = builder.createNode('principledshadercore', 'core')
core.parmTuple('basecolor').set((0.8, 0.3, 0.2))
noise = builder.createNode('aanoise', 'noise')
noise.parmTuple('freq').set((3.0, 3.0, 3.0))
noise.setNamedInput('pos', builder.node('surface_globals'), 'P')
core.setNamedInput('rough', noise, 'noise')
surface = builder.node('surface_output')
surface.setNamedInput('F', core, 'F')
surface.setNamedInput('Of', core, 'Of')
# Displacement along the normal by another noise (D8).
dglobals = builder.node('displacement_globals')
bumps = builder.createNode('aanoise', 'bumps')
bumps.parmTuple('freq').set((2.0, 2.0, 2.0))
bumps.setNamedInput('pos', dglobals, 'P')
displace = builder.createNode('displacenml', 'displace')
displace.parm('scale').set(0.4)
displace.setNamedInput('P', dglobals, 'P')
displace.setNamedInput('nN', dglobals, 'N')
displace.setNamedInput('amount', bumps, 'noise')
dout = builder.node('displacement_output')
dout.setNamedInput('P', displace, 'dispP')
dout.setNamedInput('N', displace, 'dispN')

# A polygon sphere: Karma does not run VEX displacement on an implicit
# UsdGeomSphere (measured; Nova does), so the two renderers compare on a mesh.
geo = hou.node('/obj').createNode('geo', 'ballgeo')
polysphere = geo.createNode('sphere')
polysphere.parm('type').set('polymesh')
polysphere.parm('rows').set(48)
polysphere.parm('cols').set(96)
sphere = stage.createNode('sopimport', 'ball')
sphere.parm('soppath').set(polysphere.path())
dome =stage.createNode('domelight::3.0', 'dome') if hou.nodeType(hou.lopNodeTypeCategory(), 'domelight::3.0') \
    else stage.createNode('domelight', 'dome')
cam = stage.createNode('camera', 'cam')
cam.parm('tz').set(7.0)
merge = stage.createNode('merge')
for i, n in enumerate((sphere, dome, cam, lib)):
    merge.setInput(i, n)
assign = stage.createNode('assignmaterial')
assign.setInput(0, merge)
assign.parm('primpattern1').set('/ball')
assign.parm('matspecpath1').set('/materials/vexmat')
settings = stage.createNode('nova_rendersettings')
settings.setInput(0, assign)
settings.parm('camera').set('/cameras/cam')
settings.parmTuple('resolution').set((64, 64))
settings.parm('samples').set(64)
scene = os.path.join(OUT, 'vex_builder.usda')
settings.stage().Export(scene)

# The builder's USD: the surface's VFL as source code in the vex context.
usd = settings.stage()
material = usd.GetPrimAtPath('/materials/vexmat')
check(material.IsValid(), 'the builder material is exported')
connection = material.GetAttribute('outputs:vex:surface')
targets = connection.GetConnections() if connection else []
check(len(targets) == 1, 'outputs:vex:surface is connected: %s' % targets)
shader = usd.GetPrimAtPath(targets[0].GetPrimPath()) if targets else None
code = shader.GetAttribute('info:sourceCode').Get() if shader else None
check(bool(code) and 'surface' in code, 'the surface shader carries its VFL as info:sourceCode (%d characters)'
      % len(code or ''))
connection = material.GetAttribute('outputs:vex:displacement')
targets = connection.GetConnections() if connection else []
shader = usd.GetPrimAtPath(targets[0].GetPrimPath()) if targets else None
code = shader.GetAttribute('info:sourceCode').Get() if shader else None
check(bool(code) and 'displace' in code, 'the displacement carries its VFL as info:sourceCode (%d characters)'
      % len(code or ''))

# The same scene without the displacement, for the displaced silhouette.
dout.setInput(0, None)
dout.setInput(1, None)
flat_scene = os.path.join(OUT, 'vex_builder_flat.usda')
settings.stage().Export(flat_scene)

hfs = os.environ['HFS']


def husk(renderer, output, path=scene):
    cmd = [os.path.join(hfs, 'bin', 'husk.exe'), '--renderer', renderer, '--settings', '/Render/rendersettings',
           '--pixel-samples', '64', '-o', output, path]
    try:
        return subprocess.run(cmd, capture_output=True, text=True, cwd=OUT, timeout=300)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, -1, '', 'timeout')


nova_picture = os.path.join(OUT, 'vex_builder_nova.exr')
nova_run = husk('Nova', nova_picture)
check(nova_run.returncode == 0 and os.path.exists(nova_picture), 'husk renders the builder material with Nova')
log = nova_run.stdout + nova_run.stderr
for bad in ('VEX compile failed', 'does not run', 'VEX shader:', 'render context'):
    check(bad not in log, 'Nova reports nothing like "%s"' % bad)
if nova_run.returncode != 0 or 'Warning' in log:
    print(log[-3000:])
karma_picture = os.path.join(OUT, 'vex_builder_karma.exr')
karma_run = husk('BRAY_HdKarma', karma_picture)
check(karma_run.returncode == 0 and os.path.exists(karma_picture), 'Karma renders the same material')
flat = {}
for renderer, name in (('Nova', 'nova'), ('BRAY_HdKarma', 'karma')):
    flat[name] = os.path.join(OUT, 'vex_builder_flat_%s.exr' % name)
    check(husk(renderer, flat[name], flat_scene).returncode == 0, '%s renders the undisplaced variant' % renderer)


def pixels(path):
    import OpenImageIO as oiio
    return oiio.ImageBuf(path).get_pixels(oiio.FLOAT)


def sphere_mean(path):
    """Mean colour of the pixels the sphere covers fully (alpha 1)."""
    p = pixels(path)
    covered = p[..., 3] > 0.999
    return [float(p[..., c][covered].mean()) for c in range(3)], int(covered.sum())


def moved(displaced, undisplaced):
    """Pixels whose coverage the displacement changes."""
    return int(((pixels(displaced)[..., 3] > 0.5) ^ (pixels(undisplaced)[..., 3] > 0.5)).sum())


if os.path.exists(nova_picture) and os.path.exists(karma_picture):
    # Each renderer's own covered pixels: Karma tessellates the implicit
    # sphere about 7 % smaller (docs/design/M4.md closing notes).
    nova_mean, nova_count = sphere_mean(nova_picture)
    karma_mean, karma_count = sphere_mean(karma_picture)
    worst = max(abs(n - k) / k for n, k in zip(nova_mean, karma_mean))
    check(worst <= 0.05, 'the sphere: Nova %s over %d pixels vs Karma %s over %d (max rel. deviation %.3f)'
          % ([round(v, 4) for v in nova_mean], nova_count, [round(v, 4) for v in karma_mean], karma_count, worst))
if all(os.path.exists(p) for p in (nova_picture, karma_picture, flat['nova'], flat['karma'])):
    nova_moved, karma_moved = moved(nova_picture, flat['nova']), moved(karma_picture, flat['karma'])
    check(nova_moved > 40 and abs(nova_moved - karma_moved) <= 0.3 * karma_moved,
          'the displacement moves the silhouette: %d pixels in Nova, %d in Karma' % (nova_moved, karma_moved))

print('%d failure(s)' % len(FAILURES))
sys.exit(1 if FAILURES else 0)
