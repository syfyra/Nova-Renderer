"""Karma probes for M8.3 (docs/design/M8.md D10, D12, D16), Karma CPU only,
run in an isolated hython (tests/houdini/run_hython.ps1):

  mult    which light multiplier (inputs:diffuse / inputs:specular) lights
          volume scattering, directly and in a furnace;
  lpe     the event type, scatter class and label of volume scattering and
          volume emission in light path expressions;
  aov     first-hit AOVs over volume pixels (a plane behind the cube);
  lobe    kma_volume's secondary lobe (which weight secondarymix is);
  vdf     ND_absorption_vdf (absorbs, does not scatter);
  mod     kma_volume's extinction / contribution modifiers and start level:
          alpha, furnace at volume limits 0, 1, 2 and 64.

Every scene is one unit cube of density 1 (20^3 voxels) at the origin, seen
by an orthographic camera down -Z; values are means over the cube's middle.
Usage: hython hv_m83_probe.py <outdir>  (writes <outdir>/hv_out_m83_probe.txt)
"""

import os
import subprocess
import sys

import hou

OUT = os.path.abspath(sys.argv[1]).replace('\\', '/')
os.makedirs(OUT, exist_ok=True)
HFS = os.environ['HFS']
LOG = []


def log(*args):
    line = ' '.join(str(a) for a in args)
    print(line, flush=True)
    LOG.append(line)


# The cube: density 1, 20 voxels per unit.
geo = hou.node('/obj').createNode('geo', 'box')
vol = geo.createNode('volume')
vol.parm('name').set('density')
vol.parmTuple('size').set((1.0, 1.0, 1.0))
vol.parm('samplediv').set(20)
fill = geo.createNode('volumewrangle')
fill.setInput(0, vol)
fill.parm('snippet').set('@density = 1;')
vdb = geo.createNode('convertvdb')
vdb.setInput(0, fill)
vdb.parm('conversion').set('vdb')
BOX = OUT + '/box_d1.vdb'
vdb.geometry().saveToFile(BOX)

DENSITY3 = '''
        def Shader "density"
        {{
            uniform token info:id = "ND_geompropvalue_float"
            string inputs:geomprop = "density"
            float outputs:out
        }}
        def Shader "density3"
        {{
            uniform token info:id = "ND_convert_float_color3"
            float inputs:in.connect = </m/{name}/density.outputs:out>
            color3f outputs:out
        }}'''


def kma(name, scattering=True, absorption=False, emission=0.0, g=0.0, g2=0.0, mix=0.0, ext=0.0, contrib=1.0,
        start=1):
    """A kma_volume material reading density."""
    body = DENSITY3.format(name=name)
    inputs = []
    if scattering:
        inputs.append('color3f inputs:scattering.connect = </m/%s/density3.outputs:out>' % name)
    if absorption:
        inputs.append('color3f inputs:absorption.connect = </m/%s/density3.outputs:out>' % name)
    if emission:
        body += '''
        def Shader "emit"
        {
            uniform token info:id = "ND_multiply_color3FA"
            color3f inputs:in1.connect = </m/%s/density3.outputs:out>
            float inputs:in2 = %g
            color3f outputs:out
        }''' % (name, emission)
        inputs.append('color3f inputs:emission.connect = </m/%s/emit.outputs:out>' % name)
    inputs += ['float inputs:anisotropy = %g' % g, 'float inputs:secondaryanisotropy = %g' % g2,
               'float inputs:secondarymix = %g' % mix, 'float inputs:extinctionmodifier = %g' % ext,
               'float inputs:contributionmodifier = %g' % contrib, 'int inputs:modifierstartlevel = %d' % start]
    return '''
    def Material "%s"
    {
        token outputs:kma:volume.connect = </m/%s/vol.outputs:out>
        def Shader "vol"
        {
            uniform token info:id = "kma_volume"
            %s
            token outputs:out
        }%s
    }''' % (name, name, '\n            '.join(inputs), body)


def absorption_vdf(name):
    return '''
    def Material "%s"
    {
        token outputs:mtlx:volume.connect = </m/%s/vol.outputs:out>
        def Shader "vol"
        {
            uniform token info:id = "ND_volume"
            token inputs:vdf.connect = </m/%s/vdf.outputs:out>
            token outputs:out
        }
        def Shader "vdf"
        {
            uniform token info:id = "ND_absorption_vdf"
            vector3f inputs:absorption.connect = </m/%s/density3.outputs:out>
            token outputs:out
        }%s
    }''' % (name, name, name, name, DENSITY3.format(name=name).replace('ND_convert_float_color3', 'ND_convert_float_vector3')
            .replace('color3f outputs:out', 'vector3f outputs:out'))


TOP = '''
    def DistantLight "top"
    {
        float inputs:intensity = 1
        bool inputs:normalize = 1
        float inputs:diffuse = %g
        float inputs:specular = %g
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }'''
DOME = '''
    def DomeLight "dome"
    {
        float inputs:intensity = 1
        float inputs:diffuse = %g
        float inputs:specular = %g
    }'''
PLANE = '''
    def Mesh "plane"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -2, -1.5), (2, -2, -1.5), (2, 2, -1.5), (-2, 2, -1.5)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (interpolation = "vertex")
        float3[] extent = [(-2, -2, -1.5), (2, 2, -1.5)]
    }'''


def var(name, source, kind='lpe', dtype='color3f'):
    return name, source, kind, dtype


def scene(name, material=None, lights='', limit=0, vars=(var('C', 'C.*[LO]', 'lpe', 'color4f'),), extra='',
          materials=''):
    bind = ('\n        rel material:binding = <%s>' % material) if material else ''
    vdefs = ''.join('''
        def RenderVar "%s"
        {
            uniform token dataType = "%s"
            uniform string sourceName = "%s"
            uniform token sourceType = "%s"
            token driver:parameters:aov:format = "%s"
            string driver:parameters:aov:name = "%s"
        }''' % (n.replace('<', '_').replace('>', '_'), d, s, k, d, n) for n, s, k, d in vars)
    order = ', '.join('</Render/Vars/%s>' % n.replace('<', '_').replace('>', '_') for n, s, k, d in vars)
    text = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{
    def Volume "cube" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        rel field:density = </World/cube/density>%s
        def OpenVDBAsset "density"
        {
            asset filePath = @%s@
            token fieldName = "density"
        }
    }%s%s
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 20
        float verticalAperture = 20
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
        int2 resolution = (40, 40)
        rel products = [</Render/Products/p>]
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1e-06
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = 256
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
        float karma:object:volumelimit = %d
    }
    def Scope "Products"
    {
        def RenderProduct "p"
        {
            token productName = "out.exr"
            rel orderedVars = [%s]
        }
    }
    def Scope "Vars"
    {%s
    }
}
''' % (bind, BOX, lights, extra, materials, limit, order, vdefs)
    path = '%s/%s.usda' % (OUT, name)
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    return path


def render(name, *args, **kw):
    usd = scene(name, *args, **kw)
    exr = '%s/%s.exr' % (OUT, name)
    run = subprocess.run([os.path.join(HFS, 'bin', 'husk.exe'), '--renderer', 'BRAY_HdKarma', '--headlight', 'none',
                          '-s', '/Render/s', '-o', exr, usd], capture_output=True, text=True, cwd=OUT, timeout=900)
    if run.returncode != 0 or not os.path.exists(exr):
        log('  %s FAILED: %s' % (name, (run.stdout + run.stderr)[-800:]))
        return None
    warnings = [l for l in (run.stdout + run.stderr).splitlines() if 'arning' in l or 'rror' in l]
    for w in warnings[:5]:
        log('  %s: %s' % (name, w.strip()))
    # husk writes one EXR part per render var.
    import OpenImageIO as oiio
    inp = oiio.ImageInput.open(exr)
    out = {}
    part = 0
    while inp.seek_subimage(part, 0):
        names = inp.spec().channelnames
        region = inp.read_image(oiio.FLOAT)[15:25, 15:25]
        for i, ch in enumerate(names):
            layer, _, c = ch.rpartition('.')
            out.setdefault(layer or 'C', {})[c] = float(region[..., i].mean())
        part += 1
    inp.close()
    return out


def rgb(d):
    return '(%s)' % ', '.join('%.5f' % d[c] for c in ('R', 'G', 'B', 'r', 'g', 'b', 'x', 'y', 'z', 'A', 'Z') if c in d)


log('== mult: distant light from above, limit 0 (R mean; cal_distant was 0.03084)')
for d, s in ((1, 1), (0, 1), (1, 0)):
    r = render('mult_top_d%d_s%d' % (d, s), lights=TOP % (d, s))
    log('  diffuse %d specular %d:' % (d, s), rgb(r['C']) if r else '-')
log('== mult: furnace (dome radiance 1, invisible), limit 4')
for d, s in ((1, 1), (0, 1), (1, 0)):
    r = render('mult_dome_d%d_s%d' % (d, s), lights=DOME % (d, s), limit=4)
    log('  diffuse %d specular %d:' % (d, s), rgb(r['C']) if r else '-')

log('== lpe: distant light from above, limit 4')
LPES = ['CVL', 'C<VD>L', 'C<VG>L', 'C<VS>L', 'CDL', 'CGL', 'C<RD>L', 'C<TD>L', 'CVVL', 'CV+L', 'C.*L']
r = render('lpe_top', lights=TOP % (1, 1), limit=4,
           vars=[var('C', 'C.*[LO]', 'lpe', 'color4f')] + [var('L%d' % i, e) for i, e in enumerate(LPES)])
if r:
    log('  beauty', rgb(r['C']))
    for i, e in enumerate(LPES):
        log('  %-8s' % e, rgb(r['L%d' % i]) if 'L%d' % i in r else '-')
for i, e in enumerate(["C<V.'volume'>L", "C<..'volume'>L", 'C<V.>L']):
    r = render('lpe_label%d' % i, lights=TOP % (1, 1), limit=4,
               vars=[var('C', 'C.*[LO]', 'lpe', 'color4f'), var('X', e)])
    log('  %-16s' % e, rgb(r['X']) if r and 'X' in r else '-')
EM = ['CO', 'C.*O', 'CVO', 'CV+O', 'C<V.>O', 'CL']
for limit in (4, 0):
    log('== lpe: emissive scattering cube (emission 0.5 * density), no lights, limit %d' % limit)
    r = render('lpe_emit_vl%d' % limit, material='/m/emit', limit=limit, materials=kma('emit', emission=0.5, start=1),
               vars=[var('C', 'C.*[LO]', 'lpe', 'color4f')] + [var('E%d' % i, e) for i, e in enumerate(EM)])
    if r:
        log('  beauty', rgb(r['C']))
        for i, e in enumerate(EM):
            log('  %-8s' % e, rgb(r['E%d' % i]) if 'E%d' % i in r else '-')

log('== aov: plane at z = -1.5 behind the cube (camera at z = 5), distant light, limit 0')
AOVS = [var('Pz', 'ray:hitPz', 'raw', 'float'), var('P', 'ray:hitP', 'raw'), var('N', 'ray:hitN', 'raw'),
        var('dist', 'ray:hitdist', 'raw', 'float'), var('albedo', 'C<RD>A'), var('valbedo', 'CVA'),
        var('oid', 'ray:objectid', 'raw', 'float')]
r = render('aov_plane', lights=TOP % (1, 1), extra=PLANE, vars=[var('C', 'C.*[LO]', 'lpe', 'color4f')] + AOVS)
if r:
    for k in ['C'] + [a[0] for a in AOVS]:
        log('  %-7s' % k, rgb(r[k]) if k in r else '-')
r = render('aov_alone', lights=TOP % (1, 1), vars=[var('C', 'C.*[LO]', 'lpe', 'color4f')] + AOVS)
if r:
    log('  without the plane:')
    for k in ['C'] + [a[0] for a in AOVS]:
        log('  %-7s' % k, rgb(r[k]) if k in r else '-')

log('== lobe: distant light from above, limit 0 (HG(0.5) at 90 degrees was 0.01655, HG(0) 0.03084)')
for name, kw in (('g05', dict(g=0.5)), ('g05_mix025', dict(g=0.5, g2=0.0, mix=0.25)),
                 ('g0_g2_05_mix025', dict(g=0.0, g2=0.5, mix=0.25))):
    r = render('lobe_' + name, material='/m/' + name, lights=TOP % (1, 1), materials=kma(name, **kw))
    log('  %-16s' % name, rgb(r['C']) if r else '-')

log('== vdf: ND_absorption_vdf(absorption = density)')
r = render('vdf_alpha', material='/m/absvdf', materials=absorption_vdf('absvdf'))
log('  alpha', rgb(r['C']) if r else '-')
r = render('vdf_top', material='/m/absvdf', lights=TOP % (1, 1), materials=absorption_vdf('absvdf'))
log('  lit  ', rgb(r['C']) if r else '-')

log('== mod: kma_volume scattering = density; (extinction modifier, contribution modifier, start level)')
for ext, contrib, start in ((0, 1, 0), (0.5, 1, 0), (0.5, 1, 1), (0.5, 1, 2), (0.25, 1, 0), (0.9, 1, 0),
                            (0, 0.5, 0), (0, 0.5, 1), (0, 0.5, 2), (0.5, 0.5, 1)):
    name = 'mod_e%g_c%g_s%d' % (ext, contrib, start)
    mat = kma('mm', ext=ext, contrib=contrib, start=start)
    values = []
    r = render(name + '_alpha', material='/m/mm', materials=mat)
    values.append('alpha %.5f' % r['C']['A'] if r else 'alpha -')
    for limit in (0, 1, 2, 64):
        r = render('%s_vl%d' % (name, limit), material='/m/mm', lights=DOME % (1, 1), limit=limit, materials=mat)
        values.append('vl%d %.5f' % (limit, r['C']['R']) if r else 'vl%d -' % limit)
    log('  e %-4g c %-4g s %d:' % (ext, contrib, start), ', '.join(values))

with open(OUT + '/hv_out_m83_probe.txt', 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(l.replace(OUT, '<out>') for l in LOG) + '\n')
