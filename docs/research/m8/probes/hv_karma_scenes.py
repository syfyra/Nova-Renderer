# M8 research probe (houdini-volumes.md): builds the Karma reference scenes for
# the volume conventions in an isolated hython. Nothing generated here is
# committed: the Karma Pyro Material is instantiated from Houdini's own tool
# recipe at run time and saved into <outdir>.
#
#   powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script docs\research\m8\probes\hv_karma_scenes.py <outdir> <hv_pyro.vdb>
#
# Writes to <outdir>:
#   box_d1.vdb          unit cube [-0.5,0.5]^3, voxel 0.05: density 1, temperature 1, flame 1
#   box_e_t<T>.vdb      the same cube with density 0.001 and temperature T (0.5, 1, 2), flame 1
#   box_bb_<T>.vdb      blackbody sweep cubes (density 0.001, temperature TB[i])
#   box_nod.vdb         temperature + flame only (no density field)
#   pyromat.usda        /materials/karmapyromaterial1 (recipe sidefx::recipe::vop::kma_pyromaterial)
#   mtlxvol.usda, mtlxbb.usda  hand-written MaterialX volume materials (ND_volume)
#   common.usda         render products/vars, camera-independent Karma settings class
#   <scene>.usda        one file per measurement, rendered by hv_karma_renders.ps1
# and prints the interface of the pyro shader node graph (input name, type, value).
import json
import os
import sys

import hou
from pxr import Sdf, Usd, UsdShade

outdir = os.path.abspath(sys.argv[1]).replace('\\', '/')
pyro_vdb = os.path.abspath(sys.argv[2]).replace('\\', '/')
os.makedirs(outdir, exist_ok=True)
hou.hipFile.clear(suppress_save_prompt=True)


def log(*a):
    print('[hv_karma]', *a, flush=True)


# ---------------------------------------------------------------- VDB boxes
def write_box(path, values):
    geo = hou.node('/obj').createNode('geo', 'box_' + os.path.basename(path).split('.')[0])
    vols = []
    for name in values:
        v = geo.createNode('volume', 'vol_' + name)
        v.parm('name').set(name)
        v.parmTuple('size').set((1.0, 1.0, 1.0))
        v.parm('samplediv').set(20)
        vols.append(v)
    mg = geo.createNode('merge')
    for i, v in enumerate(vols):
        mg.setInput(i, v)
    vw = geo.createNode('volumewrangle')
    vw.setInput(0, mg)
    vw.parm('snippet').set(' '.join('@%s = %r;' % (k, float(x)) for k, x in values.items()))
    cv = geo.createNode('convertvdb')
    cv.setInput(0, vw)
    cv.parm('conversion').set('vdb')
    g = cv.geometry()
    g.saveToFile(path)
    log('wrote', path, [(p.attribValue('name'), p.type().name(), p.resolution()) for p in g.prims()])


write_box(outdir + '/box_d1.vdb', {'density': 1.0, 'temperature': 1.0, 'flame': 1.0})
for t in (0.5, 1.0, 2.0):
    write_box(outdir + '/box_e_t%s.vdb' % str(t).replace('.', 'p'), {'density': 0.001, 'temperature': t, 'flame': 1.0})

# ---------------------------------------------------------------- pyro material
stage = hou.node('/stage')
matlib = stage.createNode('materiallibrary', 'matlib')
from hrecipes import rtypes, storage  # noqa: E402

ref = storage.recipeStorage().refForName('sidefx::recipe::vop::kma_pyromaterial', [rtypes.RecipeCategory.tool])
ref.instanceInsideNode(matlib)
material = [c for c in matlib.children() if c.type().name() != 'output'][0]
matlib.parm('matnode1').set(material.name())
matlib.parm('matpath1').set('/materials/' + material.name())
rop = stage.createNode('usd_rop', 'save_mat')
rop.parm('loppath').set(matlib.path())
rop.parm('lopoutput').set(outdir + '/pyromat.usda')
rop.parm('trange').set('off')
rop.render()
log('saved pyromat.usda, errors', rop.errors())
st = Usd.Stage.Open(outdir + '/pyromat.usda')
mat_path = None
for prim in st.Traverse():
    if prim.IsA(UsdShade.Material):
        mat_path = str(prim.GetPath())
        m = UsdShade.Material(prim)
        log('material', mat_path, 'outputs', [o.GetFullName() for o in m.GetOutputs()], 'interface', [i.GetFullName() for i in m.GetInputs()])
ng = st.GetPrimAtPath(mat_path + '/kma_pyroshader1')
iface = {}
for a in UsdShade.NodeGraph(ng).GetInputs():
    srcs = a.GetConnectedSources()[0]
    iface[a.GetBaseName()] = ('<- ' + ','.join(str(s.source.GetPath()).replace(mat_path, '') + '.' + s.sourceName for s in srcs)) if srcs \
        else '%s = %r' % (a.GetTypeName(), a.Get())
for k in sorted(iface):
    log('  pyro input %-28s %s' % (k, iface[k]))
for prim in Usd.PrimRange(st.GetPrimAtPath(mat_path)):
    sh = UsdShade.Shader(prim)
    if prim.IsA(UsdShade.Shader) and str(sh.GetShaderId() or '') == 'ND_geompropvalue_float':
        log('  geomprop', str(prim.GetPath()).replace(mat_path, ''), sh.GetInput('geomprop').Get())

# ---------------------------------------------------------------- USD scenes
COMMON = '''#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

class "_class_Settings"
{
    rel products = [</Render/Products/beauty>]
}

class "_class_Calib"
{
    token karma:global:convergence_mode = "Path Traced"
    string karma:global:pixeloracle = '["uniform",{}]'
    int karma:global:pathtracedsamples = 64
    string karma:global:pixelfilter = "box"
    float karma:global:pixelfilterscale = 1
    float karma:global:colorlimit = 1e9
    float karma:global:colorlimitindirect = 1e9
    rel products = [</Render/Products/beauty>]
}

def Scope "Render"
{
    def Scope "Products"
    {
        def RenderProduct "beauty"
        {
            token productName = "beauty.exr"
            rel orderedVars = [</Render/Vars/C>]
        }
    }

    def Scope "Vars"
    {
        def RenderVar "C"
        {
            uniform token dataType = "color4f"
            uniform string sourceName = "C.*[LO]"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "color4f"
            string driver:parameters:aov:name = "C"
        }
    }
}
'''
with open(outdir + '/common.usda', 'w', encoding='utf-8', newline='\n') as f:
    f.write(COMMON)

MTLX_VOL = '''#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

def Scope "materials"
{
    def Material "mtlxvol"
    {
        token outputs:mtlx:volume.connect = </materials/mtlxvol/volume.outputs:out>

        def Shader "volume"
        {
            uniform token info:id = "ND_volume"
            token inputs:vdf.connect = </materials/mtlxvol/vdf.outputs:out>
            token inputs:edf.connect = </materials/mtlxvol/edf.outputs:out>
            token outputs:out
        }

        def Shader "vdf"
        {
            uniform token info:id = "ND_anisotropic_vdf"
            vector3f inputs:absorption = (0, 0, 0)
            vector3f inputs:scattering.connect = </materials/mtlxvol/density3.outputs:out>
            float inputs:anisotropy = 0
            token outputs:out
        }

        def Shader "edf"
        {
            uniform token info:id = "ND_uniform_edf"
            color3f inputs:color = (0, 0, 0)
            token outputs:out
        }

        def Shader "density"
        {
            uniform token info:id = "ND_geompropvalue_float"
            string inputs:geomprop = "density"
            float inputs:default = 0
            float outputs:out
        }

        def Shader "density3"
        {
            uniform token info:id = "ND_convert_float_vector3"
            float inputs:in.connect = </materials/mtlxvol/density.outputs:out>
            vector3f outputs:out
        }
    }
}
'''
with open(outdir + '/mtlxvol.usda', 'w', encoding='utf-8', newline='\n') as f:
    f.write(MTLX_VOL)


def volume_prim(name, vdb, fields, material=None, translate=(0, 0, 0), extra=''):
    rels = '\n'.join('        rel field:%s = </World/%s/%s>' % (fl, name, fl) for fl in fields)
    kids = '\n'.join('''        def OpenVDBAsset "%s"
        {
            asset filePath = @%s@
            token fieldName = "%s"
        }''' % (fl, vdb, fl) for fl in fields)
    mat = '        rel material:binding = <%s>\n' % material if material else ''
    mat += extra
    return '''    def Volume "%s" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
%s
%s        double3 xformOp:translate = (%g, %g, %g)
        uniform token[] xformOpOrder = ["xformOp:translate"]
%s
    }
''' % (name, rels, mat, translate[0], translate[1], translate[2], kids)


def scene(name, body, settings, cls='_class_Settings', res=(200, 300), sublayers=()):
    subs = ', '.join('@./%s@' % s for s in ('common.usda',) + tuple(sublayers))
    attrs = '\n'.join('    %s' % s for s in settings)
    txt = '''#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
    renderSettingsPrimPath = "/Render/settings"
    subLayers = [%s]
)

def Xform "World"
{
%s}

over "Render"
{
    def RenderSettings "settings" (
        inherits = <%s>
    )
    {
        rel camera = </World/cam>
        int2 resolution = (%d, %d)
%s
    }
}
''' % (subs, body, '/' + cls, res[0], res[1], attrs)
    with open('%s/%s.usda' % (outdir, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write(txt)
    return name


PERSP_CAM = '''    def Camera "cam"
    {
        float focalLength = 37.1
        float horizontalAperture = 13.97
        float verticalAperture = 20.955
        double3 xformOp:translate = (0, 2, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
'''
ORTHO_CAM = '''    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 40
        float verticalAperture = 40
        double3 xformOp:translate = (%g, 0, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
'''
# normalize = 1 as Houdini's Distant Light LOP authors it: the intensity is
# the irradiance (un-normalized, it would be the radiance of a 0.53 degree
# disk, ~2e-4 of that irradiance).
KEY_LIGHT = '''    def DistantLight "key"
    {
        float inputs:intensity = 3
        bool inputs:normalize = 1
        float3 xformOp:rotateXYZ = (-30, 50, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
'''
TOP_LIGHT = '''    def DistantLight "top"
    {
        float inputs:intensity = 1
        bool inputs:normalize = 1
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
'''
DOME = '''    def DomeLight "dome"
    {
        float inputs:intensity = 1
    }
'''
PYRO_FIELDS = ('density', 'temperature', 'flame')
MAT = mat_path
scenes = []


def pyro_overrides(d):
    lines = '\n'.join('            %s inputs:%s = %s' % (t, k, v) for k, (t, v) in d.items())
    return '''over "materials"
{
    over "%s"
    {
        over "kma_pyroshader1"
        {
%s
        }
    }
}
''' % (os.path.basename(MAT), lines)


def with_overrides(name, overrides):
    if not overrides:
        return ()
    fn = name + '_ovr.usda'
    with open(outdir + '/' + fn, 'w', encoding='utf-8', newline='\n') as f:
        f.write('#usda 1.0\n\n' + pyro_overrides(overrides))
    return (fn,)


# Pyro sim (hv_pyro.vdb) under a key light, perspective camera, Karma defaults.
pyro_body = PERSP_CAM + KEY_LIGHT
for nm, mat, ovr, st in (
        ('pyro_bare', None, {}, []),
        ('pyro_bare_vl4', None, {}, ['float karma:object:volumelimit = 4']),
        ('pyro_mat', MAT, {}, []),
        ('pyro_mat_vl4', MAT, {}, ['float karma:object:volumelimit = 4']),
        ('pyro_fire', MAT, {'enablefire': ('int', 1)}, []),
        ('pyro_fire_bb', MAT, {'enablefire': ('int', 1), 'firecolormode': ('int', 1)}, []),
        ('pyro_fire_vl4', MAT, {'enablefire': ('int', 1)}, ['float karma:object:volumelimit = 4']),
        ('pyro_fire_sr1', MAT, {'enablefire': ('int', 1)}, ['float karma:object:volumesteprate = 1']),
        ('pyro_mtlx', '/materials/mtlxvol', {}, []),
):
    subs = with_overrides(nm, ovr) + (('pyromat.usda',) if mat == MAT else ()) + (('mtlxvol.usda',) if mat == '/materials/mtlxvol' else ())
    scenes.append(scene(nm, pyro_body + volume_prim('pyro', pyro_vdb, PYRO_FIELDS, mat, (0, 0, 0)), st, sublayers=subs))

# Calibration: unit cube of density 1 seen through exactly one unit of depth
# by an orthographic camera (aperture 40 = 4 units wide); three cubes at
# x = -1.5, 0, 1.5 with no material, the pyro material and the MaterialX volume.
cal_body = (ORTHO_CAM % 0) + \
    volume_prim('bare', outdir + '/box_d1.vdb', PYRO_FIELDS, None, (-1.5, 0, 0)) + \
    volume_prim('pyro', outdir + '/box_d1.vdb', PYRO_FIELDS, MAT, (0, 0, 0)) + \
    volume_prim('mtlx', outdir + '/box_d1.vdb', ('density',), '/materials/mtlxvol', (1.5, 0, 0))
CAL = '_class_Calib'
scenes.append(scene('cal_alpha', cal_body, [], CAL, (80, 80), ('pyromat.usda', 'mtlxvol.usda')))
scenes.append(scene('cal_furnace_vl0', cal_body + DOME, [], CAL, (80, 80), ('pyromat.usda', 'mtlxvol.usda')))
scenes.append(scene('cal_furnace_vl64', cal_body + DOME, ['float karma:object:volumelimit = 64'], CAL, (80, 80), ('pyromat.usda', 'mtlxvol.usda')))
# Emission: three thin (density 0.001) cubes with temperature 0.5, 1, 2 at
# x = -1.5, 0, 1.5, pyro material with fire on (ramp colour / blackbody), no lights.
emit_body = (ORTHO_CAM % 0) + ''.join(
    volume_prim('t%s' % t.replace('.', 'p'), outdir + '/box_e_t%s.vdb' % t.replace('.', 'p'), PYRO_FIELDS, MAT, (x, 0, 0))
    for t, x in (('0.5', -1.5), ('1.0', 0.0), ('2.0', 1.5)))
for nm, ovr in (('cal_emit_ramp', {'enablefire': ('int', 1)}),
                ('cal_emit_bb', {'enablefire': ('int', 1), 'firecolormode': ('int', 1)}),
                ('cal_emit_smokeoff', {'enablefire': ('int', 1), 'enablesmoke': ('int', 0)})):
    scenes.append(scene(nm, emit_body, [], CAL, (80, 80), with_overrides(nm, ovr) + ('pyromat.usda',)))
# Same thin cubes without a material: does the default volume shader emit?
bare_emit = (ORTHO_CAM % 0) + ''.join(
    volume_prim('t%s' % t.replace('.', 'p'), outdir + '/box_e_t%s.vdb' % t.replace('.', 'p'), PYRO_FIELDS, None, (x, 0, 0))
    for t, x in (('0.5', -1.5), ('1.0', 0.0), ('2.0', 1.5)))
scenes.append(scene('cal_emit_bare', bare_emit, [], CAL, (80, 80)))

# Single scattering from a normalized distant light (irradiance 1) shining
# straight down onto the three density-1 cubes, isotropic and with HG g = 0.5
# (pyro anisotropy input and anisotropic_vdf anisotropy).
scenes.append(scene('cal_distant', cal_body + TOP_LIGHT, [], CAL, (80, 80), ('pyromat.usda', 'mtlxvol.usda')))
# (one 'over "materials"' only: a second root spec of the same name in one
# layer makes husk drop the layer silently)
with open(outdir + '/cal_distant_g05_ovr.usda', 'w', encoding='utf-8', newline='\n') as f:
    f.write('#usda 1.0\n\n' + pyro_overrides({'anisotropy': ('float', 0.5)}).rstrip().rstrip('}') +
            '    over "mtlxvol"\n    {\n        over "vdf"\n        {\n'
            '            float inputs:anisotropy = 0.5\n        }\n    }\n}\n')
scenes.append(scene('cal_distant_g05', cal_body + TOP_LIGHT, [], CAL, (80, 80),
                    ('cal_distant_g05_ovr.usda', 'pyromat.usda', 'mtlxvol.usda')))

# Blackbody sweep: eight thin cubes (density 0.001) with temperature field
# values TB[i] at x = -5.25 + 1.5 i, seen by a 12 x 2 unit orthographic camera.
# 'mtlxbb' emits uniform_edf(blackbody(temperature * 2500)) per unit length;
# the pyro variant uses the Karma Pyro Material in blackbody mode with the
# intensity range shrunk to (0, 0.01) so the intensity is 1 everywhere.
TB = (0.32, 0.4, 0.6, 0.8, 1.0, 1.6, 2.4, 4.0)
for t in TB:
    write_box(outdir + '/box_bb_%s.vdb' % str(t).replace('.', 'p'), {'density': 0.001, 'temperature': t, 'flame': 1.0})
MTLX_BB = '''#usda 1.0

def Scope "materials"
{
    def Material "mtlxbb"
    {
        token outputs:mtlx:volume.connect = </materials/mtlxbb/volume.outputs:out>

        def Shader "volume"
        {
            uniform token info:id = "ND_volume"
            token inputs:vdf.connect = </materials/mtlxbb/vdf.outputs:out>
            token inputs:edf.connect = </materials/mtlxbb/edf.outputs:out>
            token outputs:out
        }

        def Shader "vdf"
        {
            uniform token info:id = "ND_anisotropic_vdf"
            vector3f inputs:scattering.connect = </materials/mtlxbb/density3.outputs:out>
            token outputs:out
        }

        def Shader "edf"
        {
            uniform token info:id = "ND_uniform_edf"
            color3f inputs:color.connect = </materials/mtlxbb/bb.outputs:out>
            token outputs:out
        }

        def Shader "bb"
        {
            uniform token info:id = "ND_blackbody"
            float inputs:temperature.connect = </materials/mtlxbb/kelvin.outputs:out>
            color3f outputs:out
        }

        def Shader "kelvin"
        {
            uniform token info:id = "ND_multiply_float"
            float inputs:in1.connect = </materials/mtlxbb/temp.outputs:out>
            float inputs:in2 = 2500
            float outputs:out
        }

        def Shader "temp"
        {
            uniform token info:id = "ND_geompropvalue_float"
            string inputs:geomprop = "temperature"
            float outputs:out
        }

        def Shader "density"
        {
            uniform token info:id = "ND_geompropvalue_float"
            string inputs:geomprop = "density"
            float outputs:out
        }

        def Shader "density3"
        {
            uniform token info:id = "ND_convert_float_vector3"
            float inputs:in.connect = </materials/mtlxbb/density.outputs:out>
            vector3f outputs:out
        }
    }
}
'''
with open(outdir + '/mtlxbb.usda', 'w', encoding='utf-8', newline='\n') as f:
    f.write(MTLX_BB)
SWEEP_CAM = '''    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 120
        float verticalAperture = 20
        double3 xformOp:translate = (0, 0, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
'''


def sweep_body(mat):
    return SWEEP_CAM + ''.join(
        volume_prim('bb%d' % i, outdir + '/box_bb_%s.vdb' % str(t).replace('.', 'p'), PYRO_FIELDS, mat, (-5.25 + 1.5 * i, 0, 0))
        for i, t in enumerate(TB))


scenes.append(scene('cal_bb_mtlx', sweep_body('/materials/mtlxbb'), [], CAL, (240, 40), ('mtlxbb.usda',)))
scenes.append(scene('cal_bb_pyro', sweep_body(MAT), [], CAL, (240, 40),
                    with_overrides('cal_bb_pyro', {'enablefire': ('int', 1), 'firecolormode': ('int', 1),
                                                   'fireintrange': ('float2', '(0, 0.01)')}) + ('pyromat.usda',)))
with open(outdir + '/sweep.json', 'w') as f:
    json.dump(list(TB), f)

# Emission without a density field: Karma's empty-space culling uses the
# 'Volume Sampling Field' (karma:object:volumesamplingchannel, default density).
# left: temperature + flame only; mid: the same with the sampling field set to
# temperature; right: the density-0.001 cube for reference. Pyro fire on.
write_box(outdir + '/box_nod.vdb', {'temperature': 1.0, 'flame': 1.0})
nod_body = (ORTHO_CAM % 0) + \
    volume_prim('nod', outdir + '/box_nod.vdb', ('temperature', 'flame'), MAT, (-1.5, 0, 0)) + \
    volume_prim('nod_tc', outdir + '/box_nod.vdb', ('temperature', 'flame'), MAT, (0, 0, 0),
                '        string primvars:karma:object:volumesamplingchannel = "temperature" (\n            interpolation = "constant"\n        )\n') + \
    volume_prim('ref', outdir + '/box_e_t1p0.vdb', PYRO_FIELDS, MAT, (1.5, 0, 0))
scenes.append(scene('cal_emit_nodensity', nod_body, [], CAL, (80, 80),
                    with_overrides('cal_emit_nodensity', {'enablefire': ('int', 1)}) + ('pyromat.usda',)))
# Step rate on the emission cubes (ramp colour): 0.25 (default) vs 1 vs 0.05
for nm, sr in (('cal_emit_sr1', 1.0), ('cal_emit_sr005', 0.05)):
    scenes.append(scene(nm, emit_body, ['float karma:object:volumesteprate = %g' % sr], CAL, (80, 80),
                        with_overrides(nm, {'enablefire': ('int', 1)}) + ('pyromat.usda',)))
with open(outdir + '/scenes.json', 'w') as f:
    json.dump(scenes, f)
log('scenes', scenes)
