"""The Karma Pyro Material in Nova (docs/design/M8.md D4, D5, D14; ctest
houdini_pyro_material), generated at run time in an isolated hython from
Houdini's own tool recipe (sidefx::recipe::vop::kma_pyromaterial) and never
committed:

  * the material reaches husk as a MaterialX network ending in kma_volume;
  * Nova translates it without warnings into a volume shader that reads the
    density and temperature fields, with fire off (the recipe's default) and
    on in ramp and blackbody colour modes;
  * the volume bound to it loads exactly those fields and every majorant cell
    has an interval bound (no warning about estimated cells).

Usage: hython test_pyro_material.py <repo> <outdir>
"""

import os
import subprocess
import sys

import hou

REPO = os.path.abspath(sys.argv[1])
OUT = os.path.abspath(sys.argv[2])
os.makedirs(OUT, exist_ok=True)
FAILURES = []


def check(cond, message):
    print(('ok   ' if cond else 'FAIL ') + message, flush=True)
    if not cond:
        FAILURES.append(message)


stage = hou.node('/stage')
pyro = stage.createNode('volume', 'pyro')
pyro.parm('primpath').set('/pyro')
pyro.parm('filepath1').set(os.path.join(REPO, 'tests', 'data', 'volumes', 'pyro.vdb').replace('\\', '/'))
matlib = stage.createNode('materiallibrary', 'materials')
matlib.setInput(0, pyro)
from hrecipes import rtypes, storage  # noqa: E402  (Houdini's recipe API, headless)
ref = storage.recipeStorage().refForName('sidefx::recipe::vop::kma_pyromaterial', [rtypes.RecipeCategory.tool])
ref.instanceInsideNode(matlib)
material = [c for c in matlib.children() if c.type().name() != 'output'][0]
shader = material.node('kma_pyroshader1')
check(shader is not None, 'the recipe builds a Karma Pyro Material (%s)' % material.path())
matlib.parm('matnode1').set(material.name())
assign = stage.createNode('assignmaterial')
assign.setInput(0, matlib)
assign.parm('primpattern1').set('/pyro')
assign.parm('matspecpath1').set('/materials/' + material.name())
cam = stage.createNode('camera', 'cam')
cam.setInput(0, assign)
cam.parm('tz').set(8.0)
cam.parm('ty').set(2.0)
settings = stage.createNode('nova_rendersettings')
settings.setInput(0, cam)
settings.parm('camera').set('/cameras/cam')
settings.parmTuple('resolution').set((32, 32))
settings.parm('samples').set(1)

hfs = os.environ['HFS']
env = dict(os.environ, TF_DEBUG='HDNOVA_MATERIAL HDNOVA_VOLUME')
variants = [('smoke', {}), ('fire_ramp', {'enablefire': 1}), ('fire_blackbody', {'enablefire': 1, 'firecolormode': 1})]
for name, parms in variants:
    for p, v in parms.items():
        shader.parm(p).set(v)
    scene = os.path.join(OUT, 'pyro_%s.usda' % name)
    settings.stage().Export(scene)
    run = subprocess.run([os.path.join(hfs, 'bin', 'husk.exe'), '--renderer', 'Nova', '--settings', '/Render/rendersettings',
                          '-o', os.path.join(OUT, 'pyro_%s.exr' % name), scene],
                         capture_output=True, text=True, env=env, cwd=OUT, timeout=300)
    log = run.stdout + run.stderr
    with open(os.path.join(OUT, 'pyro_%s.log' % name), 'w') as f:
        f.write(log)
    check(run.returncode == 0, '%s: husk renders with Nova' % name)
    check('Warning' not in log, '%s: no warnings' % name)
    shaders = [l for l in log.splitlines() if 'has a volume shader' in l]
    check(len(shaders) == 1, '%s: the material translates to a volume shader' % name)
    reads = shaders[0].split('reads ', 1)[1] if shaders else ''
    expected = {'density', 'temperature'} if parms.get('enablefire') else {'density'}
    check(expected <= set(r.strip() for r in reads.split(',')), '%s: it reads %s' % (name, reads))
    loaded = [l for l in log.splitlines() if '[Nova] volume /pyro' in l]
    check(any('%d fields loaded' % len(expected) in l for l in loaded),
          '%s: the volume loads %d field(s): %s' % (name, len(expected), loaded[-1:] if loaded else 'nothing'))
    if FAILURES:
        print(log[-3000:])

print('%d failure(s)' % len(FAILURES))
sys.exit(1 if FAILURES else 0)
