# Subsurface calibration scenes (docs/design/M9.md M9.3), written as plain
# USDA text at test time (no Houdini needed); the generator of the research
# (docs/research/m9/probes/sss_scenes.py), whose Karma CPU measurements
# (docs/research/m9/sss.md section 4) check_sss.py compares against. Every
# scene renders in Karma CPU and in Nova with the same RenderSettings prim:
# path traced, fixed samples, box filter, no colour limit, true caustics.
#
#   python sss_scenes.py <outdir> [spp scale]
#
# Writes <outdir>/<scene>.usda, <outdir>/common.usda and <outdir>/scenes.json
# (scene name -> what sss_measure.py measures). The materials are MaterialX
# Standard Surface 1.0.1 (ND_standard_surface_surfaceshader; f_opbr and
# slab_opbr_chroma: OpenPBR 1.1) with specular 0 unless a scene says
# otherwise, so only the subsurface lobe is seen. Geometry is polygonal (UV
# spheres with smooth normals, boxes, a room shell, quads): Karma XPU supports
# SSS only on meshes and points, and the walk needs a surface to exit through.
import json
import math
import os
import sys

outdir = os.path.abspath(sys.argv[1])
spp_scale = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
os.makedirs(outdir, exist_ok=True)


def write(name, text):
    with open(os.path.join(outdir, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


# ------------------------------------------------------------------ common
# One RenderVar per LPE; every scene lists the vars it wants in its product.
LPES = {
    'C': 'C.*[LO]',
    'sss': "C<...'sss'>.*[LO]",       # Karma's standard 'BSDF labelled sss' var
    'td_all': 'C<TD>.*[LO]',          # Karma's help: the sss component
    'rd_all': 'C<RD>.*[LO]',
    'td_L': 'C<TD>L',
    'td_sssL': "C<TD'sss'>L",
    'td_td_L': 'C<TD><TD>L',
    'tdrd_L': 'C<TD><RD>L',
    'O': 'CO',
    'td_O': 'C<TD>O',
    'td_td_O': 'C<TD><TD>O',
    'td_td_td_O': 'C<TD><TD><TD>O',
    'rd_O': 'C<RD>O',
    'td_rd_O': 'C<TD><RD>O',
    'tg_all': 'C<TG>.*[LO]',
    'rg_all': 'C<RG>.*[LO]',
}

vars_txt = '\n'.join('''        def RenderVar "%s"
        {
            uniform token dataType = "color3f"
            uniform string sourceName = "%s"
            uniform token sourceType = "lpe"
            token driver:parameters:aov:format = "color3f"
            string driver:parameters:aov:name = "%s"
        }''' % (k, v, k) for k, v in LPES.items() if k != 'C')

COMMON = '''#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

class "_class_Sss"
{
    token karma:global:convergence_mode = "Path Traced"
    string karma:global:pixeloracle = '["uniform",{}]'
    string karma:global:pixelfilter = "box"
    float karma:global:pixelfilterscale = 1
    float karma:global:colorlimit = 1e9
    float karma:global:colorlimitindirect = 1e9
    bool karma:object:causticsenable = 1
    float karma:object:causticsroughnessclamp = 0
    token nova:pixelFilter = "box"
    float nova:pixelFilterScale = 1
    float nova:clampIndirect = 0
}

def Scope "Render"
{
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
%s
    }
}
''' % vars_txt
write('common.usda', COMMON)


# ------------------------------------------------------------------ geometry
def mesh(name, points, normals, counts, indices, material, extra=''):
    p = ', '.join('(%.7g, %.7g, %.7g)' % v for v in points)
    nrm = ''
    if normals is not None:
        nrm = '        normal3f[] normals = [%s] (\n            interpolation = "vertex"\n        )\n' % \
            ', '.join('(%.7g, %.7g, %.7g)' % v for v in normals)
    return '''    def Mesh "%s" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [%s]
        int[] faceVertexIndices = [%s]
        point3f[] points = [%s]
%s        rel material:binding = </World/mtl/%s>
%s    }
''' % (name, ', '.join(map(str, counts)), ', '.join(map(str, indices)), p, nrm, material, extra)


def uv_sphere(name, radius, center, material, nu=128, nv=64, extra=''):
    pts, nrm = [], []
    pts.append((center[0], center[1] + radius, center[2]))
    nrm.append((0, 1, 0))
    for j in range(1, nv):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            n = (math.sin(th) * math.cos(ph), math.cos(th), -math.sin(th) * math.sin(ph))
            nrm.append(n)
            pts.append(tuple(center[k] + radius * n[k] for k in range(3)))
    pts.append((center[0], center[1] - radius, center[2]))
    nrm.append((0, -1, 0))
    bottom = len(pts) - 1
    counts, idx = [], []

    def ring(j, i):
        return 1 + (j - 1) * nu + (i % nu)
    for i in range(nu):  # top cap, counter-clockwise seen from outside
        counts.append(3)
        idx += [0, ring(1, i), ring(1, i + 1)]
    for j in range(1, nv - 1):
        for i in range(nu):
            counts.append(4)
            idx += [ring(j, i), ring(j + 1, i), ring(j + 1, i + 1), ring(j, i + 1)]
    for i in range(nu):
        counts.append(3)
        idx += [ring(nv - 1, i), bottom, ring(nv - 1, i + 1)]
    return mesh(name, pts, nrm, counts, idx, material, extra)


def box_faces(lo, hi, inward=False):
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    pts = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
           (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    # outward, counter-clockwise (USD default orientation rightHanded)
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (2, 3, 7, 6), (1, 2, 6, 5), (0, 4, 7, 3)]
    if inward:
        faces = [tuple(reversed(f)) for f in faces]
    return pts, faces


def box(name, lo, hi, material, extra=''):
    pts, faces = box_faces(lo, hi)
    return mesh(name, pts, None, [4] * 6, [i for f in faces for i in f], material, extra)


def shell(name, outer, inner, material, extra=''):
    """A closed solid between two boxes: outer faces out, inner faces in."""
    p0, f0 = box_faces(*outer)
    p1, f1 = box_faces(*inner, inward=True)
    faces = f0 + [tuple(i + 8 for i in f) for f in f1]
    return mesh(name, p0 + p1, None, [4] * 12, [i for f in faces for i in f], material, extra)


def quad(name, lo, hi, z, material, extra=''):
    pts = [(lo[0], lo[1], z), (hi[0], lo[1], z), (hi[0], hi[1], z), (lo[0], hi[1], z)]
    return mesh(name, pts, None, [4], [0, 1, 2, 3], material, extra)


# ------------------------------------------------------------------ materials
def material(name, shader='ND_standard_surface_surfaceshader', **inputs):
    if shader == 'ND_open_pbr_surface_surfaceshader':
        base = {'base_weight': 0.0, 'specular_weight': 0.0, 'subsurface_weight': 1.0}
    else:
        base = {'base': 0.0, 'specular': 0.0, 'subsurface': 1.0}
    base.update(inputs)
    lines = []
    for k, v in base.items():
        if isinstance(v, bool):
            lines.append('            bool inputs:%s = %d' % (k, int(v)))
        elif isinstance(v, (tuple, list)):
            lines.append('            color3f inputs:%s = (%g, %g, %g)' % (k, v[0], v[1], v[2]))
        else:
            lines.append('            float inputs:%s = %g' % (k, v))
    return '''        def Material "%s"
        {
            token outputs:mtlx:surface.connect = </World/mtl/%s/ss.outputs:out>

            def Shader "ss"
            {
                uniform token info:id = "%s"
%s
                token outputs:out
            }
        }
''' % (name, name, shader, '\n'.join('    ' + l for l in lines))


# ------------------------------------------------------------------ cameras, lights
def ortho_cam(width, center=(0, 0), z=6.0, height=None):
    height = width if height is None else height
    return '''    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = %g
        float verticalAperture = %g
        float2 clippingRange = (0.01, 100)
        double3 xformOp:translate = (%g, %g, %g)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
''' % (width * 10, height * 10, center[0], center[1], z)


def persp_cam(pos=(0, 0, 0), fov_ap=4.0):
    return '''    def Camera "cam"
    {
        float focalLength = 50
        float horizontalAperture = %g
        float verticalAperture = %g
        float2 clippingRange = (0.01, 100)
        double3 xformOp:translate = (%g, %g, %g)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
''' % (fov_ap, fov_ap, pos[0], pos[1], pos[2])


def distant(name, rot_y, intensity=1.0):
    # Emits along its -Z axis; rotateY(a) turns -Z into (-sin a, 0, -cos a).
    # normalize = 1: the intensity is the irradiance at normal incidence.
    return '''    def DistantLight "%s"
    {
        float inputs:intensity = %g
        bool inputs:normalize = 1
        float inputs:angle = 0.53
        float3 xformOp:rotateXYZ = (0, %g, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
''' % (name, intensity, rot_y)


DOME = '''    def DomeLight "dome"
    {
        float inputs:intensity = 1
    }
'''


# ------------------------------------------------------------------ scenes
SCENES = {}


def scene(name, body, mats, res, spp, vars_=('C',), settings=(), measure=None):
    spp = max(1, int(round(spp * spp_scale)))
    attrs = '\n'.join('        %s' % s for s in settings)
    var_rels = ', '.join('</Render/Vars/%s>' % v for v in vars_)
    txt = '''#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
    renderSettingsPrimPath = "/Render/s"
    subLayers = [@./common.usda@]
)

def Xform "World"
{
    def Scope "mtl"
    {
%s    }

%s}

over "Render"
{
    def RenderProduct "p"
    {
        token productName = "%s.exr"
        rel orderedVars = [%s]
    }

    def RenderSettings "s" (
        inherits = </_class_Sss>
    )
    {
        rel camera = </World/cam>
        rel products = </Render/p>
        int2 resolution = (%d, %d)
        int karma:global:pathtracedsamples = %d
        int nova:samples = %d
%s
    }
}
''' % (''.join(mats), body, name, var_rels, res[0], res[1], spp, spp, attrs)
    write(name + '.usda', txt)
    SCENES[name] = measure or {}


def limits(sss=None, diffuse=None):
    s = []
    if sss is not None:
        s += ['float karma:object:ssslimit = %g' % sss, 'float nova:object:sssLimit = %g' % sss]
    if diffuse is not None:
        s += ['float karma:object:diffuselimit = %g' % diffuse, 'float nova:object:diffuseLimit = %g' % diffuse]
    return s


FURNACE_VARS = ('C', 'sss', 'td_all', 'rd_all', 'td_L', 'td_sssL', 'td_td_L', 'tdrd_L', 'tg_all', 'rg_all')
FUR = {'kind': 'disc', 'radius': 1.0, 'width': 2.2, 'inner': 0.95}

# 1. White furnace: one sphere (radius 1) in a uniform dome of radiance 1.
#    Three subsurface colours per scene, one per channel (the channels are
#    independent with an achromatic radius).
for nm, kw in (
        ('f_a1', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.001)),
        ('f_a2', dict(subsurface_color=(0.05, 0.95, 0.99), subsurface_scale=0.001)),
        ('f_gp5', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.001, subsurface_anisotropy=0.5)),
        ('f_gm5', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.001, subsurface_anisotropy=-0.5)),
        ('f_gp9', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.001, subsurface_anisotropy=0.9)),
        ('f_spec', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.001, specular=1.0)),
        ('f_s003', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.03)),
        ('f_s01', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.1)),
        ('f_s03', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.3)),
        ('f_s1', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=1.0)),
        ('f_s3', dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=3.0)),
        ('f_chroma', dict(subsurface_color=(0.8, 0.8, 0.8), subsurface_radius=(1, 0.5, 0.25), subsurface_scale=0.3)),
):
    scene(nm, ortho_cam(2.2) + DOME + uv_sphere('ball', 1.0, (0, 0, 0), 'm'), [material('m', **kw)],
          (96, 96), 256, FURNACE_VARS, measure=dict(FUR, params=kw))
for nm, lim in (('f_lim0', limits(sss=0)), ('f_lim0d0', limits(sss=0, diffuse=0)), ('f_d0', limits(diffuse=0)),
                ('f_lim4', limits(sss=4, diffuse=4))):
    kw = dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.001)
    scene(nm, ortho_cam(2.2) + DOME + uv_sphere('ball', 1.0, (0, 0, 0), 'm'), [material('m', **kw)],
          (96, 96), 256, FURNACE_VARS, lim, measure=dict(FUR, params=kw, settings=lim))

# 2. Event counting: a camera inside a closed room whose thick walls are SSS
#    (walk albedo per channel 0.25 / 0.5 / 0.75) and emit radiance 1. With one
#    SSS event per walk the camera sees 1 + A + A^2 + ... up to the limit.
ROOM_VARS = ('C', 'sss', 'O', 'td_O', 'td_td_O', 'td_td_td_O', 'rd_O', 'td_rd_O', 'td_all', 'rd_all')
room_body = persp_cam((0, 0, 0), 4.0) + shell('room', ((-3, -3, -3), (3, 3, 3)), ((-1, -1, -1), (1, 1, 1)), 'w')
room_mat = material('w', subsurface_color=(0.25, 0.5, 0.75), subsurface_scale=0.003, emission=1.0,
                    emission_color=(1, 1, 1))
for nm, lim in (('room_l0', limits(sss=0)), ('room_l1', limits(sss=1)), ('room_l2', limits(sss=2)),
                ('room_l3', limits(sss=3)), ('room_l8', limits(sss=8, diffuse=8)), ('room_l1d0', limits(sss=1, diffuse=0)),
                ('room_l2d0', limits(sss=2, diffuse=0)), ('room_l1d4', limits(sss=1, diffuse=4))):
    scene(nm, room_body, [room_mat], (32, 32), 512, ROOM_VARS, lim, measure={'kind': 'all', 'settings': lim})

# 3. Transmission through slabs (4 x 4 x d) lit from behind by a distant light
#    of irradiance 1 at normal incidence; an orthographic camera in front sees
#    the central 1 x 1. mfp = scale = 0.05.
SLAB_VARS = ('C', 'sss', 'td_all', 'rd_all', 'td_L', 'tg_all')
SLAB = {'kind': 'center', 'width': 1.0}
for d in (0.025, 0.05, 0.1, 0.2, 0.4):
    kw = dict(subsurface_color=(0.5, 0.8, 0.95), subsurface_scale=0.05)
    nm = 'slab_d%s' % ('%g' % d).replace('.', 'p')
    scene(nm, ortho_cam(1.0) + distant('back', 180) + box('slab', (-2, -2, -d / 2), (2, 2, d / 2), 'm'),
          [material('m', **kw)], (32, 32), 1024, SLAB_VARS, measure=dict(SLAB, params=kw, d=d))
for nm, kw, rot in (
        ('slab_chroma', dict(subsurface_color=(0.8, 0.8, 0.8), subsurface_radius=(1, 0.5, 0.25), subsurface_scale=0.1), 180),
        ('slab_gp5', dict(subsurface_color=(0.5, 0.8, 0.95), subsurface_scale=0.05, subsurface_anisotropy=0.5), 180),
        ('slab_gm5', dict(subsurface_color=(0.5, 0.8, 0.95), subsurface_scale=0.05, subsurface_anisotropy=-0.5), 180),
        ('slab_a60', dict(subsurface_color=(0.5, 0.8, 0.95), subsurface_scale=0.05), 120),
        ('slab_spec', dict(subsurface_color=(0.5, 0.8, 0.95), subsurface_scale=0.05, specular=1.0), 180),
):
    scene(nm, ortho_cam(1.0) + distant('back', rot) + box('slab', (-2, -2, -0.05), (2, 2, 0.05), 'm'),
          [material('m', **kw)], (32, 32), 1024, SLAB_VARS, measure=dict(SLAB, params=kw, d=0.1))
# Thin-walled subsurface on a single quad, lit from behind.
kw = dict(subsurface_color=(0.5, 0.8, 0.95), thin_walled=True)
scene('slab_thin', ortho_cam(1.0) + distant('back', 180) + quad('q', (-2, -2), (2, 2), 0.0, 'm'),
      [material('m', **kw)], (32, 32), 256, SLAB_VARS, measure=dict(SLAB, params=kw))
# The lit side of a thick slab: reflection at normal incidence (camera and
# light on the same side).
kw = dict(subsurface_color=(0.5, 0.8, 0.95), subsurface_scale=0.05)
scene('slab_refl', ortho_cam(1.0) + distant('front', 0) + box('slab', (-2, -2, -2), (2, 2, 0), 'm'),
      [material('m', **kw)], (32, 32), 512, SLAB_VARS, measure=dict(SLAB, params=kw))

# 4. Ballistic light: an emissive card (radiance 1, x < 0) behind a slab of
#    optical thickness 0.5; no lights. A walk that enters and leaves straight
#    shows the card's edge sharply, attenuated by exp(-0.5) = 0.61.
card = material('card', base=0.0, subsurface=0.0, emission=1.0, emission_color=(1, 1, 1))
kw = dict(subsurface_color=(0.5, 0.8, 0.95), subsurface_scale=0.1)
for nm, mkw in (('ball_d05', kw), ('ball_d05_spec', dict(kw, specular=1.0))):
    scene(nm, ortho_cam(1.0, height=0.25) + quad('card', (-3, -3), (0, 3), -1.0, 'card') + box('slab', (-2, -2, -0.025), (2, 2, 0.025), 'm'),
          [material('m', **mkw), card], (64, 16), 1024, ('C', 'sss', 'td_all', 'O', 'td_O', 'td_td_O'),
          measure={'kind': 'xprofile', 'width': 1.0, 'params': mkw, 'd': 0.05})

# 5. Exit/entry angular behaviour: a thick sphere lit from the camera's
#    direction (irradiance mu at a point with normal-view cosine mu).
kw = dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.001)
scene('head', ortho_cam(2.2) + distant('key', 0) + uv_sphere('ball', 1.0, (0, 0, 0), 'm'), [material('m', **kw)],
      (96, 96), 256, ('C', 'sss', 'td_all', 'td_L'), measure=dict(FUR, kind='mu', params=kw))

# 6. Light bleeding past the terminator: sphere lit from the side (+x),
#    radiance along the horizontal centre line, for three scales.
for s in (0.01, 0.05, 0.2):
    kw = dict(subsurface_color=(0.5, 0.8, 0.95), subsurface_scale=s)
    nm = 'term_s%s' % ('%g' % s).replace('.', 'p')
    scene(nm, ortho_cam(2.2, height=0.55) + distant('side', 90) + uv_sphere('ball', 1.0, (0, 0, 0), 'm'), [material('m', **kw)],
          (128, 32), 512, ('C', 'sss', 'td_all'), measure={'kind': 'xprofile', 'width': 2.2, 'params': kw})

# 7. OpenPBR 1.1 and partial subsurface weight.
OPBR = 'ND_open_pbr_surface_surfaceshader'
kw = dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_radius=0.001, subsurface_radius_scale=(1, 1, 1))
scene('f_opbr', ortho_cam(2.2) + DOME + uv_sphere('ball', 1.0, (0, 0, 0), 'm'), [material('m', OPBR, **kw)],
      (96, 96), 256, FURNACE_VARS, measure=dict(FUR, params=kw))
kw = dict(subsurface_color=(0.8, 0.8, 0.8), subsurface_radius=0.1)  # radius_scale default (1, 0.5, 0.25)
scene('slab_opbr_chroma', ortho_cam(1.0) + distant('back', 180) + box('slab', (-2, -2, -0.05), (2, 2, 0.05), 'm'),
      [material('m', OPBR, **kw)], (32, 32), 1024, SLAB_VARS, measure=dict(SLAB, params=kw, d=0.1))
kw = dict(subsurface=0.5, base=1.0, base_color=(0.2, 0.2, 0.2), subsurface_color=(0.8, 0.8, 0.8), subsurface_scale=0.001)
scene('f_mix', ortho_cam(2.2) + DOME + uv_sphere('ball', 1.0, (0, 0, 0), 'm'), [material('m', **kw)],
      (96, 96), 256, FURNACE_VARS, measure=dict(FUR, params=kw))
# Coat over subsurface (coat 1, IOR 1.5, roughness 0.1).
kw = dict(subsurface_color=(0.2, 0.5, 0.8), subsurface_scale=0.001, coat=1.0)
scene('f_coat', ortho_cam(2.2) + DOME + uv_sphere('ball', 1.0, (0, 0, 0), 'm'), [material('m', **kw)],
      (96, 96), 256, FURNACE_VARS, measure=dict(FUR, params=kw))

# 8. An open surface: a single quad with (not thin-walled) subsurface, lit
#    from the front; the walk never finds an exit.
kw = dict(subsurface_color=(0.5, 0.8, 0.95), subsurface_scale=0.05)
scene('open_quad', ortho_cam(1.0) + distant('front', 0) + quad('q', (-2, -2), (2, 2), 0.0, 'm'),
      [material('m', **kw)], (32, 32), 256, SLAB_VARS, measure=dict(SLAB, params=kw))

with open(os.path.join(outdir, 'scenes.json'), 'w', encoding='utf-8', newline='\n') as f:
    json.dump(SCENES, f, indent=1)
print('wrote %d scenes to %s' % (len(SCENES), outdir))
