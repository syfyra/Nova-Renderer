"""Writes the texture-bake scenes of check_karma_bake.py (docs/design/M11-P8.md).

The scenes carry RenderSettings prims shaped like the Karma Texture Baker
LOP's (karma:global:bake_* values, raw RenderVars, one EXR product per
var with a relative name, the generated bake camera), on small analytic
meshes: low quads with high planes (trace direction, bias, maximum
distance, tangent frames), slabs and ceilings (cavity, curvature,
thickness, occlusion), 135-degree edges (edge, rounded normal), position
boxes, the image filters, and a bumpy sphere baked onto a coarse one.
Standard library only:  python make_scenes.py [outdir]
"""

import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# name -> (dataType, aov format, sourceName); as husd/karmatexturebaker.py authors them
VARS = {
    "Nt": ("normal3f", "float3", "Nt"), "Ds": ("vector3f", "float3", "Ds"), "Vd": ("vector3f", "float3", "Vd"),
    "Vdt": ("vector3f", "float3", "Vdt"), "Oc": ("color3f", "float3", "Oc"), "Cv": ("vector3f", "float3", "Cv"),
    "Th": ("color3f", "float3", "Th"), "Cu": ("vector3f", "float3", "Cu"), "Eg": ("vector3f", "float3", "Eg"),
    "Rn": ("vector3f", "float3", "Rn"), "Af": ("float", "float", "Af"), "P": ("point3f", "float3", "__bake_P"),
    "N": ("normal3f", "float3", "N"), "Ng": ("normal3f", "float3", "Ng"),
    "base_color": ("color3f", "float3", "export_base_color"),
}

DEFAULTS = {
    "bake_cage": ("string", ""), "bake_cagemode": ("string", ""), "bake_cu_bias": ("float", 0.75),
    "bake_cu_enable": ("bool", 1), "bake_cu_samples": ("int", 16), "bake_cu_scale": ("float", 1),
    "bake_cu_sdist": ("float", 0.025), "bake_cv_bias": ("float", 0), "bake_cv_enable": ("bool", 1),
    "bake_cv_maxdist": ("float", 0.05), "bake_cv_samples": ("int", 16), "bake_eg_cuspangle": ("float", 30),
    "bake_eg_enable": ("bool", 1), "bake_eg_mode": ("int", 0), "bake_eg_radius": ("float", 0.01),
    "bake_eg_raybias": ("float", 0.001), "bake_eg_samples": ("int", 9), "bake_high": ("string", ""),
    "bake_mode": ("string", "trace"), "bake_oc_enable": ("bool", 1), "bake_oc_maxdist": ("float", -1),
    "bake_oc_raybias": ("float", 0.001), "bake_oc_samples": ("int", 16), "bake_pbox_max": ("float3", (1, 1, 1)),
    "bake_pbox_min": ("float3", (-1, -1, -1)), "bake_pbox_mode": ("string", "world"),
    "bake_pbox_uniform": ("bool", 1), "bake_rn_cuspangle": ("float", 30), "bake_rn_enable": ("bool", 1),
    "bake_rn_mode": ("int", 0), "bake_rn_radius": ("float", 0.01), "bake_rn_raybias": ("float", 0.001),
    "bake_rn_samples": ("int", 9), "bake_th_enable": ("bool", 1), "bake_th_samples": ("int", 1),
    "bake_trace_bias": ("float", 0.001), "bake_trace_bias_attr": ("string", ""),
    "bake_trace_bias_opt": ("string", "global"), "bake_trace_bias_tex": ("string", ""),
    "bake_trace_max": ("float", 1e17), "bake_trace_max_attr": ("string", ""),
    "bake_trace_max_opt": ("string", "global"), "bake_trace_max_tex": ("string", ""),
    "bake_trace_skew_tex": ("string", ""), "bake_traceset": ("string", "*"), "bake_udim": ("int", 1001),
    "bake_use_lighting": ("bool", 0), "bake_use_mikkt": ("bool", 1), "bake_uv": ("string", "st"),
}


def fmt(values, kind):
    if kind == "int":
        return "[" + ", ".join(str(int(v)) for v in values) + "]"
    if kind == "vec2":
        return "[" + ", ".join("(%.9g, %.9g)" % tuple(v) for v in values) + "]"
    return "[" + ", ".join("(%.9g, %.9g, %.9g)" % tuple(v) for v in values) + "]"


def mesh(name, points, counts, indices, st=None, normals=None, hidden=False, left=False, indent="    "):
    lines = ['def Mesh "%s"' % name, "{",
             "    int[] faceVertexCounts = %s" % fmt(counts, "int"),
             "    int[] faceVertexIndices = %s" % fmt(indices, "int"),
             "    point3f[] points = %s" % fmt(points, "vec3")]
    if normals is not None:
        lines += ["    normal3f[] normals = %s (" % fmt(normals, "vec3"), '        interpolation = "faceVarying"', "    )"]
    if st is not None:
        lines += ["    texCoord2f[] primvars:st = %s (" % fmt(st, "vec2"), '        interpolation = "faceVarying"', "    )"]
    if hidden:
        lines += ['    string primvars:karma:object:rendervisibility = "" (', '        interpolation = "constant"', "    )"]
    if left:
        lines.append('    uniform token orientation = "leftHanded"')
    lines += ['    uniform token subdivisionScheme = "none"', "}"]
    return "\n".join(lines)


def xform(name, children):
    body = "\n".join("    " + l for l in "\n\n".join(children).split("\n"))
    return 'def Xform "%s"\n{\n%s\n}' % (name, body)


def quad_strip(cases, z=0.0):
    """Low quads [x0, x0 + 1] x [0, 1] at z, each with a UV rect (u0, v0, u1, v1)
    and optional mirroring/left-handedness: cases = [(x0, rect, mirror, left)]."""
    pts, st, counts, idx = [], [], [], []
    for x0, (u0, v0, u1, v1), mirror, left in cases:
        corners = [(0, 0), (1, 0), (1, 1), (0, 1)]
        if left:
            corners = [corners[0], corners[3], corners[2], corners[1]]
        for cx, cy in corners:
            pts.append((x0 + cx, cy, z))
            u = 1 - cx if mirror else cx
            st.append((u0 + (u1 - u0) * u, v0 + (v1 - v0) * cy))
        counts.append(4)
        idx += list(range(len(pts) - 4, len(pts)))
    return pts, counts, idx, st


def box(name, lo, hi):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    p = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    f = [[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4], [2, 3, 7, 6], [1, 2, 6, 5], [0, 4, 7, 3]]
    normals = [(0, 0, -1)] * 4 + [(0, 0, 1)] * 4 + [(0, -1, 0)] * 4 + [(0, 1, 0)] * 4 + [(1, 0, 0)] * 4 + [(-1, 0, 0)] * 4
    return xform(name, [mesh("mesh", p, [4] * 6, sum(f, []), normals=normals)])


def plane(name, x0, z, sx=0.0, sy=0.0, flip=False):
    """A plane over [x0 - 0.5, x0 + 1.5] x [-0.5, 1.5]: z + sx (x - x0) + sy y."""
    corners = [(x0 - 0.5, -0.5), (x0 + 1.5, -0.5), (x0 + 1.5, 1.5), (x0 - 0.5, 1.5)]
    p = [(x, y, z + sx * (x - x0) + sy * y) for x, y in corners]
    return xform(name, [mesh("mesh", p, [4], [0, 3, 2, 1] if flip else [0, 1, 2, 3])])


def settings(name, low, varnames, res, spp, overrides=None, imagefilter="[]"):
    attrs = dict(DEFAULTS)
    attrs["bake_low"] = ("string", low)
    for k, v in (overrides or {}).items():
        attrs[k] = (attrs.get(k, ("string", ""))[0], v)
    lines = ['    def RenderSettings "%s" (' % name, '        apiSchemas = ["KarmaRenderSettingsAPI"]', "    )", "    {",
             "        rel camera = </__HoudiniTextureBakeCamera>", "        uniform bool disableMotionBlur = 1",
             "        bool karma:global:autoraybias = 0"]
    for k in sorted(attrs):
        t, v = attrs[k]
        if t == "string":
            v = '"%s"' % v
        elif t == "float3":
            v = "(%g, %g, %g)" % tuple(v)
        elif t in ("int", "bool"):
            v = str(int(v))
        else:
            v = "%.9g" % v
        lines.append("        custom %s karma:global:%s = %s" % (t, k, v))
    lines += ["        float karma:global:dicingqualityscale = 1",
              "        string karma:global:imagefilter = '%s'" % imagefilter,
              "        float karma:global:offscreenquality = 1",
              "        int karma:global:samplesperpixel = %d" % spp,
              "        custom int karma:object:truedisplace = 1",
              "        rel products = [%s]" % ", ".join("</Render/%s/Products/p_%s>" % (name, v) for v in varnames),
              "        uniform int2 resolution = (%d, %d)" % res,
              '        def Scope "Vars"', "        {"]
    for v in varnames:
        dt, f, src = VARS[v]
        lines += ['            def RenderVar "%s" (' % v, '                apiSchemas = ["KarmaRenderVarAPI"]', "            )",
                  "            {", '                uniform token dataType = "%s"' % dt,
                  '                custom token driver:parameters:aov:format = "%s"' % f,
                  '                custom string driver:parameters:aov:name = "%s"' % v,
                  '                uniform string sourceName = "%s"' % src, '                uniform token sourceType = "raw"',
                  "            }"]
    lines += ["        }", '        def Scope "Products"', "        {"]
    for v in varnames:
        lines += ['            def RenderProduct "p_%s" (' % v, '                apiSchemas = ["HuskRenderProductAPI"]',
                  "            )", "            {", "                rel orderedVars = </Render/%s/Vars/%s>" % (name, v),
                  '                token productName = "%s_%s.exr"' % (name, v),
                  '                uniform token productType = "raster"', "            }"]
    lines += ["        }", "    }"]
    return "\n".join(lines)


CAMERA = """def Camera "__HoudiniTextureBakeCamera"
{
    float2 clippingRange = (0.001, 10000)
    float focalLength = 35
    float horizontalAperture = 10
    float verticalAperture = 10
    matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 5, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}"""


def write(out, name, prims, settings_list):
    path = os.path.join(out, name + ".usda")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write('#usda 1.0\n(\n    metersPerUnit = 1\n    upAxis = "Y"\n)\n\n')
        for p in prims + [CAMERA]:
            f.write(p + "\n\n")
        f.write('def Scope "Render"\n{\n' + "\n\n".join(settings_list) + "\n}\n")
    return path


# UV islands with gaps between them: Karma's maps darken where the pixel
# filter mixes islands that touch in UV but lie apart in space (measured:
# occlusion 0.55 along such seams), a Karma artefact the scenes avoid.
QUADS = [(0.0, (0.06, 0.06, 0.44, 0.44)), (2.0, (0.56, 0.06, 0.94, 0.44)), (4.0, (0.06, 0.56, 0.44, 0.94)),
         (6.0, (0.56, 0.56, 0.94, 0.94))]


def trace_scene(out):
    # Low normals tilted (0.5, 0, 1) on case d, +Z elsewhere.
    pts, counts, idx, st = quad_strip([(x0, r, False, False) for x0, r in QUADS])
    n = (0.5 / math.sqrt(1.25), 0.0, 1.0 / math.sqrt(1.25))
    normals = [(0, 0, 1)] * 12 + [n] * 4
    low = xform("low", [mesh("mesh", pts, counts, idx, st=st, normals=normals, hidden=True)])
    highs = [plane("ha", 0.0, 0.2, 0.3, 0.0),           # above, tilted in x (outward ray)
             plane("hb", 2.0, -0.2, 0.0, 0.3),          # below, tilted in y (inward ray)
             plane("hc1", 4.0, 0.3), plane("hc2", 4.0, -0.2),  # both sides: the nearer one
             plane("hd", 6.0, 0.2, 0.0, 0.2)]           # above, rays along the tilted normal
    v = ["Nt", "Ds", "Vd", "Vdt", "P", "N", "Af"]
    write(out, "trace", [low] + highs, [
        settings("trace", "/low/mesh", v, (32, 32), 16),
        settings("trace_max", "/low/mesh", ["Af", "Vd"], (32, 32), 16, {"bake_trace_bias": 0.1, "bake_trace_max": 0.25}),
        settings("trace_offset", "/low/mesh", ["Af", "Vd"], (32, 32), 16,
                 {"bake_cagemode": "cageoffset", "bake_trace_bias": 0.25})])


def frames_scene(out):
    cases = [(x0, r, m, False) for (x0, r), m in zip(QUADS[:2], (False, True))]
    pts, counts, idx, st = quad_strip(cases)
    right = mesh("r", pts, counts, idx, st=st, normals=[(0, 0, 1)] * 8, hidden=True)
    cases = [(x0, r, m, True) for (x0, r), m in zip(QUADS[2:], (False, True))]
    pts, counts, idx, st = quad_strip(cases)
    left = mesh("l", pts, counts, idx, st=st, normals=[(0, 0, 1)] * 8, hidden=True, left=True)
    highs = [plane("h%d" % k, x0, 0.2, 0.3, 0.2) for k, (x0, _) in enumerate(QUADS)]
    write(out, "frames", [xform("low", [right, left])] + highs, [
        settings("mikkt", "/low", ["Nt", "Vdt"], (16, 16), 16),
        settings("uvframe", "/low", ["Nt", "Vdt"], (16, 16), 16, {"bake_use_mikkt": 0})])


def slab_scene(out):
    pts, counts, idx, st = quad_strip([(100.0 * k, r, False, False) for k, (_, r) in enumerate(QUADS)])
    low = xform("low", [mesh("mesh", pts, counts, idx, st=st, normals=[(0, 0, 1)] * 16, hidden=True)])
    H = [0.012, 0.022, 0.032, 0.042]
    slabs = [box("slab%d" % k, (100.0 * k - 30, -30, 0.002 - H[k]), (100.0 * k + 31, 31, 0.002)) for k in range(4)]
    write(out, "slabs", [low] + slabs, [
        settings("slabs", "/low/mesh", ["Cv", "Cu", "Th", "Oc"], (32, 32), 16,
                 {"bake_cv_samples": 64, "bake_cu_samples": 64, "bake_th_samples": 16}),
        settings("slabs_b5", "/low/mesh", ["Cu", "Cv"], (32, 32), 16,
                 {"bake_cu_samples": 64, "bake_cu_bias": 0.5, "bake_cu_sdist": 0.05, "bake_cv_bias": 0.5,
                  "bake_cv_samples": 64})])
    G = [0.005, 0.01, 0.015, 0.02]
    ceil = []
    for k in range(4):
        x0 = 100.0 * k
        ceil.append(box("floor%d" % k, (x0 - 30, -30, -5), (x0 + 31, 31, 0.002)))
        ceil.append(box("ceil%d" % k, (x0 - 30, -30, 0.002 + G[k]), (x0 + 31, 31, 5)))
    write(out, "ceilings", [low] + ceil, [
        settings("ceilings", "/low/mesh", ["Cu", "Oc"], (32, 32), 16,
                 {"bake_cu_samples": 64, "bake_oc_maxdist": 0.05, "bake_oc_samples": 64})])


def edge_scene(out):
    zl = 0.9995
    pts = [(0.97, 0.4, zl), (1.0, 0.4, zl), (1.0, 0.6, zl), (0.97, 0.6, zl),
           (9.97, 0.4, -0.0005), (10.0, 0.4, -0.0005), (10.0, 0.6, -0.0005), (9.97, 0.6, -0.0005)]
    st = [(0, 0), (1, 0), (1, 0.4), (0, 0.4), (0, 0.6), (1, 0.6), (1, 1), (0, 1)]  # a gap between the strips
    low = xform("low", [mesh("mesh", pts, [4, 4], list(range(8)), st=st, normals=[(0, 0, 1)] * 8, hidden=True)])
    s2 = math.sqrt(0.5)
    cv = [(-4, -5, 1), (1, -5, 1), (1, 5, 1), (-4, 5, 1), (2, -5, 0), (2, 5, 0)]
    convex = xform("convex", [mesh("mesh", cv, [4, 4], [0, 1, 2, 3, 1, 4, 5, 2], normals=[(0, 0, 1)] * 4 + [(s2, 0, s2)] * 4)])
    cc = [(5, -5, 0), (10, -5, 0), (10, 5, 0), (5, 5, 0), (11, -5, 1), (11, 5, 1)]
    concave = xform("concave", [mesh("mesh", cc, [4, 4], [0, 1, 2, 3, 1, 4, 5, 2], normals=[(0, 0, 1)] * 4 + [(-s2, 0, s2)] * 4)])
    write(out, "edges", [low, convex, concave], [
        settings("edges", "/low/mesh", ["Eg", "Rn"], (64, 16), 16, {"bake_eg_samples": 64, "bake_rn_samples": 64})])


def pbox_scene(out):
    pts, counts, idx, st = quad_strip([(x0, r, False, False) for x0, r in QUADS])
    low = xform("low", [mesh("mesh", pts, counts, idx, st=st, normals=[(0, 0, 1)] * 16, hidden=True)])
    highs = [plane("h%d" % k, x0, 0.2) for k, (x0, _) in enumerate(QUADS)]
    s = []
    for mode in ("bakemesh", "scene", "user"):
        for uni in (1, 0):
            s.append(settings("pbox_%s_%d" % (mode, uni), "/low/mesh", ["P"], (16, 16), 16,
                              {"bake_pbox_mode": mode, "bake_pbox_uniform": uni, "bake_pbox_min": (-2, -1, 0),
                               "bake_pbox_max": (6, 3, 1)}))
    write(out, "pbox", [low] + highs, s)


def bumps(s, t):
    return 0.1 + 0.2 * math.sin(6 * s) * math.cos(5 * t)


def grid(nx, ny, f):
    pts, st, nrm, counts, idx = [], [], [], [], []
    for j in range(ny):
        for i in range(nx):
            for a, b in [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]:
                s, t = a / nx, b / ny
                pts.append(f(s, t))
                e = 1e-4
                du = [p - q for p, q in zip(f(s + e, t), f(s - e, t))]
                dv = [p - q for p, q in zip(f(s, t + e), f(s, t - e))]
                n = (du[1] * dv[2] - du[2] * dv[1], du[2] * dv[0] - du[0] * dv[2], du[0] * dv[1] - du[1] * dv[0])
                l = math.sqrt(sum(c * c for c in n))
                nrm.append(tuple(c / l for c in n))
                st.append((s, t))
            counts.append(4)
            idx += list(range(len(pts) - 4, len(pts)))
    return pts, counts, idx, st, nrm


def filter_scene(out):
    pts = [(0.3, 0.3, 0), (0.7, 0.3, 0), (0.3, 0.7, 0)]
    low = xform("low", [mesh("mesh", pts, [3], [0, 1, 2], st=[(0.3, 0.3), (0.7, 0.3), (0.3, 0.7)],
                             normals=[(0, 0, 1)] * 3, hidden=True)])
    p, c, i, _, n = grid(48, 48, lambda s, t: (s, t, bumps(s, t)))
    high = xform("high", [mesh("mesh", p, c, i, normals=n)])
    filt = [["shift", {"scale": [0.5, 0.5, 0.5], "offset": [0.5, 0.5, 0.5], "skipbackgroundcolor": 0.0,
                       "ispremultipliedalpha": True, "runlast": True, "aovs": ["Nt"]}],
            ["uv_enlarger", {"scheme": "diffusefill", "borderpixels": 4, "alphaname": "Af"}]]
    write(out, "filters", [low, high], [
        settings("raw", "/low/mesh", ["Nt", "Af"], (32, 32), 64),
        settings("filtered", "/low/mesh", ["Nt", "Af"], (32, 32), 64, imagefilter=json.dumps(filt))])


def torus(nu, nv, offset_fn, R=1.0, r=0.4):
    """A torus around Y (major radius R, minor r), u around the axis and v
    around the tube, both in [0, 1]; the surface moved along its normal by
    offset_fn(u angle, v angle). Smooth normals by central differences.
    No degenerate faces (a lat-long sphere's poles bake differently in
    Karma, whose quads collapse there)."""
    pts, st, nrm, counts, idx = [], [], [], [], []

    def point(i, j):
        phi, theta = 2 * math.pi * i / nu, 2 * math.pi * j / nv
        d = (math.cos(theta) * math.cos(phi), math.sin(theta), -math.cos(theta) * math.sin(phi))
        c = (R * math.cos(phi), 0.0, -R * math.sin(phi))
        h = r + offset_fn(phi, theta)
        return tuple(cc + h * dd for cc, dd in zip(c, d)), d

    for j in range(nv):
        for i in range(nu):
            for a, b in [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]:
                p, d = point(a, b)
                pts.append(p)
                # normals by central differences of the radius function
                e = 1e-4
                pu = [x - y for x, y in zip(point(a + e, b)[0], point(a - e, b)[0])]
                pv = [x - y for x, y in zip(point(a, b + e)[0], point(a, b - e)[0])]
                n = (pu[1] * pv[2] - pu[2] * pv[1], pu[2] * pv[0] - pu[0] * pv[2], pu[0] * pv[1] - pu[1] * pv[0])
                l = math.sqrt(sum(c * c for c in n))
                nrm.append(tuple(c / l for c in n) if l > 1e-12 else d)
                st.append((a / nu, b / nv))
            counts.append(4)
            idx += list(range(len(pts) - 4, len(pts)))
    return pts, counts, idx, st, nrm


def asset_scene(out):
    p, c, i, st, n = torus(24, 12, lambda ph, th: 0.0)
    low = xform("low", [mesh("mesh", p, c, i, st=st, normals=n, hidden=True)])
    p, c, i, _, n = torus(144, 72, lambda ph, th: 0.03 * math.sin(7 * ph) * math.sin(3 * th))
    high = xform("high", [mesh("mesh", p, c, i, normals=n)])
    v = ["Nt", "Oc", "Cu", "Cv", "Th", "Ds", "Af", "base_color"]
    write(out, "asset", [low, high], [
        settings("asset", "/low/mesh", v, (64, 64), 16, {"bake_cu_sdist": 0.1})])


def main_into(out):
    os.makedirs(out, exist_ok=True)
    for f in (trace_scene, frames_scene, slab_scene, edge_scene, pbox_scene, filter_scene, asset_scene):
        f(out)


def main():
    main_into(os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd())


if __name__ == "__main__":
    main()
