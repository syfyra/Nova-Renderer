"""Volumes against Karma (docs/design/M8.md M8.2, M8.3; ctest
houdini_volume_karma), generated at run time in an isolated hython: the
calibration cubes of docs/research/m8/houdini-volumes.md 4.2 and 4.7,
rendered by Karma CPU and by Nova through husk, compared cube by cube:

  * alpha through one unit of density 1 (extinction = density) for a bare
    volume, the Karma Pyro Material's defaults and a MaterialX volume
    (ND_volume(ND_anisotropic_vdf(scattering = float density as vector3)),
    as in the research probes: Karma reads nothing through a vector3
    geompropvalue of a float field);
  * emission of thin cubes (density 0.001) with temperature 0.5, 1 and 2
    through the Karma Pyro Material with fire on, in ramp and blackbody
    colour modes (emission per unit length, trilinear fields, Karma's
    blackbody);
  * scattering: a furnace (dome of radiance 1, not seen by the camera) at
    volume limits 0 and 64, a distant light from above (single scattering),
    kma_volume's primary and secondary HG lobes, and its extinction and
    contribution modifiers at volume limits 0, 1 and 64;
  * the research's pyro sim (tests/data/volumes/pyro.vdb) with the Karma
    Pyro Material under a key light, volume limits 0 and 4: image means.

The Karma Pyro Material comes from Houdini's recipe at run time and is never
committed. Usage: hython test_volume_karma.py <outdir>
"""

import os
import subprocess
import sys

import hou

OUT = os.path.abspath(sys.argv[1]).replace('\\', '/')
os.makedirs(OUT, exist_ok=True)
HFS = os.environ['HFS']
FAILURES = []


def check(cond, message):
    print(('ok   ' if cond else 'FAIL ') + message, flush=True)
    if not cond:
        FAILURES.append(message)


def write_box(name, values):
    """A unit cube of constant fields, 20 voxels per unit, centred at 0."""
    geo = hou.node('/obj').createNode('geo', name)
    vols = []
    for field in values:
        v = geo.createNode('volume', 'vol_' + field)
        v.parm('name').set(field)
        v.parmTuple('size').set((1.0, 1.0, 1.0))
        v.parm('samplediv').set(20)
        vols.append(v)
    merge = geo.createNode('merge')
    for i, v in enumerate(vols):
        merge.setInput(i, v)
    wrangle = geo.createNode('volumewrangle')
    wrangle.setInput(0, merge)
    wrangle.parm('snippet').set(' '.join('@%s = %r;' % (k, float(x)) for k, x in values.items()))
    vdb = geo.createNode('convertvdb')
    vdb.setInput(0, wrangle)
    vdb.parm('conversion').set('vdb')
    path = '%s/%s.vdb' % (OUT, name)
    vdb.geometry().saveToFile(path)
    return path


boxes = {'d1': write_box('box_d1', {'density': 1.0, 'temperature': 1.0, 'flame': 1.0})}
for t in ('0.5', '1.0', '2.0'):
    boxes[t] = write_box('box_t' + t.replace('.', 'p'), {'density': 0.001, 'temperature': float(t), 'flame': 1.0})

# The Karma Pyro Material from Houdini's recipe, exported per variant.
stage = hou.node('/stage')
matlib = stage.createNode('materiallibrary', 'materials')
from hrecipes import rtypes, storage  # noqa: E402  (Houdini's recipe API, headless)
storage.recipeStorage().refForName('sidefx::recipe::vop::kma_pyromaterial', [rtypes.RecipeCategory.tool]) \
    .instanceInsideNode(matlib)
material = [c for c in matlib.children() if c.type().name() != 'output'][0]
shader = material.node('kma_pyroshader1')
matlib.parm('matnode1').set(material.name())
PYRO = '/materials/' + material.name()
variants = {'default': {}, 'ramp': {'enablefire': 1}, 'blackbody': {'enablefire': 1, 'firecolormode': 1}}
for name, parms in variants.items():
    for p in ('enablefire', 'firecolormode'):
        shader.parm(p).revertToDefaults()
    for p, v in parms.items():
        shader.parm(p).set(v)
    matlib.stage().Export('%s/pyro_%s.usda' % (OUT, name))

DENSITY = '''
        def Shader "density"
        {{
            uniform token info:id = "ND_geompropvalue_float"
            string inputs:geomprop = "density"
            float outputs:out
        }}
        def Shader "density3"
        {{
            uniform token info:id = "ND_convert_float_{type}"
            float inputs:in.connect = </m/{name}/density.outputs:out>
            {usd} outputs:out
        }}'''

MTLX = '''
    def Material "mtlxvol"
    {
        token outputs:mtlx:volume.connect = </m/mtlxvol/vol.outputs:out>
        def Shader "vol"
        {
            uniform token info:id = "ND_volume"
            token inputs:vdf.connect = </m/mtlxvol/vdf.outputs:out>
            token outputs:out
        }
        def Shader "vdf"
        {
            uniform token info:id = "ND_anisotropic_vdf"
            vector3f inputs:scattering.connect = </m/mtlxvol/density3.outputs:out>
            token outputs:out
        }%s
    }''' % DENSITY.format(name='mtlxvol', type='vector3', usd='vector3f')


def kma(name, g=0.0, g2=0.0, mix=0.0, ext=0.0, contrib=1.0, start=1):
    """A kma_volume material: scattering = density."""
    return '''
    def Material "%s"
    {
        token outputs:kma:volume.connect = </m/%s/vol.outputs:out>
        def Shader "vol"
        {
            uniform token info:id = "kma_volume"
            color3f inputs:scattering.connect = </m/%s/density3.outputs:out>
            float inputs:anisotropy = %g
            float inputs:secondaryanisotropy = %g
            float inputs:secondarymix = %g
            float inputs:extinctionmodifier = %g
            float inputs:contributionmodifier = %g
            int inputs:modifierstartlevel = %d
            token outputs:out
        }%s
    }''' % (name, name, name, g, g2, mix, ext, contrib, start,
            DENSITY.format(name=name, type='color3', usd='color3f'))


MATERIALS = MTLX + ''.join(kma(*a) for a in (
    ('g05', 0.5), ('g05_mix025', 0.5, 0.0, 0.25), ('g0_g2_05_mix025', 0.0, 0.5, 0.25),
    ('e05_s0', 0.0, 0.0, 0.0, 0.5, 1.0, 0), ('e05_s1', 0.0, 0.0, 0.0, 0.5, 1.0, 1),
    ('c05_s0', 0.0, 0.0, 0.0, 0.0, 0.5, 0), ('c05_s1', 0.0, 0.0, 0.0, 0.0, 0.5, 1)))

TOP = '''
    def DistantLight "top"
    {
        float inputs:intensity = 1
        bool inputs:normalize = 1
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }'''
DOME = '''
    def DomeLight "dome"
    {
        float inputs:intensity = 1
    }'''


def volume(name, vdb, x, material):
    bind = ('\n        rel material:binding = <%s>' % material) if material else ''
    fields = ''.join('''
        def OpenVDBAsset "%s"
        {
            asset filePath = @%s@
            token fieldName = "%s"
        }''' % (f, vdb, f) for f in ('density', 'temperature', 'flame'))
    return '''
    def Volume "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        rel field:density = </World/%s/density>
        rel field:temperature = </World/%s/temperature>
        rel field:flame = </World/%s/flame>
        double3 xformOp:translate = (%g, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]%s%s
    }''' % (name, name, name, name, x, bind, fields)


def scene(name, cubes, pyro_variant, lights, limit, samples):
    body = ''.join(volume(n, vdb, x, m) for n, vdb, x, m in cubes)
    text = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    subLayers = [@pyro_%s.usda@]
)
def Xform "World"
{%s%s
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 50
        float verticalAperture = 10
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
def Scope "m"
{%s
}
def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        int2 resolution = (100, 20)
        rel products = [</Render/Products/p>]
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1e-06
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = %d
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
        float karma:object:volumelimit = %d
        int nova:samples = %d
        float nova:clampIndirect = 0
    }
    def Scope "Products"
    {
        def RenderProduct "p"
        {
            token productName = "out.exr"
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
''' % (pyro_variant, body, lights, MATERIALS, samples, limit, samples)
    path = '%s/%s.usda' % (OUT, name)
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    return path


def render(renderer, usd, exr):
    run = subprocess.run([os.path.join(HFS, 'bin', 'husk.exe'), '--renderer', renderer, '--headlight', 'none',
                          '-s', '/Render/s', '-o', exr, usd], capture_output=True, text=True, cwd=OUT, timeout=900)
    return run.returncode == 0 and os.path.exists(exr), run.stdout + run.stderr


def cube_means(exr):
    """Mean RGBA over the middle 10 x 10 pixels of each cube (x = -1.5, 0, 1.5)."""
    import OpenImageIO as oiio
    px = oiio.ImageBuf(exr).get_pixels(oiio.FLOAT)
    out = []
    for cx in (20, 50, 80):
        region = px[5:15, cx - 5:cx + 5]
        out.append([float(region[..., c].mean()) for c in range(4)])
    return out


D1 = boxes['d1']
TRIPLE = [('bare', D1, -1.5, None), ('pyro', D1, 0.0, PYRO), ('mtlx', D1, 1.5, '/m/mtlxvol')]
LOBES = [('g05', D1, -1.5, '/m/g05'), ('mix', D1, 0.0, '/m/g05_mix025'), ('mix2', D1, 1.5, '/m/g0_g2_05_mix025')]
MODS = [('e05_s0', D1, -1.5, '/m/e05_s0'), ('e05_s1', D1, 0.0, '/m/e05_s1'), ('c05_s0', D1, 1.5, '/m/c05_s0')]
FIRE = [('t05', boxes['0.5'], -1.5, PYRO), ('t10', boxes['1.0'], 0.0, PYRO), ('t20', boxes['2.0'], 1.5, PYRO)]
# name, cubes, pyro variant, lights, volume limit, samples, compared channels, tolerance
# Karma's single scattering is about 1.3 % above the analytic value (research
# H 4.2), Nova's matches it: the lit cases allow 3 %.
cases = [
    ('alpha', TRIPLE, 'default', '', 0, 256, [3], 0.02),
    ('emit_ramp', FIRE, 'ramp', '', 0, 64, [0, 1, 2], 0.02),
    ('emit_blackbody', FIRE, 'blackbody', '', 0, 64, [0, 1, 2], 0.02),
    ('furnace_vl0', TRIPLE, 'default', DOME, 0, 256, [0], 0.03),
    ('furnace_vl64', TRIPLE, 'default', DOME, 64, 256, [0], 0.03),
    ('distant', TRIPLE, 'default', TOP, 0, 256, [0], 0.03),
    ('lobes', LOBES, 'default', TOP, 0, 256, [0], 0.03),
    ('mod_vl0', MODS, 'default', DOME, 0, 256, [0], 0.03),
    ('mod_vl1', MODS, 'default', DOME, 1, 256, [0], 0.03),
    ('mod_vl64', MODS, 'default', DOME, 64, 256, [0], 0.03),
]
only = sys.argv[2].split(',') if len(sys.argv) > 2 else None
for name, cubes, variant, lights, limit, samples, channels, tolerance in cases:
    if only and name not in only:
        continue
    usd = scene(name, cubes, variant, lights, limit, samples)
    results = {}
    for renderer, tag in (('BRAY_HdKarma', 'karma'), ('Nova', 'nova')):
        exr = '%s/%s_%s.exr' % (OUT, name, tag)
        ok, log = render(renderer, usd, exr)
        check(ok, '%s: %s renders' % (name, renderer))
        if tag == 'nova':
            check('Warning' not in log, '%s: Nova prints no warnings' % name)
            if 'Warning' in log:
                print(log[-2000:])
        results[tag] = cube_means(exr) if ok else None
    if not results['karma'] or not results['nova']:
        continue
    for i, cube in enumerate(cubes):
        k, n = results['karma'][i], results['nova'][i]
        worst = max(abs(n[c] - k[c]) / max(abs(k[c]), 1e-3) for c in channels)
        check(worst < tolerance, '%s %s: Nova %s vs Karma %s (max rel. deviation %.4f)'
              % (name, cube[0], [round(n[c], 5) for c in channels], [round(k[c], 5) for c in channels], worst))

# The research's pyro sim with the Karma Pyro Material (its defaults equal the
# bare volume in Karma, research H 4.5; pyro_bare and pyro_bare_vl4 are
# reference scenes): the image means.
PYRO_VDB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'volumes', 'pyro.vdb')
PYRO_VDB = os.path.abspath(PYRO_VDB).replace('\\', '/')


def pyro_scene(name, limit, samples, variant='default'):
    fields = ''.join('''
        def OpenVDBAsset "%s"
        {
            asset filePath = @%s@
            token fieldName = "%s"
        }''' % (f, PYRO_VDB, f) for f in ('density', 'temperature', 'flame'))
    text = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    subLayers = [@pyro_%s.usda@]
)
def Xform "World"
{
    def Volume "pyro" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        rel field:density = </World/pyro/density>
        rel field:temperature = </World/pyro/temperature>
        rel field:flame = </World/pyro/flame>
        rel material:binding = <%s>%s
    }
    def DistantLight "key"
    {
        float inputs:intensity = 3
        bool inputs:normalize = 1
        float3 xformOp:rotateXYZ = (-30, 50, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
    def Camera "cam"
    {
        float focalLength = 37.1
        float horizontalAperture = 13.97
        float verticalAperture = 20.955
        double3 xformOp:translate = (0, 2, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        int2 resolution = (100, 150)
        rel products = [</Render/Products/p>]
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1e-06
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = %d
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
        float karma:object:volumelimit = %d
        float karma:object:volumesteprate = 4
        int nova:samples = %d
        float nova:clampIndirect = 0
    }
    def Scope "Products"
    {
        def RenderProduct "p"
        {
            token productName = "out.exr"
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
''' % (variant, PYRO, fields, samples, limit, samples)
    path = '%s/%s.usda' % (OUT, name)
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    return path


for name, limit, variant in (('pyro_mat', 0, 'default'), ('pyro_mat_vl4', 4, 'default'), ('pyro_fire', 0, 'ramp'),
                             ('pyro_fire_bb', 0, 'blackbody'), ('pyro_fire_vl4', 4, 'ramp')):
    if only and name not in only:
        continue
    usd = pyro_scene(name, limit, 128, variant)
    means = {}
    for renderer, tag in (('BRAY_HdKarma', 'karma'), ('Nova', 'nova')):
        exr = '%s/%s_%s.exr' % (OUT, name, tag)
        ok, log = render(renderer, usd, exr)
        check(ok, '%s: %s renders' % (name, renderer))
        if tag == 'nova':
            check('Warning' not in log, '%s: Nova prints no warnings' % name)
        if ok:
            import OpenImageIO as oiio
            px = oiio.ImageBuf(exr).get_pixels(oiio.FLOAT)
            means[tag] = [float(px[..., c].mean()) for c in range(4)]
    if len(means) == 2:
        k, n = means['karma'], means['nova']
        worst = max(abs(n[c] - k[c]) / max(abs(k[c]), 1e-4) for c in range(4))
        check(worst < 0.03, '%s: Nova %s vs Karma %s (image means RGBA, max rel. deviation %.4f)'
              % (name, [round(v, 5) for v in n], [round(v, 5) for v in k], worst))

# An emissive volume as a light source (research H 4.8): a fire cube
# (absorption = density, emission (2, 1, 0.5) * density) and a white smoke
# cube on a grey ground, seen from above; the ground between them, the smoke
# and the fire, with treat_as_lightsource 0 / 1 and diffuse scale 2. Karma
# marches emission with a step rate; at its default 0.25 (5 steps through
# the cube) an absorbing emitter comes out 10 % bright, so Karma renders
# these at step rate 4.
FIRE_MATERIALS = '''
    def Material "fire"
    {
        token outputs:kma:volume.connect = </m/fire/vol.outputs:out>
        def Shader "vol"
        {
            uniform token info:id = "kma_volume"
            color3f inputs:absorption.connect = </m/fire/density3.outputs:out>
            color3f inputs:emission.connect = </m/fire/emit.outputs:out>
            token outputs:out
        }
        def Shader "density"
        {
            uniform token info:id = "ND_geompropvalue_float"
            string inputs:geomprop = "density"
            float outputs:out
        }
        def Shader "density3"
        {
            uniform token info:id = "ND_convert_float_color3"
            float inputs:in.connect = </m/fire/density.outputs:out>
            color3f outputs:out
        }
        def Shader "emit"
        {
            uniform token info:id = "ND_multiply_color3"
            color3f inputs:in1.connect = </m/fire/density3.outputs:out>
            color3f inputs:in2 = (2, 1, 0.5)
            color3f outputs:out
        }
    }
    def Material "ground"
    {
        token outputs:mtlx:surface.connect = </m/ground/surf.outputs:out>
        def Shader "surf"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.5, 0.5, 0.5)
            float inputs:specular = 0
            token outputs:out
        }
    }'''


def fire_scene(name, light, dscale, limit, samples):
    text = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{
    def Volume "fire" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        rel field:density = </World/fire/density>
        rel material:binding = </m/fire>
        int primvars:karma:object:treat_as_lightsource = %d
        float primvars:karma:object:lightsource_diffusescale = %g
        double3 xformOp:translate = (-1.2, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def OpenVDBAsset "density"
        {
            asset filePath = @%s@
            token fieldName = "density"
        }
    }
    def Volume "smoke"
    {
        rel field:density = </World/smoke/density>
        double3 xformOp:translate = (1.2, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def OpenVDBAsset "density"
        {
            asset filePath = @%s@
            token fieldName = "density"
        }
    }
    def Mesh "ground" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-4, -0.5, 4), (4, -0.5, 4), (4, -0.5, -4), (-4, -0.5, -4)]
        normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (interpolation = "vertex")
        float3[] extent = [(-4, -0.5, -4), (4, -0.5, 4)]
        rel material:binding = </m/ground>
    }
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 60
        float verticalAperture = 60
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 5, 0)
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
}
def Scope "m"
{%s
}
def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        int2 resolution = (60, 60)
        rel products = [</Render/Products/p>]
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1e-06
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = %d
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
        float karma:object:volumelimit = %d
        float karma:object:volumesteprate = 4
        int nova:samples = %d
        float nova:clampIndirect = 0
    }
    def Scope "Products"
    {
        def RenderProduct "p"
        {
            token productName = "out.exr"
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
''' % (light, dscale, D1, D1, FIRE_MATERIALS, samples, limit, samples)
    path = '%s/%s.usda' % (OUT, name)
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    return path


FIRE_REGIONS = {'ground': (slice(27, 33), slice(27, 33)), 'smoke': (slice(27, 33), slice(39, 45)),
                'fire': (slice(27, 33), slice(15, 21))}
for name, light, dscale, limit in (('firelight0_vl0', 0, 1, 0), ('firelight1_vl0', 1, 1, 0),
                                   ('firelight1_vl4', 1, 1, 4), ('firelight1_d2_vl4', 1, 2, 4)):
    if only and name not in only:
        continue
    usd = fire_scene(name, light, dscale, limit, 256)
    means = {}
    for renderer, tag in (('BRAY_HdKarma', 'karma'), ('Nova', 'nova')):
        exr = '%s/%s_%s.exr' % (OUT, name, tag)
        ok, log = render(renderer, usd, exr)
        check(ok, '%s: %s renders' % (name, renderer))
        if tag == 'nova':
            check('Warning' not in log, '%s: Nova prints no warnings' % name)
        if ok:
            import OpenImageIO as oiio
            px = oiio.ImageBuf(exr).get_pixels(oiio.FLOAT)
            means[tag] = {k: [float(px[r, c, ch].mean()) for ch in range(3)] for k, (r, c) in FIRE_REGIONS.items()}
    if len(means) == 2:
        # Karma's single scattering from the volume light is about 4.5 %
        # above Nova's at volume limit 0 (Nova's light, phase and MIS
        # estimates agree; at limit 4 both agree within 0.3 %), 6.2 % since
        # Nova renders with Karma's box filter of these settings (M11 P1;
        # with Nova's own Gaussian 4.6 %); values below 0.002 (the smoke
        # without the light) compare absolutely.
        for region in FIRE_REGIONS:
            k, n = means['karma'][region], means['nova'][region]
            worst = max(abs(n[c] - k[c]) / max(abs(k[c]), 1e-3) for c in range(3))
            absolute = max(abs(n[c] - k[c]) for c in range(3))
            check(worst < 0.07 or absolute < 5e-4, '%s %s: Nova %s vs Karma %s (max rel. deviation %.4f)'
                  % (name, region, [round(v, 5) for v in n], [round(v, 5) for v in k], worst))

print('%d failure(s)' % len(FAILURES))
sys.exit(1 if FAILURES else 0)
