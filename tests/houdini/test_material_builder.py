"""The Nova Material Builder (docs/design/M4.md 3.7, M4.8), in hython in an
isolated environment (run_hython.ps1; ctest houdini_material_builder):

  * the tool is on the shelf and its module loads from the package;
  * a Nova builder exports the same USD structure as Karma's builder (the
    kma context, the same outputs and shaders);
  * its tab mask hides the Karma nodes Nova does not render;
  * a builder material with Karma's AO, ray switch and constant ramp renders
    in Nova without unsupported-node warnings, and in Karma.

Usage: hython test_material_builder.py <outdir>
"""

import os
import subprocess
import sys

import hou
import voptoolutils

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
FAILURES = []


def check(cond, message):
    print(('ok   ' if cond else 'FAIL ') + message)
    if not cond:
        FAILURES.append(message)


import novautils  # noqa: E402 (from the package's python3.13libs)

check(os.path.normcase(novautils.__file__).replace('\\', '/').find('/nova/python3.13libs/') >= 0,
      'novautils comes from the staged package: %s' % novautils.__file__)
check('vop_novamtlxsubnet' in hou.shelves.tools(), 'the Nova Material Builder tool is on the shelf')

stage = hou.node('/stage')
lib = stage.createNode('materiallibrary', 'materials')
nova = novautils.createBuilder(lib, 'novamat')
karma = lib.createNode('subnet', 'karmamat')
karma.setShaderLanguageName('MaterialX')
voptoolutils._setupMtlXBuilderSubnet(subnet_node=karma, mask=voptoolutils.KARMAMTLX_TAB_MASK,
                                     folder_label='Karma Material Builder', render_context='kma')
mask = nova.parm('tabmenumask').eval()
for hidden in ('^kma_hair', '^kma_fur*', '^kma_volume', '^kma_lfilter_*'):
    check(hidden in mask.split(), 'the tab mask hides %s' % hidden[1:])
check('karma' in mask.split() and 'MaterialX' in mask.split(), 'the tab mask keeps Karma and MaterialX nodes')
check(nova.parm('shader_rendercontextname').eval() == 'kma', 'the builder writes the kma context')

# The same USD structure as Karma's builder.
usd = lib.stage()


def structure(path):
    prim = usd.GetPrimAtPath(path)
    outputs = sorted(a.GetName() for a in prim.GetAttributes() if a.GetName().startswith('outputs:'))
    shaders = sorted(str(c.GetAttribute('info:id').Get()) for c in prim.GetChildren() if c.GetAttribute('info:id'))
    return prim.IsValid(), outputs, shaders


nv, nout, nsh = structure('/materials/novamat')
kv, kout, ksh = structure('/materials/karmamat')
check(nv and kv, 'both materials are exported')
check(nout == kout, 'the same outputs: %s' % nout)
check(nsh == ksh, 'the same shaders: %s' % nsh)
check('outputs:kma:surface' in nout and 'outputs:kma:displacement' in nout, 'kma surface and displacement outputs')

# Karma's AO -> ray switch (camera) -> constant ramp -> base color.
surface = nova.node('mtlxstandard_surface')
ao = nova.createNode('kma_ao', 'ao')
ao.parm('maxdistance').set(0.5)
switch = nova.createNode('kma_rayswitch', 'switch')
switch.setNamedInput('camera', ao, 'visibility')
ramp = nova.createNode('kma_rampconst', 'ramp')
ramp.parm('signature').set('color3')
ramp.setNamedInput('t', switch, 'result')
surface.setNamedInput('base_color', ramp, 'out')

# A sphere beside a box under a dome, with the material.
sphere = stage.createNode('sphere', 'ball')
grid = stage.createNode('cube', 'ground')
move = grid.parmTuple('t') or grid.parmTuple('translate')
if move is not None:
    move.set((1.8, 0.0, 0.0))
dome = stage.createNode('domelight::3.0', 'dome') if hou.nodeType(hou.lopNodeTypeCategory(), 'domelight::3.0') \
    else stage.createNode('domelight', 'dome')
cam = stage.createNode('camera', 'cam')
cam.parm('tz').set(6.0)
merge = stage.createNode('merge')
for i, n in enumerate((sphere, grid, dome, cam, lib)):
    merge.setInput(i, n)
assign = stage.createNode('assignmaterial')
assign.setInput(0, merge)
assign.parm('primpattern1').set('/ball /ground')
assign.parm('matspecpath1').set('/materials/novamat')
settings = stage.createNode('nova_rendersettings')
settings.setInput(0, assign)
settings.parm('camera').set('/cameras/cam')
settings.parmTuple('resolution').set((96, 72))
settings.parm('samples').set(8)
picture = os.path.join(OUT, 'builder_nova.exr').replace('\\', '/')
settings.parm('picture').set(picture)
scene = os.path.join(OUT, 'builder.usda')
settings.stage().Export(scene)

hfs = os.environ['HFS']


def husk(renderer, output):
    cmd = [os.path.join(hfs, 'bin', 'husk.exe'), '--renderer', renderer, '--settings', '/Render/rendersettings']
    if output:
        cmd += ['-o', output]
    cmd.append(scene)
    try:
        return subprocess.run(cmd, capture_output=True, text=True, cwd=OUT, timeout=300)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, -1, '', 'timeout')


nova_run = husk('Nova', None)
check(nova_run.returncode == 0, 'husk renders the builder material with Nova')
log = nova_run.stdout + nova_run.stderr
for bad in ('not supported', 'unknown node', 'unsupported'):
    check(bad not in log, 'Nova reports nothing %s' % bad)
if nova_run.returncode != 0 or 'not supported' in log:
    print(log[-3000:])
check(os.path.exists(picture), 'Nova wrote the image')
karma_picture = os.path.join(OUT, 'builder_karma.exr').replace('\\', '/')
karma_run = husk('BRAY_HdKarma', karma_picture)
check(karma_run.returncode == 0 and os.path.exists(karma_picture), 'Karma renders the same material')
if karma_run.returncode != 0:
    print((karma_run.stdout + karma_run.stderr)[-2000:])

print('%d failure(s)' % len(FAILURES))
sys.exit(1 if FAILURES else 0)
