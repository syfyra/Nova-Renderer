"""Karma probes for M8.4 (docs/design/M8.md D13): emissive volumes as light
sources. Karma CPU only, run in an isolated hython
(tests/houdini/run_hython.ps1).

Scene: a fire cube (kma_volume: absorption = density, emission =
(2, 1, 0.5) * density, no scattering) at x = -1.2 and a white smoke cube
(scattering = density) at x = +1.2, both unit cubes of density 1, on a grey
diffuse ground plane at y = -0.5; no other lights. An orthographic camera
looks straight down. Measured: the ground between the cubes (lit by the
fire only), the top of the smoke cube (lit by the fire) and the fire, for
karma:object:treat_as_lightsource 0 / 1 on the fire, its diffuse and
specular scales, volume limits 0 and 4, with light path expressions.
Usage: hython hv_m84_probe.py <outdir>  (writes <outdir>/hv_out_m84_probe.txt)
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

MATERIALS = '''
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


def scene(name, light, dscale, sscale, limit, lpes):
    prims = '''
        int primvars:karma:object:treat_as_lightsource = %d
        float primvars:karma:object:lightsource_diffusescale = %g
        float primvars:karma:object:lightsource_specularscale = %g''' % (light, dscale, sscale)
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
    def Volume "fire" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        rel field:density = </World/fire/density>
        rel material:binding = </m/fire>%s
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
''' % (prims, BOX, BOX, MATERIALS, limit, order, vdefs)
    path = '%s/%s.usda' % (OUT, name)
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    return path


# Regions (rows, columns) of the 60 x 60 top view, 10 px per unit.
REGIONS = {'ground': (slice(27, 33), slice(27, 33)), 'smoke': (slice(27, 33), slice(39, 45)),
           'fire': (slice(27, 33), slice(15, 21))}


def render(name, *args):
    usd = scene(name, *args)
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
        out[layer] = {k: float(px[r, c, 0].mean()) for k, (r, c) in REGIONS.items()}
        part += 1
    inp.close()
    return out


LPES = ['CO', 'CL', 'CDO', 'CDL', 'CVO', 'CVL', 'C.*O', 'C.*L']
for light, dscale, sscale, limit in ((0, 1, 1, 0), (1, 1, 1, 0), (0, 1, 1, 4), (1, 1, 1, 4), (1, 0, 1, 4), (1, 1, 0, 4),
                                     (1, 2, 1, 4)):
    name = 'light%d_d%g_s%g_vl%d' % (light, dscale, sscale, limit)
    r = render(name, light, dscale, sscale, limit, LPES)
    log('== treat_as_lightsource %d, diffusescale %g, specularscale %g, volume limit %d (R means)'
        % (light, dscale, sscale, limit))
    if not r:
        continue
    for region in ('ground', 'smoke', 'fire'):
        parts = ', '.join('%s %.5f' % (e, r['L%d' % i][region]) for i, e in enumerate(LPES) if 'L%d' % i in r)
        log('  %-6s beauty %.5f | %s' % (region, r['C'][region], parts))

with open(OUT + '/hv_out_m84_probe.txt', 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(l.replace(OUT, '<out>') for l in LOG) + '\n')
