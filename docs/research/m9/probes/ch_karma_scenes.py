# M9 research probe (curves-hair.md, sections 2 and 4): writes the Karma CPU
# calibration scenes for curves and hair as hand-written USD (no Houdini
# session, nothing SideFX-generated). Rendered by ch_karma_renders.ps1, measured
# by ch_karma_measure.py, predicted by ch_hair_proto.cpp.
#
#   & "$HFS\python313\python.exe" -I docs\research\m9\probes\ch_karma_scenes.py <outdir> [<ch_authored.usda>]
#
# Conventions: an orthographic camera at z = 10 looks down -z (aperture in
# tenths of a unit), so camera rays have wo = +z. Distant lights use
# normalize = 1: the intensity is the irradiance. Karma: path traced, fixed
# samples, box pixel filter of width 1, no colour limit.
import json
import math
import os
import sys

import numpy as np

outdir = os.path.abspath(sys.argv[1])
authored = os.path.abspath(sys.argv[2]).replace('\\', '/') if len(sys.argv) > 2 else None
os.makedirs(outdir, exist_ok=True)
scenes = []

# ----------------------------------------------------------------- render vars
RAW = {  # name: (sourceName, sourceType, dataType, format)
    'N': ('ray:hitN', 'raw', 'normal3f', 'float3'),
    'Ng': ('ray:hitNg', 'raw', 'normal3f', 'float3'),
    'P': ('ray:hitP', 'raw', 'point3f', 'float3'),
    'uv': ('ray:hituv', 'raw', 'vector3f', 'float3'),
    'st': ('st', 'primvar', 'float3', 'float3'),
    'primid': ('ray:primid', 'raw', 'int', 'float'),
    'element': ('ray:element', 'raw', 'int', 'float'),
}
LPE_EVENTS = {'RD': 'C<RD>.*L', 'RG': 'C<RG>.*L', 'RS': 'C<RS>.*L', 'TD': 'C<TD>.*L', 'TG': 'C<TG>.*L',
              'TS': 'C<TS>.*L', 'direct': 'C[DGS]L', 'indirect': 'C[DGS].+L'}


def render_vars(raw=(), lpes=None):
    """RenderVar prims: C (beauty with alpha), raw/primvar utilities, LPEs."""
    out = ['''        def RenderVar "C"
        {
            uniform token dataType = "color4f"
            uniform string sourceName = "C.*[LO]"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "color4f"
            string driver:parameters:aov:name = "C"
        }''']
    names = ['C']
    for n in raw:
        src, typ, dt, fmt = RAW[n]
        out.append('''        def RenderVar "%s"
        {
            uniform token dataType = "%s"
            uniform string sourceName = "%s"
            uniform token sourceType = "%s"
            token driver:parameters:aov:format = "%s"
            string driver:parameters:aov:name = "%s"
            bool driver:parameters:aov:multiSampled = 0
        }''' % (n, dt, src, typ, fmt, n))
        names.append(n)
    for n, expr in (lpes or {}).items():
        out.append('''        def RenderVar "%s"
        {
            uniform token dataType = "color3f"
            uniform string sourceName = "%s"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "color3f"
            string driver:parameters:aov:name = "%s"
        }''' % (n, expr, n))
        names.append(n)
    return '\n'.join(out), names


def scene(name, body, res, aperture, raw=(), lpes=None, samples=64, settings=(), materials='', camera=''):
    """One USD file: camera, settings, products, the body (prims under /World)."""
    vars_txt, names = render_vars(raw, lpes)
    ordered = ', '.join('</Render/Vars/%s>' % n for n in names)
    attrs = '\n'.join('        %s' % s for s in settings)
    txt = '''#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
    renderSettingsPrimPath = "/Render/settings"
)

def Xform "World"
{
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = %g
        float verticalAperture = %g
        float2 clippingRange = (0.01, 100)
%s        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
%s
}
%s
def Scope "Render"
{
    def RenderSettings "settings"
    {
        rel camera = </World/cam>
        int2 resolution = (%d, %d)
        rel products = </Render/Products/beauty>
        token karma:global:convergence_mode = "Path Traced"
        string karma:global:pixeloracle = '["uniform",{}]'
        int karma:global:pathtracedsamples = %d
        string karma:global:pixelfilter = "box"
        float karma:global:pixelfilterscale = 1
        float karma:global:colorlimit = 1e9
        float karma:global:colorlimitindirect = 1e9
%s
    }

    def Scope "Products"
    {
        def RenderProduct "beauty"
        {
            token productName = "%s.exr"
            rel orderedVars = [%s]
        }
    }

    def Scope "Vars"
    {
%s
    }
}
''' % (aperture[0], aperture[1], camera, body, materials, res[0], res[1], samples, attrs, name, ordered, vars_txt)
    with open(os.path.join(outdir, name + '.usda'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(txt)
    scenes.append(name)


def fl(v):
    return '(%s)' % ', '.join('%.9g' % x for x in v)


def curves(name, pts, counts, widths, ctype='linear', basis=None, wrap='nonperiodic', normals=None, extra='',
           material=None, primvars=None):
    """A BasisCurves prim; widths: one value (constant) or one per point (vertex)."""
    winterp = 'constant' if len(widths) == 1 else 'vertex'
    lines = ['    def BasisCurves "%s" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )\n    {' % name,
             '        uniform token type = "%s"' % ctype,
             '        uniform token wrap = "%s"' % wrap,
             '        int[] curveVertexCounts = [%s]' % ', '.join(str(c) for c in counts),
             '        point3f[] points = [%s]' % ', '.join(fl(p) for p in pts),
             '        float[] widths = [%s] (\n            interpolation = "%s"\n        )' % (
                 ', '.join('%.9g' % w for w in widths), winterp)]
    if basis:
        lines.append('        uniform token basis = "%s"' % basis)
    if normals is not None:
        lines.append('        normal3f[] normals = [%s] (\n            interpolation = "vertex"\n        )' % ', '.join(
            fl(n) for n in normals))
    for k, v in (primvars or {}).items():
        lines.append('        %s' % v.replace('NAME', 'primvars:' + k))
    if material:
        lines.append('        rel material:binding = <%s>' % material)
    if extra:
        lines.append(extra)
    lines.append('    }')
    return '\n'.join(lines) + '\n'


def distant(name, wi, intensity=1.0, tag=None):
    """A normalized distant light whose light arrives from direction wi."""
    z = np.array(wi, float)
    z /= np.linalg.norm(z)
    a = np.array([0.0, 1.0, 0.0]) if abs(z[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
    x = np.cross(a, z)
    x /= np.linalg.norm(x)
    y = np.cross(z, x)
    m = '((%s, 0), (%s, 0), (%s, 0), (0, 0, 0, 1))' % (', '.join('%.9g' % c for c in x), ', '.join('%.9g' % c for c in y),
                                                        ', '.join('%.9g' % c for c in z))
    t = '        string inputs:karma:light:lpetag = "%s"\n' % tag if tag else ''
    return '''    def DistantLight "%s"
    {
        float inputs:intensity = %g
        bool inputs:normalize = 1
        float inputs:angle = 0
%s        matrix4d xformOp:transform = %s
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
''' % (name, intensity, t, m)


DOME = '''    def DomeLight "dome"
    {
        float inputs:intensity = 1
    }
'''

# =================================================================== geometry
# geo_width: 4 x 1 units at 800 x 200 (5 mm pixels). Straight linear curves
# along y spanning the frame (no ends visible), widths 0.2 / 0.0025 / 0.0005;
# a cubic B-spline (pinned) of width 0.2; a sphere point of width 0.2;
# the 0.2 curve again with karma:object:width_scale 2.
body = ''
body += curves('lin_w02', [(-1.6, -2, 0), (-1.6, 2, 0)], [2], [0.2])
body += curves('lin_w0025', [(-1.1, -2, 0), (-1.1, 2, 0)], [2], [0.0025])
body += curves('lin_w0005', [(-0.8, -2, 0), (-0.8, 2, 0)], [2], [0.0005])
body += curves('bsp_w02', [(-0.4, y, 0) for y in (-2, -0.7, 0.7, 2)], [4], [0.2], 'cubic', 'bspline', 'pinned')
body += '''    def Points "pt_w02"
    {
        point3f[] points = [(0.3, 0, 0)]
        float[] widths = [0.2] (
            interpolation = "vertex"
        )
    }
'''
body += curves('lin_w02_scale2', [(1.0, -2, 0), (1.0, 2, 0)], [2], [0.2],
               primvars={'karma:object:width_scale': 'float NAME = 2 (\n            interpolation = "constant"\n        )'})
body += curves('lin_vw', [(1.6, -2, 0), (1.6, 2, 0)], [2], [0.1, 0.3])  # width varies along y: 0.2 at y=0
scene('geo_width', body, (800, 200), (40, 10), raw=('uv', 'primid', 'element'))

# geo_caps: the ends of a linear and a pinned B-spline curve (width 0.1) at
# y = 0, seen in [-0.25, 0.25]^2 at 200 x 200 (2.5 mm pixels).
body = curves('lin_end', [(-0.12, -2, 0), (-0.12, 0, 0)], [2], [0.1])
body += curves('bsp_end', [(0.12, y, 0) for y in (-2, -1.4, -0.6, 0)], [4], [0.1], 'cubic', 'bspline', 'pinned')
scene('geo_caps', body, (200, 200), (5, 5), raw=('uv', 'N'))

# geo_ribbon: straight pinned B-splines of width 0.2 whose normals are tilted
# 60 degrees from the view direction (about the curve's axis): with
# karma:object:curve_style unset, 0 (flat), 1 (rounded), 2 (oriented), 3 (USD
# default), and a linear curve with normals and style 2.
n60 = (math.sin(math.radians(60)), 0.0, math.cos(math.radians(60)))
body = ''
for i, style in enumerate((None, 0, 1, 2, 3)):
    x = -2.5 + i
    pv = None if style is None else {'karma:object:curve_style': 'int NAME = %d (\n            interpolation = "constant"\n        )' % style}
    body += curves('bsp_style_%s' % ('unset' if style is None else style), [(x, y, 0) for y in (-2, -0.7, 0.7, 2)], [4],
                   [0.2], 'cubic', 'bspline', 'pinned', normals=[n60] * 4, primvars=pv)
body += curves('lin_style_2', [(2.5, -2, 0), (2.5, 2, 0)], [2], [0.2], normals=[n60] * 2,
               primvars={'karma:object:curve_style': 'int NAME = 2 (\n            interpolation = "constant"\n        )'})
scene('geo_ribbon', body, (1200, 100), (60, 5), raw=('N', 'Ng', 'uv'))

# geo_shade: default shading (no material) under a distant light from the
# camera (irradiance 1): a tube without displayColor, a tube with displayColor
# 1, a flat ribbon (style 0) with displayColor 1, and a mesh quad with
# displayColor 1 for reference. 4 x 1 units at 800 x 200.
dc1 = {'displayColor': 'color3f[] NAME = [(1, 1, 1)] (\n            interpolation = "constant"\n        )'}
body = curves('tube_nocolor', [(-1.5, -2, 0), (-1.5, 2, 0)], [2], [0.4])
body += curves('tube_white', [(-0.5, -2, 0), (-0.5, 2, 0)], [2], [0.4], primvars=dc1)
pv = dict(dc1)
pv['karma:object:curve_style'] = 'int NAME = 0 (\n            interpolation = "constant"\n        )'
body += curves('flat_white', [(0.5, -2, 0), (0.5, 2, 0)], [2], [0.4], primvars=pv)
body += '''    def Mesh "quad_white"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(1.3, -2, 0), (1.7, -2, 0), (1.7, 2, 0), (1.3, 2, 0)]
        color3f[] primvars:displayColor = [(1, 1, 1)] (
            interpolation = "constant"
        )
    }
'''
body += distant('key', (0, 0, 1))
scene('geo_shade', body, (800, 200), (40, 10), raw=('N', 'Ng', 'uv', 'P'), samples=64)
# The same objects lit from 60 degrees to the side, split into diffuse and
# glossy reflection; plus the parametrisation of multi-segment curves (hituv)
# in a second row: a 3-segment linear curve and a 2-segment B-spline along x.
body2 = body.replace(distant('key', (0, 0, 1)), distant('key', (math.sin(math.radians(60)), 0, 0.5)))
body2 = body2.replace('(-1.5, -2, 0), (-1.5, 2, 0)', '(-1.5, -2, 0), (-1.5, 0.3, 0)')
body2 += curves('lin3', [(-1.9, 0.4, 0), (-1.1, 0.4, 0), (-0.9, 0.4, 0), (1.9, 0.4, 0)], [4], [0.05])
body2 += curves('bsp5', [(-1.9 + 0.95 * k, 0.47, 0) for k in range(5)], [5], [0.03], 'cubic', 'bspline', 'nonperiodic')
scene('geo_shade_side', body2, (800, 200), (40, 10), raw=('N', 'uv', 'primid', 'element'),
      lpes={'RD': 'C<RD>.*L', 'RG': 'C<RG>.*L', 'RS': 'C<RS>.*L'}, samples=64)

# Cubic bases: the same four control points as Bezier, B-spline (nonperiodic
# and pinned) and Catmull-Rom (pinned), width 0.02, 1000 x 500 over 2 x 1 units.
CP = [(-0.9, -0.4, 0), (-0.4, 0.9, 0), (0.4, -0.9, 0), (0.9, 0.4, 0)]
for basis, wrap in (('bezier', 'nonperiodic'), ('bspline', 'nonperiodic'), ('bspline', 'pinned'),
                    ('catmullRom', 'pinned')):
    scene('geo_basis_%s_%s' % (basis, wrap), curves('c', CP, [4], [0.02], 'cubic', basis, wrap), (1000, 500), (20, 10))

# A many-span Bezier with tight bends (ratio of width to bend radius ~ 1/4).
pts = []
for k in range(4):
    x0 = -0.8 + 0.4 * k
    pts += [(x0, -0.3, 0), (x0, 0.3, 0), (x0 + 0.2, 0.3, 0)] if k % 2 == 0 else [(x0, 0.3, 0), (x0, -0.3, 0), (x0 + 0.2, -0.3, 0)]
pts.append((0.8, 0.3 if len(pts) % 2 else -0.3, 0))
pts = pts[:13]
scene('geo_basis_bezier_wave', curves('c', pts, [len(pts)], [0.05], 'cubic', 'bezier'), (1000, 500), (20, 10))

# Ids: one BasisCurves prim with three 3-point linear curves (2 segments
# each), and a NurbsCurves prim (order 4, clamped knots = one Bezier span):
# does Karma render NurbsCurves, and exactly?
body = curves('ids', [(x, y, 0) for x in (-1.5, -1.0, -0.5) for y in (-0.45, 0.0, 0.45)], [3, 3, 3], [0.1])
body += '''    def NurbsCurves "nurbs"
    {
        int[] curveVertexCounts = [4]
        int[] order = [4]
        double[] knots = [0, 0, 0, 0, 1, 1, 1, 1]
        double2[] ranges = [(0, 1)]
        point3f[] points = [(0.1, -0.4, 0), (0.35, 0.9, 0), (1.15, -0.9, 0), (1.6, 0.4, 0)]
        float[] widths = [0.02] (
            interpolation = "constant"
        )
    }
'''
scene('geo_ids', body, (800, 200), (40, 10), raw=('primid', 'element', 'uv'))

# ====================================================================== hair
# Single fibre: a straight linear tube of width 0.5 through the origin along
# t = (0, cos a, sin a) (root at -2t), seen in a 1 x 0.2 unit window at
# 200 x 40 px (100 px across the fibre). wo = +z, so theta_o = a.
HAIR_LIGHTS = {  # tag: direction the light arrives from (fibre frame for a = 0)
    'l0': (0, 0, 1),                                   # front (camera side)
    'l1': (1, 0, 0),                                   # side
    'l2': (0, 0, -1),                                  # back (TT)
    'l3': (0, math.sin(math.radians(10)), math.cos(math.radians(10))),   # front, 10 deg towards the tip
    'l4': (0, -math.sin(math.radians(10)), math.cos(math.radians(10))),  # front, 10 deg towards the root
    'l5': (0.5, 0.5, math.sqrt(0.5)),                  # generic
}


def mx_hair(path, absorption, rough=(0.3, 0.3), cuticle=0.5, ior=1.55, tints=((1, 1, 1),) * 3):
    lr, ar = rough
    lr = min(max(lr, 0.001), 1.0)
    ar = min(max(ar, 0.001), 1.0)
    v = (0.726 * lr + 0.812 * lr * lr + 3.7 * lr ** 20) ** 2
    s = 0.265 * ar + 1.194 * ar * ar + 5.372 * ar ** 22
    return '''    def Material "%s"
    {
        token outputs:mtlx:surface.connect = <%s/surf.outputs:out>

        def Shader "surf"
        {
            uniform token info:id = "ND_surface"
            token inputs:bsdf.connect = <%s/hair.outputs:out>
            token outputs:out
        }

        def Shader "hair"
        {
            uniform token info:id = "ND_chiang_hair_bsdf"
            color3f inputs:tint_R = %s
            color3f inputs:tint_TT = %s
            color3f inputs:tint_TRT = %s
            float inputs:ior = %g
            float2 inputs:roughness_R = (%.9g, %.9g)
            float2 inputs:roughness_TT = (%.9g, %.9g)
            float2 inputs:roughness_TRT = (%.9g, %.9g)
            float inputs:cuticle_angle = %.9g
            vector3f inputs:absorption_coefficient = %s
            token outputs:out
        }
    }
''' % (path.split('/')[-1], path, path, fl(tints[0]), fl(tints[1]), fl(tints[2]), ior, v, s, v * 0.25, s, v * 4, s,
       cuticle, fl(absorption))


def kma_hair(path, **inputs):
    lines = []
    for k, v in inputs.items():
        if isinstance(v, tuple):
            lines.append('            vector3f inputs:%s = %s' % (k, fl(v)) if k == 'absorption' else
                         '            color3f inputs:%s = %s' % (k, fl(v)))
        elif isinstance(v, int) and not isinstance(v, bool):
            lines.append('            int inputs:%s = %d' % (k, v))
        else:
            lines.append('            float inputs:%s = %.9g' % (k, v))
    return '''    def Material "%s"
    {
        token outputs:kma:surface.connect = <%s/hair.outputs:surface>

        def Shader "hair"
        {
            uniform token info:id = "kma_hair"
%s
            token outputs:surface
        }
    }
''' % (path.split('/')[-1], path, '\n'.join(lines))


MATS = {
    'mx_s0': lambda p: mx_hair(p, (0, 0, 0)),
    'mx_sa': lambda p: mx_hair(p, (0.2, 0.5, 1.0)),
    'mx_sa_tilt': lambda p: mx_hair(p, (0.2, 0.5, 1.0), cuticle=0.5 + 3.0 / 180.0),
    'mx_sa_r2': lambda p: mx_hair(p, (0.2, 0.5, 1.0), rough=(0.15, 0.5)),
    'mx_sa_ior': lambda p: mx_hair(p, (0.2, 0.5, 1.0), ior=1.3),
    'kma_s0': lambda p: kma_hair(p, melanin=0.0, melaninRedness=0.0, shift=0.0),
    'kma_sa': lambda p: kma_hair(p, melanin=0.0, melaninRedness=0.0, shift=0.0, absorption=(0.2, 0.5, 1.0)),
    'kma_def': lambda p: kma_hair(p),
    'kma_def_shift0': lambda p: kma_hair(p, shift=0.0),
    'kma_m08': lambda p: kma_hair(p, melanin=0.8, melaninRedness=0.0, shift=0.0),
    'kma_base': lambda p: kma_hair(p, melanin=0.0, melaninRedness=0.0, shift=0.0, baseColor=(0.8, 0.5, 0.2)),
    'kma_rough': lambda p: kma_hair(p, melanin=0.0, melaninRedness=0.0, shift=0.0, absorption=(0.2, 0.5, 1.0),
                                    roughness=0.15, roughnessAnisotropic=1, roughnessAzimuthal=0.5),
    'kma_diffuse': lambda p: kma_hair(p, melanin=0.0, melaninRedness=0.0, shift=0.0, diffuse=1.0,
                                      diffuseColor=(0.5, 0.5, 0.5)),
    'kma_coat': lambda p: kma_hair(p, melanin=0.0, melaninRedness=0.0, shift=0.0, absorption=(0.2, 0.5, 1.0), coat=1.0),
    'kma_tilt': lambda p: kma_hair(p, melanin=0.0, melaninRedness=0.0, shift=3.0 / 90.0, absorption=(0.2, 0.5, 1.0)),
    'mx_s0_ior1': lambda p: mx_hair(p, (0, 0, 0), ior=1.0),
    'kma_s0_ior1': lambda p: kma_hair(p, melanin=0.0, melaninRedness=0.0, shift=0.0, ior=1.0),
}
# Karma Hair's melanin controls: (melanin, redness) and thicknessScale.
MEL_SWEEP = ([(m, 0.0) for m in (0.1, 0.2, 0.3, 0.5, 0.6, 0.7, 0.9, 0.95)] + [(0.5, r) for r in (0.25, 0.5, 0.75, 1.0)] +
             [(0.3, 1.0), (0.2, 1.0)])
# How the colour controls combine: melanin + base colour + absorption, the
# "base" lightness, the tint, and the per-strand melanin randomisation.
MATS['kma_combo'] = lambda p: kma_hair(p, melanin=0.5, melaninRedness=0.0, shift=0.0, baseColor=(0.8, 0.5, 0.2),
                                       absorption=(0.1, 0.1, 0.1))
MATS['kma_base05'] = lambda p: kma_hair(p, melanin=0.5, melaninRedness=0.0, shift=0.0, base=0.5)
MATS['kma_tint'] = lambda p: kma_hair(p, melanin=0.5, melaninRedness=0.0, shift=0.0, tint=(0.5, 1.0, 1.0))
MATS['kma_mrand'] = lambda p: kma_hair(p, melanin=0.5, melaninRedness=0.0, shift=0.0, melaninRandomize=0.5)
for m, r in MEL_SWEEP:
    MATS['kma_mel_%02d_%03d' % (round(m * 100), round(r * 100))] = (
        lambda p, m=m, r=r: kma_hair(p, melanin=m, melaninRedness=r, shift=0.0))
for t in (0.5, 2.0):
    MATS['kma_thick_%02d' % round(t * 10)] = lambda p, t=t: kma_hair(p, melanin=0.5, melaninRedness=0.0, shift=0.0,
                                                                     thicknessScale=t)


def fiber_body(alpha_deg, material):
    a = math.radians(alpha_deg)
    t = (0.0, math.cos(a), math.sin(a))
    p0 = tuple(-2 * c for c in t)
    p1 = tuple(2 * c for c in t)
    return curves('fiber', [p0, p1], [2], [0.5], material='/World/mtl/m')


def mat_scope(*defs):
    """Materials under /World/mtl (USDA indentation is irrelevant)."""
    return '    def Scope "mtl"\n    {\n' + ''.join(defs) + '    }\n'


def fiber_scene(name, mat, alpha_deg, lights, lpes, samples=256, settings=()):
    body = fiber_body(alpha_deg, mat)
    body += mat_scope(MATS[mat]('/World/mtl/m'))
    body += lights
    scene(name, body, (200, 40), (10, 2), raw=('N', 'uv'), lpes=lpes, samples=samples, settings=settings)


LIGHT_LPES = {tag: "C.*<L.'%s'>" % tag for tag in HAIR_LIGHTS}
lights_txt = ''.join(distant(tag, wi, tag=tag) for tag, wi in HAIR_LIGHTS.items())
HIGH = ('float karma:object:diffuselimit = 64', 'float karma:object:reflectlimit = 64',
        'float karma:object:refractlimit = 64')
for mat in ('mx_s0', 'mx_sa', 'kma_s0', 'kma_sa', 'kma_def', 'kma_m08', 'kma_base', 'kma_diffuse'):
    for a in (0, 30, 60):
        fiber_scene('fur_%s_a%d' % (mat, a), mat, a, DOME, LPE_EVENTS, samples=256, settings=HIGH)
for mat in [k for k in MATS if k.startswith('kma_mel_') or k.startswith('kma_thick_')] + [
        'mx_s0_ior1', 'kma_s0_ior1', 'kma_combo', 'kma_base05', 'kma_tint', 'kma_mrand']:
    fiber_scene('fur_%s_a0' % mat, mat, 0, DOME, LPE_EVENTS, samples=256, settings=HIGH)
for mat in ('mx_sa', 'mx_sa_tilt', 'mx_sa_r2', 'mx_sa_ior', 'kma_sa', 'kma_def', 'kma_def_shift0', 'kma_rough',
            'kma_coat', 'kma_diffuse', 'kma_tilt'):
    for a in (0, 30):
        lp = dict(LIGHT_LPES)
        if a == 0:
            lp.update({k: v for k, v in LPE_EVENTS.items() if k in ('RD', 'RG', 'TD', 'TG', 'RS', 'TS')})
        fiber_scene('dl_%s_a%d' % (mat, a), mat, a, lights_txt, lp, samples=256)

# Hair ball: 2000 random straight fibres (length 0.6, width 0.01, random
# directions, centres uniform in a ball of radius 0.35), white furnace (dome 1),
# Karma Hair without absorption (kma_s0), with ray limits varied.
rng = np.random.default_rng(1234)
n = 2000
c = rng.normal(size=(n, 3))
c = c / np.linalg.norm(c, axis=1)[:, None] * (rng.random(n) ** (1 / 3) * 0.35)[:, None]
d = rng.normal(size=(n, 3))
d /= np.linalg.norm(d, axis=1)[:, None]
pts = []
for i in range(n):
    pts += [tuple(c[i] - 0.3 * d[i]), tuple(c[i] + 0.3 * d[i])]
ball = curves('ball', pts, [2] * n, [0.01], material='/World/mtl/m')
ball_mat = mat_scope(MATS['kma_s0']('/World/mtl/m'))
LIMITS = {
    'high': HIGH,
    'default': (),
    'refl0': ('float karma:object:diffuselimit = 64', 'float karma:object:reflectlimit = 0',
              'float karma:object:refractlimit = 64'),
    'refr0': ('float karma:object:diffuselimit = 64', 'float karma:object:reflectlimit = 64',
              'float karma:object:refractlimit = 0'),
    'diff0': ('float karma:object:diffuselimit = 0', 'float karma:object:reflectlimit = 64',
              'float karma:object:refractlimit = 64'),
}
for k, s in LIMITS.items():
    scene('ball_%s' % k, ball + ball_mat + DOME, (240, 240), (12, 12), lpes=LPE_EVENTS, samples=128, settings=s)
# The same ball with the MaterialX Chiang BSDF (mx_s0) and with Karma Hair's
# defaults (kma_def), limits 64 and Karma's defaults.
for mat in ('mx_s0', 'kma_def'):
    for k in ('high', 'default'):
        scene('ball%s_%s' % (mat.replace('_', ''), k), ball + mat_scope(MATS[mat]('/World/mtl/m')) + DOME, (240, 240),
              (12, 12), lpes=LPE_EVENTS, samples=128, settings=LIMITS[k])

# Hydra probe scene: the authored SOP curves plus a deforming curve, an
# instanced curve, a hair material and the Karma curve settings.
if authored:
    body = curves('moving', [(0, 0, 0), (0, 1, 0)], [2], [0.05]).replace(
        '        point3f[] points = [(0, 0, 0), (0, 1, 0)]',
        '        point3f[] points.timeSamples = {\n            1: [(0, 0, 0), (0, 1, 0)],\n            2: [(0.5, 0, 0), (0.5, 1, 0)],\n        }')
    body += '''    def PointInstancer "inst"
    {
        point3f[] positions = [(0, 0, 0), (1, 0, 0)]
        int[] protoIndices = [0, 0]
        rel prototypes = [</World/inst/proto>]
        def Scope "proto"
        {
%s        }
    }
''' % curves('pc', [(0, 0, 0), (0, 0.5, 0)], [2], [0.02]).replace('\n', '\n        ').replace('    def', '        def', 1)
    body += '''    def HermiteCurves "hermite"
    {
        int[] curveVertexCounts = [2]
        point3f[] points = [(0, 0, 0), (1, 0, 0)]
        vector3f[] tangents = [(0, 1, 0), (0, -1, 0)]
        float[] widths = [0.05] (interpolation = "constant")
    }
'''
    body += mat_scope(MATS['kma_def']('/World/mtl/kh'), MATS['mx_sa']('/World/mtl/mh'))
    body += '''    def Xform "authored" (
        prepend references = @%s@</curves>
    )
    {
        over "hairgen"
        {
            over "curve_0" (
                prepend apiSchemas = ["MaterialBindingAPI"]
            )
            {
                rel material:binding = </World/mtl/kh>
            }
        }
        over "poly_ptwidth"
        {
            over "curve_0" (
                prepend apiSchemas = ["MaterialBindingAPI"]
            )
            {
                rel material:binding = </World/mtl/mh>
            }
        }
    }
''' % authored
    scene('probe_hydra', body, (64, 64), (40, 40), samples=1,
          settings=('int karma:object:curve_style = 1', 'float karma:object:width_scale = 1'),
          camera='        double shutter:open = -0.25\n        double shutter:close = 0.25\n')

with open(os.path.join(outdir, 'scenes.json'), 'w', encoding='utf-8') as f:
    json.dump(scenes, f, indent=0)
print('wrote %d scenes to %s' % (len(scenes), outdir))
