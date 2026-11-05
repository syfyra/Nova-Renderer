# M8 research probe (houdini-volumes.md): builds the volume test scene in an
# isolated hython and saves it as a hip file plus volume files.
#
#   powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m8\probes\hv_build_scene.py <outdir> [frames] [voxelsize]
#
# Creates in <outdir>:
#   hv_scene.hip       /obj/pyro (SOP pyro sim: sphere source, density/temperature/flame),
#                      /obj/native (Houdini native volumes density + temperature),
#                      /obj/vdbsops (VDB built from SOPs, in-memory only),
#                      /stage (LOP networks, see hv_dump_stage.py)
#   pyro.vdb           the pyro sim at the last frame (VDB grids only)
#   pyro.bgeo.sc       the same frame as bgeo
#   native.bgeo.sc     the native volumes
# Nothing in the scene contains SideFX code; the Karma Pyro Material is created
# by applying Houdini's own tool recipe at run time.
import os
import sys

import hou

outdir = os.path.abspath(sys.argv[1])
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 24
divsize = float(sys.argv[3]) if len(sys.argv) > 3 else 0.04
os.makedirs(outdir, exist_ok=True)
hou.hipFile.clear(suppress_save_prompt=True)
hou.playbar.setFrameRange(1, max(frames, 2))
hou.setFrame(1)


def log(*a):
    print('[hv_build]', *a, flush=True)


# ---------------------------------------------------------------- pyro sim
obj = hou.node('/obj')
pyro = obj.createNode('geo', 'pyro')
sph = pyro.createNode('sphere', 'src_sphere')
sph.parm('type').set('poly')
sph.parmTuple('rad').set((0.25, 0.25, 0.25))
sph.parmTuple('t').set((0.0, 0.25, 0.0))
scat = pyro.createNode('scatter', 'scatter_pts')
scat.setInput(0, sph)
scat.parm('npts').set(4000)
# A volume source: points inside the sphere carrying the source attributes.
src = pyro.createNode('pyrosource', 'pyrosource1')
src.setInput(0, sph)
attr = pyro.createNode('attribwrangle', 'source_attribs')
attr.setInput(0, src)
attr.parm('snippet').set('f@density = 1.0; f@temperature = 1.0; f@burn = 1.0; v@v = set(0, 0.5, 0);')
ras = pyro.createNode('volumerasterizeattributes', 'rasterize')
ras.setInput(0, attr)
ras.parm('attributes').set('density temperature burn v')
ras.parm('voxelsize').set(divsize)
solver = pyro.createNode('pyrosolver', 'pyrosolver1')
log('pyrosolver inputs:', solver.inputLabels())
solver.setInput(0, ras)
solver.parm('divsize').set(divsize)
# Describe the source multiparm as found (defaults) and make sure density,
# temperature and flame are sourced.
nsrc_parm = None
for p in solver.parms():
    if p.parmTemplate().type() == hou.parmTemplateType.Folder and 'source' in p.name().lower():
        pass
for cand in ('source_numsources', 'sources', 'numsources', 'source_folder'):
    if solver.parm(cand) is not None:
        nsrc_parm = solver.parm(cand)
        break
if nsrc_parm is None:
    for p in solver.parms():
        pt = p.parmTemplate()
        if isinstance(pt, hou.FolderParmTemplate) and pt.folderType() in (hou.folderType.MultiparmBlock,) and 'source_volume#' in [c.name() for c in pt.parmTemplates()] + [c.name() for f in pt.parmTemplates() if isinstance(f, hou.FolderParmTemplate) for c in f.parmTemplates()]:
            nsrc_parm = p
            break
log('source multiparm:', nsrc_parm.name() if nsrc_parm else None, nsrc_parm.eval() if nsrc_parm else None)
if nsrc_parm is not None:
    if nsrc_parm.eval() < 3:
        nsrc_parm.set(3)
    for i, name in enumerate(('density', 'temperature', 'flame'), 1):
        if solver.parm('source_volume%d' % i).eval() == '':
            solver.parm('source_volume%d' % i).set(name)
            solver.parm('source_vfield%d' % i).set(name)
            solver.parm('source_voperator%d' % i).set(1)  # add
    for i in range(1, nsrc_parm.eval() + 1):
        log('  source %d: %s -> %s op %s' % (i, solver.parm('source_volume%d' % i).eval(),
                                            solver.parm('source_vfield%d' % i).eval(),
                                            solver.parm('source_voperator%d' % i).evalAsString()))
out = pyro.createNode('null', 'OUT')
out.setInput(0, solver)
out.setDisplayFlag(True)
out.setRenderFlag(True)
log('pyrosolver conv_vdb default:', solver.parm('conv_vdb').eval(), 'fp16', solver.parm('conv_usefp16').eval())
# VDB branch: the solver's output converted with Convert VDB (what a user does
# before writing .vdb files). 16-bit floats keep the file small.
cv = pyro.createNode('convertvdb', 'to_vdb')
cv.setInput(0, solver)
log('convertvdb parms:', [p.name() for p in cv.parms() if any(k in p.name() for k in ('conver', 'fp16', 'vdbclass', 'vdbtype', 'prune', 'flood', 'activate'))])
cv.parm('conversion').set('vdb')
for n, v in (('vdbclass', 'fog'), ('usefp16', 1), ('prune', 1)):
    if cv.parm(n) is not None:
        try:
            cv.parm(n).set(v)
        except Exception as e:
            log('convertvdb', n, e)
# keep only density/temperature/flame in the file (vel is not needed by the shaders)
blast = pyro.createNode('blast', 'keep_scalar')
blast.setInput(0, cv)
blast.parm('group').set('@name=vel*')
out_vdb = pyro.createNode('null', 'OUT_VDB')
out_vdb.setInput(0, blast)

for f in range(1, frames + 1):
    hou.setFrame(f)
    g = out.geometry()
    gv = out_vdb.geometry()
log('pyro OUT at frame', hou.frame(), ':', [(p.attribValue('name') if g.findPrimAttrib('name') else '?', p.type().name()) for p in g.prims()])
for gg, label in ((g, 'OUT'), (gv, 'OUT_VDB')):
    for p in gg.prims():
        if p.type() == hou.primType.VDB:
            log('  %s vdb %-12s res %s voxel %s data %s bbox %s' % (label, p.attribValue('name'), p.resolution(), p.voxelSize(), p.dataType(), p.boundingBox()))
        elif p.type() == hou.primType.Volume:
            log('  %s vol %-12s res %s bbox %s' % (label, p.attribValue('name'), p.resolution(), p.boundingBox()))
    for a in gg.primAttribs():
        log('  %s prim attrib %s %s' % (label, a.name(), a.dataType()))

# Freeze the last frame: write files and keep the in-memory nodes for SOP Import.
vdb_path = os.path.join(outdir, 'pyro.vdb').replace('\\', '/')
bgeo_path = os.path.join(outdir, 'pyro.bgeo.sc').replace('\\', '/')
gv.saveToFile(vdb_path)
g.saveToFile(bgeo_path)
log('wrote', vdb_path, os.path.getsize(vdb_path), 'bytes;', bgeo_path, os.path.getsize(bgeo_path))
lastframe = hou.frame()

# Static copy of the last frame: a File SOP reading pyro.bgeo.sc. This is what
# a cached sim looks like in a real scene (File Cache SOP -> SOP Import).
cached = pyro.createNode('file', 'cached_bgeo')
cached.parm('file').set(bgeo_path)
cached_out = pyro.createNode('null', 'CACHED')
cached_out.setInput(0, cached)

# ---------------------------------------------------------------- native volumes
nat = obj.createNode('geo', 'native')
vd = nat.createNode('volume', 'density_vol')
vd.parm('name').set('density')
vd.parmTuple('size').set((1.0, 1.0, 1.0))
vd.parm('samplediv').set(40)
vt = nat.createNode('volume', 'temperature_vol')
vt.parm('name').set('temperature')
vt.parmTuple('size').set((1.0, 1.0, 1.0))
vt.parm('samplediv').set(20)
mg = nat.createNode('merge', 'merge_vols')
mg.setInput(0, vd)
mg.setInput(1, vt)
vw = nat.createNode('volumewrangle', 'fill')
vw.setInput(0, mg)
vw.parm('snippet').set('float r = length(@P); @density = clamp(1.0 - 2.0 * r, 0, 1); @temperature = clamp(1.5 - 3.0 * r, 0, 1.5);')
nout = nat.createNode('null', 'OUT')
nout.setInput(0, vw)
nout.setDisplayFlag(True)
ng = nout.geometry()
native_path = os.path.join(outdir, 'native.bgeo.sc').replace('\\', '/')
ng.saveToFile(native_path)
log('native prims:', [(p.attribValue('name'), p.type().name(), p.resolution()) for p in ng.prims()])

# ---------------------------------------------------------------- in-memory VDB from SOPs
vs = obj.createNode('geo', 'vdbsops')
vsph = vs.createNode('sphere', 'sph')
vsph.parm('type').set('poly')
vsph.parm('freq').set(8)
v2 = vs.createNode('vdbfrompolygons', 'fog')
v2.setInput(0, vsph)
v2.parm('voxelsize').set(0.05)
try:
    v2.parm('builddistance').set(0)
    v2.parm('buildfog').set(1)
    v2.parm('fogname').set('density')
except Exception as e:
    log('vdbfrompolygons parm issue', e)
vsout = vs.createNode('null', 'OUT')
vsout.setInput(0, v2)
vsout.setDisplayFlag(True)
log('vdbsops prims:', [(p.attribValue('name'), p.type().name()) for p in vsout.geometry().prims()])

# ---------------------------------------------------------------- LOPs
stage = hou.node('/stage')
nodes = {}
v_lop = stage.createNode('volume', 'vol_vdbfile')
v_lop.parm('primpath').set('/vdbfile')
v_lop.parm('filepath1').set(vdb_path)
nodes['vdbfile'] = v_lop
b_lop = stage.createNode('volume', 'vol_bgeofile')
b_lop.parm('primpath').set('/bgeofile')
b_lop.parm('filepath1').set(native_path)
nodes['bgeofile'] = b_lop
s1 = stage.createNode('sopimport', 'sop_pyro')
s1.parm('soppath').set(out.path())
s1.parm('pathprefix').set('/sop_pyro')
nodes['sop_pyro'] = s1
s1v = stage.createNode('sopimport', 'sop_pyro_vdb')
s1v.parm('soppath').set(out_vdb.path())
s1v.parm('pathprefix').set('/sop_pyro_vdb')
nodes['sop_pyro_vdb'] = s1v
s1c = stage.createNode('sopimport', 'sop_cached')
s1c.parm('soppath').set(cached_out.path())
s1c.parm('pathprefix').set('/sop_cached')
nodes['sop_cached'] = s1c
s2 = stage.createNode('sopimport', 'sop_native')
s2.parm('soppath').set(nout.path())
s2.parm('pathprefix').set('/sop_native')
nodes['sop_native'] = s2
s3 = stage.createNode('sopimport', 'sop_vdbsops')
s3.parm('soppath').set(vsout.path())
s3.parm('pathprefix').set('/sop_vdbsops')
nodes['sop_vdbsops'] = s3
sc = stage.createNode('sopcreate', 'sopcreate_vdb')
sc.parm('pathprefix').set('/sopcreate')
inner = sc.node('sopnet/create')
if inner is None:
    log('sopcreate children:', [c.path() for c in sc.allSubChildren()][:20])
else:
    isph = inner.createNode('sphere', 'sph')
    isph.parm('type').set('poly')
    ifog = inner.createNode('vdbfrompolygons', 'fog')
    ifog.setInput(0, isph)
    ifog.parm('voxelsize').set(0.05)
    try:
        ifog.parm('builddistance').set(0)
        ifog.parm('buildfog').set(1)
        ifog.parm('fogname').set('density')
    except Exception as e:
        log('inner fog parm issue', e)
    iout = inner.createNode('output', 'output0')
    iout.setInput(0, ifog)
    iout.setDisplayFlag(True)
nodes['sopcreate'] = sc
mrg = stage.createNode('merge', 'merge_all')
for i, k in enumerate(['vdbfile', 'bgeofile', 'sop_pyro', 'sop_pyro_vdb', 'sop_cached', 'sop_native', 'sop_vdbsops', 'sopcreate']):
    mrg.setInput(i, nodes[k])

# Pyro "Create Render Stage" emulation (the UI-free parts of
# dopsparsepyrotools.stageSetup): SOP Import, Render Geometry Settings with the
# emission light-source switches, material library with the Karma Pyro Material
# recipe linked to the solver's Look parameters.
import dopsparsepyrotools as dsp
pyrostage = stage.createNode('lopnet', 'pyro_stage')
lop_import, primpath = dsp.createStageSOPImport(pyrostage, out, 'pyro')
lop_settings = dsp.createStageRenderGeometrySettings(pyrostage, emission=dsp.hasEmission(solver))
lop_settings.setInput(0, lop_import)
matlib = dsp.createStageMatLib(pyrostage, primpath)
matlib.setInput(0, lop_settings)
recipe_items = None
try:
    # hou.data.applyToolRecipe needs a network editor (hou.ui); instantiate the
    # same recipe data headless instead.
    from hrecipes import storage
    from hrecipes import rtypes
    ref = storage.recipeStorage().refForName('sidefx::recipe::vop::kma_pyromaterial', [rtypes.RecipeCategory.tool])
    res = ref.instanceInsideNode(matlib)
    items = res.get('items', {}) if isinstance(res, dict) else {}
    log('recipe result keys:', list(res.keys()) if isinstance(res, dict) else type(res))
    log('recipe items:', {k: (v.path() if hasattr(v, 'path') else v) for k, v in items.items()})
    log('matlib children:', [c.path() + ':' + c.type().name() for c in matlib.allSubChildren()])
    material = [c for c in matlib.children() if c.type().name() not in ('output',)][0]
    shader = material.node('kma_pyroshader1')
    log('material', material.path(), material.type().name(), 'shader', shader.path() if shader else None)
    matlib.parm('matnode1').set(material.name())
    # The "Create Render Stage" code links the shader to the solver's Look
    # parameters; in 22.0.466 this raises "Invalid parameter name" (logged), so
    # the material keeps the recipe defaults.
    try:
        dsp.linkMaterialToSolver(solver, shader)
    except Exception as e:
        log('linkMaterialToSolver failed:', repr(e))
except Exception as e:
    import traceback
    traceback.print_exc()
pout = pyrostage.createNode('null', 'OUT')
pout.setInput(0, matlib)
pout.setDisplayFlag(True)
pyrostage_out = pout
log('pyro stage primpath', primpath, 'hasEmission', dsp.hasEmission(solver))

hip = os.path.join(outdir, 'hv_scene.hip').replace('\\', '/')
hou.hipFile.save(hip)
log('saved', hip, 'lastframe', lastframe)
