"""Volume data paths (docs/design/M8.md D1, D3; ctest houdini_volumes), in
an isolated hython (run_hython.ps1):

  * the volume bridge (nova_volume.dll, ctypes) reads every field the LOPs
    author for in-memory SOP volumes ("op:" paths, native volumes and VDBs)
    with the voxels and transform Houdini has, follows a frame change (new
    "&t=" path), reads the same volumes from a .bgeo.sc file and reports a
    missing file;
  * husk renders a USD-ROP-saved stage (the volumes in .volumes/ files) and
    a Volume LOP .vdb with Nova, loading exactly the density fields, without
    warnings;
  * Hydra in this process (UsdImagingGL without a GPU, as the Solaris
    viewport) loads the op: fields, and again after a frame change.

Usage: hython test_volumes.py <stage dir> <outdir> [inprocess <hip>]
"""

import ctypes
import os
import subprocess
import sys

import hou

STAGE_DIR = os.path.abspath(sys.argv[1])
OUT = os.path.abspath(sys.argv[2])
os.makedirs(OUT, exist_ok=True)
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FAILURES = []


def check(cond, message):
    print(('ok   ' if cond else 'FAIL ') + message, flush=True)
    if not cond:
        FAILURES.append(message)


def render_in_process(hip):
    """Hydra in this process: Nova's TF_DEBUG lines go to stdout."""
    from pxr import Gf, UsdImagingGL
    hou.hipFile.load(hip, suppress_save_prompt=True, ignore_load_warnings=True)
    params = UsdImagingGL.Engine.Parameters()
    params.gpuEnabled = False
    engine = UsdImagingGL.Engine(params)
    if not engine.SetRendererPlugin('HdNovaRendererPlugin'):
        print('NO NOVA RENDERER', flush=True)
        sys.exit(2)
    engine.SetRenderBufferSize(Gf.Vec2i(32, 32))
    engine.SetRenderViewport(Gf.Vec4d(0, 0, 32, 32))
    cam = Gf.Matrix4d().SetLookAt(Gf.Vec3d(0, 0, 6), Gf.Vec3d(0, 0, 0), Gf.Vec3d(0, 1, 0))
    proj = Gf.Frustum()
    proj.SetPerspective(30.0, 1.0, 0.1, 1000.0)
    engine.SetCameraState(cam, proj.ComputeProjectionMatrix())
    for frame in (1, 2):
        hou.setFrame(frame)
        stage = hou.node('/stage/OUT').stage()
        rp = UsdImagingGL.RenderParams()
        rp.frame = frame
        print('FRAME %d' % frame, flush=True)
        for _ in range(2):
            engine.Render(stage.GetPseudoRoot(), rp)
    print('IN-PROCESS DONE', flush=True)
    sys.exit(0)


if len(sys.argv) > 4 and sys.argv[3] == 'inprocess':
    render_in_process(sys.argv[4])

# --- SOP volumes: native density and temperature, the same as VDBs ---------
hou.hipFile.clear(suppress_save_prompt=True)
hou.setFrame(1)
geo = hou.node('/obj').createNode('geo', 'vols')
fields = []
for name, expr in (('density', 'max(0, @P.x + 0.5) * (1.0 + 0.1 * @Frame)'), ('temperature', '1.0 + @P.y')):
    vol = geo.createNode('volume', name)
    vol.parm('name').set(name)
    vol.parm('uniformsamples').set('size')
    vol.parm('divsize').set(0.05)
    wrangle = geo.createNode('volumewrangle', name + '_values')
    wrangle.setInput(0, vol)
    wrangle.parm('snippet').set('@%s = %s;' % (name, expr))
    fields.append(wrangle)
merge = geo.createNode('merge', 'fields')
for i, f in enumerate(fields):
    merge.setInput(i, f)
native = geo.createNode('null', 'OUT_NATIVE')
native.setInput(0, merge)
vdb = geo.createNode('convertvdb', 'to_vdb')
vdb.parm('conversion').set('vdb')
vdb.setInput(0, native)
vdbs = geo.createNode('null', 'OUT_VDB')
vdbs.setInput(0, vdb)

stage = hou.node('/stage')
imports = []
for name, sop in (('native', native), ('vdbs', vdbs)):
    lop = stage.createNode('sopimport', name)
    lop.parm('soppath').set(sop.path())
    lop.parm('enable_pathprefix').set(1)
    lop.parm('pathprefix').set('/' + name)
    imports.append(lop)
pyro = stage.createNode('volume', 'pyrofile')
pyro.parm('primpath').set('/pyrofile')
pyro.parm('filepath1').set(os.path.join(REPO, 'tests', 'data', 'volumes', 'pyro.vdb').replace('\\', '/'))
cam = stage.createNode('camera', 'cam')
cam.parm('tz').set(6.0)
lmerge = stage.createNode('merge', 'all')
for i, n in enumerate(imports + [pyro, cam]):
    lmerge.setInput(i, n)
settings = stage.createNode('nova_rendersettings')
settings.setInput(0, lmerge)
settings.parm('camera').set('/cameras/cam')
settings.parmTuple('resolution').set((32, 32))
settings.parm('samples').set(1)
out = stage.createNode('null', 'OUT')
out.setInput(0, settings)

hip = os.path.join(OUT, 'volumes.hip').replace('\\', '/')
hou.hipFile.save(hip)

# --- The bridge on the authored field prims --------------------------------
bridge = ctypes.CDLL(os.path.join(STAGE_DIR, 'dso', 'usd_plugins', 'nova_volume.dll'))
bridge.novaVolumeFieldStats.restype = ctypes.c_int
bridge.novaVolumeFieldStats.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                        ctypes.POINTER(ctypes.c_double), ctypes.c_char_p, ctypes.c_int]


def stats(path, name, index, houdini):
    values = (ctypes.c_double * 17)()
    error = ctypes.create_string_buffer(512)
    ok = bridge.novaVolumeFieldStats(path.encode(), name.encode(), index, 1 if houdini else 0, values, error, 512)
    return ok, list(values), error.value.decode(errors='replace')


def houdini_volume(sop, name):
    """Non-zero voxels, their sum and the index map of a native volume."""
    for prim in sop.geometry().prims():
        if prim.type() == hou.primType.Volume and prim.attribValue('name') == name:
            voxels = [v for v in prim.allVoxels() if v != 0.0]
            p0 = prim.indexToPos((0, 0, 0))
            px = prim.indexToPos((1, 0, 0))
            return prim.number(), len(voxels), sum(voxels), (px - p0)[0], p0
    return None


def field_prims(usd_stage):
    for prim in usd_stage.Traverse():
        kind = str(prim.GetTypeName())
        if kind in ('OpenVDBAsset', 'HoudiniFieldAsset'):
            path = prim.GetAttribute('fieldIndex')
            index = int(path.Get(hou.frame())) if path and path.Get(hou.frame()) is not None else 0
            asset = prim.GetAttribute('filePath').Get(hou.frame())
            yield (str(prim.GetPath()), kind, str(prim.GetAttribute('fieldName').Get(hou.frame())), index,
                   asset.resolvedPath or asset.path)


def check_sop_fields(frame):
    hou.setFrame(frame)
    usd = out.stage()
    seen = {}
    for path, kind, name, index, file in field_prims(usd):
        if not file.startswith('op:'):
            continue
        ok, v, error = stats(file, name, index, kind == 'HoudiniFieldAsset')
        check(ok == 1 and v[0] == 1, 'frame %d: the bridge reads %s (%s) %s' % (frame, path, kind, error))
        if not ok:
            continue
        ref = houdini_volume(native, name)
        seen[(kind, name)] = (v, file)
        if kind == 'HoudiniFieldAsset' and ref:
            _, count, total, voxel, p0 = ref
            check(int(v[1]) == count and abs(v[4] - total) <= 1e-4 * max(1.0, abs(total)),
                  'frame %d: %s has Houdini\'s %d voxels and sum %.4f (bridge: %d, %.4f)'
                  % (frame, name, count, total, int(v[1]), v[4]))
            check(abs(v[5] - voxel) < 1e-6 and abs(v[14] - p0[0]) < 1e-5 and abs(v[15] - p0[1]) < 1e-5,
                  'frame %d: %s index (0,0,0) at %s, voxel %.4f' % (frame, name, tuple(p0), voxel))
    for name in ('density', 'temperature'):
        native_v = seen.get(('HoudiniFieldAsset', name))
        vdb_v = seen.get(('OpenVDBAsset', name))
        check(native_v is not None and vdb_v is not None, 'frame %d: %s arrives as native volume and VDB' % (frame, name))
        if native_v and vdb_v:
            a, b = native_v[0][4], vdb_v[0][4]
            check(abs(a - b) <= 1e-3 * max(1.0, abs(a)), 'frame %d: the VDB %s sums like the native (%.4f, %.4f)'
                  % (frame, name, a, b))
    return seen


first = check_sop_fields(1)
second = check_sop_fields(2)
d1, d2 = first.get(('HoudiniFieldAsset', 'density')), second.get(('HoudiniFieldAsset', 'density'))
if d1 and d2:
    check(d1[1] != d2[1], 'a frame change gives a new op: path')
    check(abs(d2[0][4] / d1[0][4] - 1.2 / 1.1) < 1e-4, 'and the new frame\'s voxels (sum ratio %.5f)'
          % (d2[0][4] / d1[0][4]))
hou.setFrame(1)

# The same volumes from a .bgeo.sc file, and a missing file.
bgeo = os.path.join(OUT, 'native.bgeo.sc').replace('\\', '/')
native.geometry().saveToFile(bgeo)
for name in ('density', 'temperature'):
    number, count, total, _, _ = houdini_volume(native, name)
    ok, v, error = stats(bgeo, name, number, True)
    check(ok == 1 and int(v[1]) == count and abs(v[4] - total) <= 1e-4 * max(1.0, abs(total)),
          'the bridge reads %s from a .bgeo.sc (%d voxels, sum %.4f) %s' % (name, int(v[1]), v[4], error))
ok, _, error = stats(os.path.join(OUT, 'missing.bgeo.sc').replace('\\', '/'), 'density', 0, True)
check(ok == 0 and error, 'a missing file is reported: %s' % error)

# --- husk on the saved stage --------------------------------------------------
rop = stage.createNode('usd_rop', 'save')
rop.parm('loppath').set(out.path())
saved = os.path.join(OUT, 'saved', 'volumes.usd').replace('\\', '/')
rop.parm('lopoutput').set(saved)
rop.parm('trange').set('off')
rop.render()
check(os.path.exists(saved), 'the USD ROP saves the stage')
hfs = os.environ['HFS']
env = dict(os.environ, TF_DEBUG='HDNOVA_VOLUME')
run = subprocess.run([os.path.join(hfs, 'bin', 'husk.exe'), '--renderer', 'Nova', '--settings', '/Render/rendersettings',
                      '-o', os.path.join(OUT, 'saved.exr'), saved], capture_output=True, text=True, env=env, cwd=OUT,
                     timeout=300)
log = run.stdout + run.stderr
loads = [line for line in log.splitlines() if '[Nova] field' in line and '1 grid(s)' in line]
check(run.returncode == 0, 'husk renders the saved stage with Nova')
check('Warning' not in log, 'husk prints no warnings')
check(len(loads) == 3, 'husk loads the three density fields (native, VDB, Volume LOP): %d' % len(loads))
check(any('.bgeo.sc' in l for l in loads) and any('.vdb' in l for l in loads),
      'from the saved .volumes files and the .vdb')
if run.returncode != 0 or len(loads) != 3 or 'Warning' in log:
    print(log[-4000:])

# --- the USD Render ROP, as Nova (Setup) renders: husk on the stage the ROP
# writes, its in-memory SOP volumes saved to temporary files ------------------
render_rop = stage.createNode('usdrender_rop', 'render')
render_rop.parm('loppath').set(out.path())
render_rop.parm('renderer').set('HdNovaRendererPlugin')
render_rop.parm('rendersettings').set('/Render/rendersettings')
image = os.path.join(OUT, 'rop.exr').replace('\\', '/')
render_rop.parm('outputimage').set(image)
try:
    render_rop.render()
except hou.OperationFailed as e:
    print('USD Render ROP:', e)
check(os.path.exists(image) and not render_rop.errors(), 'the USD Render ROP renders the volumes with Nova %s'
      % list(render_rop.errors()))
if os.path.exists(image):
    import OpenImageIO as oiio
    alpha = float(oiio.ImageBuf(image).get_pixels(oiio.FLOAT)[..., 3].mean())
    check(alpha > 0.01, 'its image shows the volumes (mean alpha %.4f)' % alpha)

# --- Hydra in this process, as the Solaris viewport ---------------------------
run = subprocess.run([os.path.join(hfs, 'bin', 'hython.exe'), os.path.abspath(__file__), STAGE_DIR, OUT, 'inprocess', hip],
                     capture_output=True, text=True, env=env, timeout=300)
log = run.stdout + run.stderr
check(run.returncode == 0 and 'IN-PROCESS DONE' in log, 'Nova renders in the cooking process')
frames = log.split('FRAME 2')
before = [l for l in frames[0].splitlines() if '[Nova] field' in l]
after = [l for l in frames[1].splitlines() if '[Nova] field' in l] if len(frames) > 1 else []
check(sum('op:' in l and '1 grid(s)' in l for l in before) == 2, 'the op: density fields load in-process: %d'
      % sum('op:' in l for l in before))
check(any('op:' in l and '1 grid(s)' in l for l in after), 'and load again after the frame change')
check('Warning' not in log, 'no warnings in-process')
if FAILURES:
    print(log[-4000:])

print('%d failure(s)' % len(FAILURES))
sys.exit(1 if FAILURES else 0)
