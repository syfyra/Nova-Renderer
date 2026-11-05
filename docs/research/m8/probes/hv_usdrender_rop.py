# M8 research probe (houdini-volumes.md): what husk sees for in-memory SOP
# volumes when a USD Render ROP renders a LOP network out of process.
# Loads hv_scene.hip (hv_build_scene.py), appends a camera and a Karma Render
# Settings LOP to the pyro stage, renders it with a USD Render ROP and lists
# the files the ROP left in HOUDINI_TEMP_DIR / next to the output. Run it with
# the VolumeProbe delegate (hv_volume_probe) staged so husk's field bprims log
# their filePath to the ROP's output:
#
#   $env:HV_PROBE_HELPER=<hv_volume_helper.dll>; $env:HOUDINI_TEMP_DIR=<empty dir>
#   powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m8\probes\hv_usdrender_rop.py -Stage <probe pkg> <scene dir> <out dir> [renderer]
import os
import sys

import hou

scene = os.path.abspath(sys.argv[1])
outdir = os.path.abspath(sys.argv[2])
renderer = sys.argv[3] if len(sys.argv) > 3 else 'HdVolumeProbeRendererPlugin'
os.makedirs(outdir, exist_ok=True)
hou.hipFile.load(os.path.join(scene, 'hv_scene.hip').replace('\\', '/'), suppress_save_prompt=True, ignore_load_warnings=True)
hou.setFrame(16)


def log(*a):
    print('[hv_rop]', *a, flush=True)


net = hou.node('/stage/pyro_stage')
cam = net.createNode('camera', 'cam')
cam.setInput(0, hou.node('/stage/pyro_stage/OUT'))
cam.parm('primpath').set('/cameras/cam')
cam.parmTuple('t').set((0, 2, 12))
krs = net.createNode('karmarendersettings', 'krs')
krs.setInput(0, cam)
krs.parm('camera').set('/cameras/cam')
try:
    krs.parmTuple('resolution').set((64, 64))
except hou.PermissionError:
    log('resolution is locked on the Karma Render Settings LOP; keeping the default')

rop = hou.node('/out').createNode('usdrender', 'render_probe')
rop.parm('loppath').set(krs.path())
rop.parm('renderer').set(renderer)
rop.parm('trange').set('off')
names = [p.name() for p in rop.parms()]
log('usdrender parms (save/husk/usd/temp/file):', [n for n in names if any(k in n.lower() for k in ('save', 'husk', 'usdfile', 'temp', 'keep', 'output', 'verbose', 'flatten', 'delegate'))])
for n in ('outputimage', 'husk_verbosity', 'verbose', 'husk_all_frames'):
    if rop.parm(n) is not None:
        log('  ', n, '=', rop.parm(n).eval())
if rop.parm('outputimage') is not None:
    rop.parm('outputimage').set(os.path.join(outdir, 'rop_$F4.exr').replace('\\', '/'))
temp = os.environ.get('HOUDINI_TEMP_DIR', '')
log('HOUDINI_TEMP_DIR', temp)


def listing(root):
    out = set()
    if root and os.path.isdir(root):
        for r, ds, fs in os.walk(root):
            for f in fs:
                out.add(os.path.join(r, f))
    return out


before = listing(temp) | listing(outdir) | listing(scene)
rop.render(verbose=True, output_progress=True)
after = listing(temp) | listing(outdir) | listing(scene)
log('ROP errors', rop.errors(), 'warnings', rop.warnings())
for f in sorted(after - before):
    log('new file', f, os.path.getsize(f))
