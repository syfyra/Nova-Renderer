"""M9 research probe (docs/research/m9/lights.md, section 3.3): Karma CPU's
emissive meshes with karma:object:treat_as_lightsource ("Efficient Emission
Sampling") 0 / 1 / 2, its light multipliers and the LPE events. Isolated
hython only (tests/houdini/run_hython.ps1); writes hand-made USDA scenes and
renders them with husk (BRAY_HdKarma) in a subprocess.

Scene 'pair' (top view, orthographic, 8 x 8 units, 15 px per unit): a grey
diffuse ground (base 0.5, specular 0) at y = 0; an emissive polygon sphere
(32 x 16, radius 0.5, centre (-1.5, 1, 0)) and an emissive quad (1 x 1 at
y = 1, centre (1.5, 1, 0), normal +y, i.e. facing away from the ground).
Emitters: standard_surface base 0, specular 0, emission 1, emission_color
(2, 1, 0.5). No lights. Measured: red-channel means of the emitters seen by
the camera and of the ground beside them, with LPEs, and the pixel noise of
the ground at 16 samples.
Scene 'auto': nine emissive spheres (radius 0.05 / 0.25 / 0.6, emission
0.1 / 1 / 20) with treat_as_lightsource 2 (Auto): whether each one is an L
or an O event to the camera (CL vs CO); 'auto_sun' the same with emission
0.001 / 0.1 / 20 next to a distant light.
Usage: hython lt_karma_emit.py <outdir>
"""

import math
import os
import subprocess
import sys

OUT = os.path.abspath(sys.argv[1]).replace('\\', '/')
os.makedirs(OUT, exist_ok=True)
HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
LOG = []
PX = 15
RES = 8 * PX


def log(*args):
    line = ' '.join(str(a) for a in args)
    print(line, flush=True)
    LOG.append(line)


def region(x, z, h):
    c0, c1 = int(round((x - h + 4) * PX)), int(round((x + h + 4) * PX))
    r0, r1 = int(round((z - h + 4) * PX)), int(round((z + h + 4) * PX))
    return slice(r0, max(r1, r0 + 1)), slice(c0, max(c1, c0 + 1))


def sphere_mesh(name, c, r, material, prims, nu=32, nv=16):
    pts = [(c[0], c[1] + r, c[2])]
    for j in range(1, nv):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            pts.append((c[0] + r * math.sin(th) * math.cos(ph), c[1] + r * math.cos(th), c[2] + r * math.sin(th) * math.sin(ph)))
    pts.append((c[0], c[1] - r, c[2]))
    faces = []
    ring = lambda j, i: 1 + (j - 1) * nu + (i % nu)  # noqa: E731
    for i in range(nu):
        faces.append([0, ring(1, i + 1), ring(1, i)])
    for j in range(1, nv - 1):
        for i in range(nu):
            faces.append([ring(j, i), ring(j, i + 1), ring(j + 1, i + 1), ring(j + 1, i)])
    last = len(pts) - 1
    for i in range(nu):
        faces.append([ring(nv - 1, i), ring(nv - 1, i + 1), last])
    # Orient every face outwards (right-handed winding).
    out = []
    for f in faces:
        p0, p1, p2 = (pts[k] for k in f[:3])
        e1 = [p1[a] - p0[a] for a in range(3)]
        e2 = [p2[a] - p0[a] for a in range(3)]
        n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        d = [p0[a] - c[a] for a in range(3)]
        out.append(f if sum(n[a] * d[a] for a in range(3)) > 0 else f[::-1])
    return '''
    def Mesh "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [%s]
        int[] faceVertexIndices = [%s]
        point3f[] points = [%s]
        float3[] extent = [(%g, %g, %g), (%g, %g, %g)]
        rel material:binding = </m/%s>%s
    }''' % (name, ', '.join(str(len(f)) for f in out), ', '.join(str(k) for f in out for k in f),
            ', '.join('(%.6f, %.6f, %.6f)' % p for p in pts), c[0] - r, c[1] - r, c[2] - r, c[0] + r, c[1] + r,
            c[2] + r, material, prims)


def quad_mesh(name, c, s, material, prims):
    h = s / 2
    x, y, z = c
    return '''
    def Mesh "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(%g, %g, %g), (%g, %g, %g), (%g, %g, %g), (%g, %g, %g)]
        float3[] extent = [(%g, %g, %g), (%g, %g, %g)]
        rel material:binding = </m/%s>%s
    }''' % (name, x - h, y, z + h, x + h, y, z + h, x + h, y, z - h, x - h, y, z - h, x - h, y, z - h, x + h, y,
            z + h, material, prims)


def materials(ground_spec=0.0, emissions=(1.0,)):
    em = ''.join('''
    def Material "emit%d"
    {
        token outputs:mtlx:surface.connect = </m/emit%d/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 0
            float inputs:specular = 0
            float inputs:emission = %g
            color3f inputs:emission_color = (2, 1, 0.5)
            token outputs:out
        }
    }''' % (i, i, e) for i, e in enumerate(emissions))
    return '''
def Scope "m"
{
    def Material "grey"
    {
        token outputs:mtlx:surface.connect = </m/grey/s.outputs:out>
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.5, 0.5, 0.5)
            float inputs:specular = %g
            float inputs:specular_roughness = 0.3
            token outputs:out
        }
    }%s
}
''' % (ground_spec, em)


def document(name, body, mats, spp, lpes):
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
        point3f[] points = [(-4, 0, 4), (4, 0, 4), (4, 0, -4), (-4, 0, -4)]
        normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (interpolation = "vertex")
        float3[] extent = [(-4, 0, -4), (4, 0, 4)]
        rel material:binding = </m/grey>
    }%s
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
''' % (body, mats, RES, RES, spp, order, vdefs)
    path = '%s/%s.usda' % (OUT, name)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return path


def render(name, body, mats, spp, lpes, regions):
    usd = document(name, body, mats, spp, lpes)
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
        out[layer] = {}
        for k, (r, c) in regions.items():
            v = px[r, c, 0]
            out[layer][k] = (float(v.mean()), float(v.std()))
        part += 1
    inp.close()
    return out


def prims(treat, dscale=1.0, sscale=1.0):
    return '''
        int primvars:karma:object:treat_as_lightsource = %d
        float primvars:karma:object:lightsource_diffusescale = %g
        float primvars:karma:object:lightsource_specularscale = %g''' % (treat, dscale, sscale)


# --- scene 'pair' -------------------------------------------------------------
PAIR_REGIONS = {
    'sphere': region(-1.5, 0.0, 0.2),
    'gnd_sph': region(-1.5, 0.95, 0.12),
    'gnd_sph_far': region(-1.5, 2.2, 0.15),
    'quad': region(1.5, 0.0, 0.2),
    'gnd_quad': region(1.5, 0.95, 0.12),
    'gnd_quad_far': region(1.5, 2.2, 0.15),
}
LPES = ['CO', 'CL', 'C<RD>O', 'C<RD>L', 'C<R[GS]>O', 'C<R[GS]>L']
CASES = [
    ('t0', 0, 1, 1, 0.0), ('t1', 1, 1, 1, 0.0), ('t2', 2, 1, 1, 0.0), ('t1_d2', 1, 2, 1, 0.0),
    ('t1_d0', 1, 0, 1, 0.0), ('t0_spec', 0, 1, 1, 1.0), ('t1_spec', 1, 1, 1, 1.0), ('t1_spec_s0', 1, 1, 0, 1.0),
]
for name, treat, d, s, gspec in CASES:
    body = sphere_mesh('glow_sphere', (-1.5, 1.0, 0.0), 0.5, 'emit0', prims(treat, d, s)) + \
        quad_mesh('glow_quad', (1.5, 1.0, 0.0), 1.0, 'emit0', prims(treat, d, s))
    mats = materials(gspec)
    r = render('pair_' + name, body, mats, 256, LPES, PAIR_REGIONS)
    n = render('pair_' + name + '_n16', body, mats, 16, [], PAIR_REGIONS)
    log('== pair %s: treat_as_lightsource %d, diffusescale %g, specularscale %g, ground specular %g'
        % (name, treat, d, s, gspec))
    if not r:
        continue
    keys = list(PAIR_REGIONS)
    log('   %-14s ' % 'region' + ' '.join('%12s' % k for k in keys))
    log('   %-14s ' % 'beauty' + ' '.join('%12.5f' % r['C'][k][0] for k in keys))
    for i, e in enumerate(LPES):
        log('   %-14s ' % e + ' '.join('%12.5f' % r['L%d' % i][k][0] for k in keys))
    if n:
        log('   %-14s ' % 'rel.std @16' + ' '.join(
            '%12.4f' % (n['C'][k][1] / n['C'][k][0] if n['C'][k][0] > 0 else 0) for k in keys))

# --- scene 'auto' -------------------------------------------------------------
RADII = (0.05, 0.25, 0.6)
SUN = '''
    def DistantLight "sun"
    {
        float inputs:intensity = 3
        bool inputs:normalize = 1
        float3 xformOp:rotateXYZ = (-60, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }'''
for title, emit, extra in (('auto', (0.1, 1.0, 20.0), ''), ('auto_sun', (0.001, 0.1, 20.0), SUN)):
    body = extra
    regions = {}
    for a, rad in enumerate(RADII):
        for b, e in enumerate(emit):
            x, z = -2.5 + 2.5 * b, -2.5 + 2.5 * a
            body += sphere_mesh('e%d%d' % (a, b), (x, 1.0, z), rad, 'emit%d' % b, prims(2))
            regions['r%g_e%g' % (rad, e)] = region(x, z, min(0.5 * rad, 0.15))
    r = render(title, body, materials(0.0, emit), 64, ['CO', 'CL'], regions)
    log('== %s: treat_as_lightsource 2 on nine spheres%s; camera sees CO (object) or CL (light)'
        % (title, ' plus a distant light of irradiance 3' if extra else ''))
    if r:
        for k in regions:
            log('   %-12s beauty %10.5f  CO %10.5f  CL %10.5f' % (k, r['C'][k][0], r['L0'][k][0], r['L1'][k][0]))

# --- scene 'auto_varied': a noise-driven emission and a dense mesh -------------
NOISE = '''
def Scope "mv"
{
    def Material "noisy"
    {
        token outputs:mtlx:surface.connect = </mv/noisy/s.outputs:out>
        def Shader "n"
        {
            uniform token info:id = "ND_noise3d_color3"
            color3f inputs:amplitude = (1, 1, 1)
            float inputs:pivot = 1
            color3f outputs:out
        }
        def Shader "s"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 0
            float inputs:specular = 0
            float inputs:emission = 1
            color3f inputs:emission_color.connect = </mv/noisy/n.outputs:out>
            token outputs:out
        }
    }
}'''
body = sphere_mesh('noisy', (-1.5, 1.0, 0.0), 0.6, 'emit0', prims(2)).replace('</m/emit0>', '</mv/noisy>') +     sphere_mesh('dense', (1.5, 1.0, 0.0), 0.6, 'emit0', prims(2), nu=256, nv=128)
regions = {'noisy': region(-1.5, 0.0, 0.15), 'dense': region(1.5, 0.0, 0.15)}
r = render('auto_varied', body, materials(0.0, (1.0,)) + NOISE, 64, ['CO', 'CL'], regions)
log('== auto_varied: treat_as_lightsource 2 on a noise-driven emitter (32 x 16) and a dense one (256 x 128, 32,768 faces)')
if r:
    for k in regions:
        log('   %-12s beauty %10.5f  CO %10.5f  CL %10.5f' % (k, r['C'][k][0], r['L0'][k][0], r['L1'][k][0]))

with open(OUT + '/lt_out_karma_emit.txt', 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(l.replace(OUT, '<out>') for l in LOG) + '\n')
