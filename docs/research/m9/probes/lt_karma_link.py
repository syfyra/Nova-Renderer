"""M9 research probe (docs/research/m9/lights.md, section 4.3): Karma CPU's
light-linking and shadow-linking behaviour, measured. Isolated hython only
(tests/houdini/run_hython.ps1); writes hand-made USDA scenes and renders them
with husk (BRAY_HdKarma) in a subprocess.

Scene (top view, orthographic, 8 x 8 units at 15 px per unit): a grey
diffuse ground quad [-3.5, 3.5]^2 at y = 0 (the corners show the background),
two grey diffuse spheres A (x = -1.5) and B (x = +1.5) of radius 0.7 resting
on it, native instances and a point instancer of small boxes at z = -2.5,
a 1 x 1 rect light "key" at (0, 2, -4) aimed at the origin (shadows fall
towards +z, visible beside the spheres), and a dome "dome" of radiance 0.3.
Lights carry LPE tags; every configuration renders the beauty and per-light
LPEs (the dome is not visible to the camera in Karma, so the corners are 0). Measured: region means (and maxima) of the red channel.
Usage: hython lt_karma_link.py <outdir> [spp]
"""

import os
import subprocess
import sys

OUT = os.path.abspath(sys.argv[1]).replace('\\', '/')
SPP = int(sys.argv[2]) if len(sys.argv) > 2 else 256
os.makedirs(OUT, exist_ok=True)
HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
LOG = []


def log(*args):
    line = ' '.join(str(a) for a in args)
    print(line, flush=True)
    LOG.append(line)


PX = 15  # pixels per unit
RES = 8 * PX


def region(x, z, h):
    """Pixel slices of the square [x-h, x+h] x [z-h, z+h] (row = z, image top = -z)."""
    c0, c1 = int(round((x - h + 4) * PX)), int(round((x + h + 4) * PX))
    r0, r1 = int(round((z - h + 4) * PX)), int(round((z + h + 4) * PX))
    return slice(r0, max(r1, r0 + 1)), slice(c0, max(c1, c0 + 1))


REGIONS = {
    'A_top': region(-1.5, 0.0, 0.25),
    'B_top': region(1.5, 0.0, 0.25),
    'B_disc': region(1.5, 0.0, 0.6),
    'shadowA': region(-2.35, 2.2, 0.12),
    'shadowB': region(2.35, 2.2, 0.12),
    'open': region(0.0, 2.2, 0.12),
    'nearA': region(-2.45, -0.2, 0.1),
    'mid': region(0.0, 0.0, 0.15),
    'ni1': region(-2.0, -2.5, 0.12),
    'ni2': region(2.0, -2.5, 0.12),
    'pi0': region(-0.6, -2.5, 0.12),
    'pi1': region(0.6, -2.5, 0.12),
}

MATERIALS = '''
def Scope "m"
{
    def Material "grey"
    {
        token outputs:mtlx:surface.connect = </m/grey/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.5, 0.5, 0.5)
            float inputs:specular = 0
            token outputs:out
        }
    }
    def Material "metal"
    {
        token outputs:mtlx:surface.connect = </m/metal/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 1
            color3f inputs:base_color = (0.9, 0.9, 0.9)
            float inputs:metalness = 1
            float inputs:specular_roughness = 0.15
            token outputs:out
        }
    }
}
'''


def box(name, x, z, s=0.4):
    return '''
        def Cube "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
        {
            double size = %g
            rel material:binding = </m/grey>
            double3 xformOp:translate = (%g, %g, %g)
            uniform token[] xformOpOrder = ["xformOp:translate"]
        }''' % (name, s, x, s / 2, z)


def scene(name, key_extra='', dome_extra='', key_size=1.0, b_material='grey', lpes=(), extra='', key_intensity=20):
    vars_ = [('C', 'C.*[LO]', 'color4f')] + [('L%d' % i, e, 'color3f') for i, e in enumerate(lpes)]
    vdefs = ''.join('''
        def RenderVar "%s"
        {
            uniform token dataType = "%s"
            uniform string sourceName = "%s"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "%s"
            string driver:parameters:aov:name = "%s"
        }''' % (n, d, s, d, n) for n, s, d in vars_)
    order = ', '.join('</Render/Vars/%s>' % n for n, s, d in vars_)
    text = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
def Xform "World"
{
    def Mesh "ground" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-3.5, 0, 3.5), (3.5, 0, 3.5), (3.5, 0, -3.5), (-3.5, 0, -3.5)]
        normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (interpolation = "vertex")
        float3[] extent = [(-3.5, 0, -3.5), (3.5, 0, 3.5)]
        rel material:binding = </m/grey>
    }
    def Sphere "sphA" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        double radius = 0.7
        rel material:binding = </m/grey>
        double3 xformOp:translate = (-1.5, 0.7, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Sphere "sphB" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        double radius = 0.7
        rel material:binding = </m/%s>
        double3 xformOp:translate = (1.5, 0.7, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Xform "ni1" (instanceable = true; references = </Proto>)
    {
        double3 xformOp:translate = (-2, 0, -2.5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Xform "ni2" (instanceable = true; references = </Proto>)
    {
        double3 xformOp:translate = (2, 0, -2.5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def PointInstancer "pi"
    {
        point3f[] positions = [(-0.6, 0, -2.5), (0.6, 0, -2.5)]
        int[] protoIndices = [0, 0]
        rel prototypes = [</World/pi/protos/p0>]
        def Scope "protos"
        {%s
        }
    }
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 80
        float verticalAperture = 80
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 10, 0)
        float3 xformOp:rotateXYZ = (-90, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
}
class Xform "Proto"
{%s
}
def Scope "lights"
{
    def RectLight "key" (prepend apiSchemas = ["CollectionAPI:lightLink", "CollectionAPI:shadowLink"])
    {
        float inputs:intensity = %g
        float inputs:width = %g
        float inputs:height = %g
        string inputs:karma:light:lpetag = "key"%s
        double3 xformOp:translate = (0, 2, -4)
        float3 xformOp:rotateXYZ = (-153.4349, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
    def DomeLight "dome" (prepend apiSchemas = ["CollectionAPI:lightLink", "CollectionAPI:shadowLink"])
    {
        float inputs:intensity = 0.3
        string inputs:karma:light:lpetag = "dome"%s
    }%s
}
%s
def Scope "Render"
{
    def RenderSettings "s"
    {
        rel camera = </World/cam>
        int2 resolution = (%d, %d)
        rel products = [</Render/Products/p>]
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1e-06
        token karma:global:convergence_mode = "Path Traced"
        int karma:global:pathtracedsamples = %d
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
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
''' % (b_material, box('p0', 0, 0), box('m', 0, 0), key_intensity, key_size, key_size, key_extra, dome_extra, extra,
       MATERIALS, RES, RES,
       SPP, order, vdefs)
    path = '%s/%s.usda' % (OUT, name)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return path


def render(name, **kw):
    usd = scene(name, **kw)
    exr = '%s/%s.exr' % (OUT, name)
    run = subprocess.run([os.path.join(HFS, 'bin', 'husk.exe'), '--renderer', 'BRAY_HdKarma', '--headlight', 'none',
                          '-s', '/Render/s', '-o', exr, usd], capture_output=True, text=True, cwd=OUT, timeout=900)
    if run.returncode != 0 or not os.path.exists(exr):
        log('  %s FAILED: %s' % (name, (run.stdout + run.stderr)[-800:]))
        return None
    import OpenImageIO as oiio
    inp = oiio.ImageInput.open(exr)
    out = {}
    part = 0
    while inp.seek_subimage(part, 0):
        names = inp.spec().channelnames
        px = inp.read_image(oiio.FLOAT)
        layer = names[0].rpartition('.')[0] or 'C'
        out[layer] = {k: (float(px[r, c, 0].mean()), float(px[r, c, 0].max())) for k, (r, c) in REGIONS.items()}
        part += 1
    inp.close()
    return out


KEY = ["C<RD><L.'key'>", "C<RD>.+<L.'key'>", "C<R[GS]><L.'key'>", "C.*<L.'key'>", "C.*<L.'dome'>", "C.*<L.'pil'>"]
PIL = '''
    def PointInstancer "pil"
    {
        point3f[] positions = [(-2.35, 0.6, 2.2), (2.35, 0.6, 2.2)]
        int[] protoIndices = [0, 0]
        rel prototypes = [</lights/pil/protos/l0>]
        def Scope "protos"
        {
            def SphereLight "l0"
            {
                float inputs:radius = 0.05
                float inputs:intensity = 50
                string inputs:karma:light:lpetag = "pil"
            }
        }
    }'''
SHOW = ('A_top', 'B_top', 'shadowA', 'shadowB', 'open', 'nearA', 'mid', 'ni1', 'ni2', 'pi0', 'pi1')

CONFIGS = [
    ('base', 'no linking', dict()),
    ('light_A_only', 'key lightLink includeRoot 0, includes sphA', dict(key_extra='''
        uniform bool collection:lightLink:includeRoot = 0
        rel collection:lightLink:includes = </World/sphA>''')),
    ('light_excl_A', 'key lightLink excludes sphA (A still casts its shadow?)', dict(key_extra='''
        rel collection:lightLink:excludes = </World/sphA>''')),
    ('shadow_excl_B', 'key shadowLink excludes sphB', dict(key_extra='''
        rel collection:shadowLink:excludes = </World/sphB>''')),
    ('shadow_excl_B_big', 'key 3x3, shadowLink excludes sphB (BSDF-sampled rays)', dict(key_size=3.0, key_extra='''
        rel collection:shadowLink:excludes = </World/sphB>''')),
    ('base_big', 'key 3x3, no linking', dict(key_size=3.0)),
    ('shadow_none_big', 'key 3x3, shadowLink includeRoot 0 (nothing casts shadows)', dict(key_size=3.0, key_extra='''
        uniform bool collection:shadowLink:includeRoot = 0''')),
    ('shadow_none', 'key shadowLink includeRoot 0 (nothing casts shadows)', dict(key_extra='''
        uniform bool collection:shadowLink:includeRoot = 0''')),
    ('metal_B_excl', 'B metal, key lightLink excludes sphB (reflection of the light on B?)', dict(
        b_material='metal', key_extra='''
        rel collection:lightLink:excludes = </World/sphB>''')),
    ('metal_B', 'B metal, no linking', dict(b_material='metal')),
    ('dome_spheres_only', 'dome lightLink includeRoot 0 includes both spheres', dict(dome_extra='''
        uniform bool collection:lightLink:includeRoot = 0
        rel collection:lightLink:includes = [</World/sphA>, </World/sphB>]''')),
    ('key_instances', 'key lightLink includeRoot 0 includes ni1 and the point instancer', dict(key_extra='''
        uniform bool collection:lightLink:includeRoot = 0
        rel collection:lightLink:includes = [</World/ni1>, </World/pi>]''')),
    ('key_proxy', 'key lightLink excludes the instance proxy /World/ni2/m', dict(key_extra='''
        rel collection:lightLink:excludes = </World/ni2/m>''')),
    ('expr_root_default', 'key membershipExpression /World/sphA, includeRoot unauthored (fallback true)', dict(
        key_extra='''
        pathExpression collection:lightLink:membershipExpression = "/World/sphA"''')),
    ('light_instancer', 'key off; a point instancer of two sphere lights over the shadow regions', dict(
        key_intensity=0, extra=PIL)),
    ('expr_root_blocked', 'key membershipExpression /World/sphA, includeRoot = None (blocked)', dict(key_extra='''
        uniform bool collection:lightLink:includeRoot = None
        pathExpression collection:lightLink:membershipExpression = "/World/sphA"''')),
]

for name, title, kw in CONFIGS:
    r = render(name, lpes=KEY, **kw)
    log('== %s: %s (red channel, region mean)' % (name, title))
    if not r:
        continue
    log('   %-22s ' % 'region' + ' '.join('%9s' % s for s in SHOW))
    rows = [('beauty', 'C')] + [(e, 'L%d' % i) for i, e in enumerate(KEY)]
    for label, layer in rows:
        if layer in r:
            log('   %-22s ' % label + ' '.join('%9.5f' % r[layer][s][0] for s in SHOW))
    if 'metal' in name:
        log('   B_disc max: beauty %.4f, key glossy %.4f' % (r['C']['B_disc'][1], r['L2']['B_disc'][1]))

with open(OUT + '/lt_out_karma_link.txt', 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(l.replace(OUT, '<out>') for l in LOG) + '\n')
