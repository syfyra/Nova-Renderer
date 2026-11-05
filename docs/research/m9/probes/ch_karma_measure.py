# M9 research probe (curves-hair.md): reads the EXRs rendered by
# ch_karma_renders.ps1 (multi-part: one subimage per RenderVar) and prints the
# measurements used in the report. Runs with Houdini's Python (numpy, OIIO):
#
#   & "$HFS\python313\python.exe" -I docs\research\m9\probes\ch_karma_measure.py <scenes dir> [group ...]
#
# Groups: geo, fur, dl, sweep, ball (default: all).
import json
import math
import os
import sys

os.add_dll_directory(os.path.join(os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466'), 'bin'))
import numpy as np  # noqa: E402
import OpenImageIO as oiio  # noqa: E402

d = os.path.abspath(sys.argv[1])
groups = sys.argv[2:] or ['geo', 'fur', 'dl', 'sweep', 'ball']


def load(name):
    """{aov name: HxWxC float array} for every part of <name>.exr."""
    p = os.path.join(d, name + '.exr')
    if not os.path.exists(p):
        return None
    inp = oiio.ImageInput.open(p)
    out = {}
    sub = 0
    while inp.seek_subimage(sub, 0):
        spec = inp.spec()
        px = inp.read_image(sub, 0, 0, spec.nchannels, oiio.FLOAT)
        nm = spec.getattribute('name') or ('part%d' % sub)
        out[nm] = np.asarray(px).reshape(spec.height, spec.width, spec.nchannels)
        sub += 1
    inp.close()
    return out


def f3(v):
    return '(' + ', '.join('%.4f' % x for x in v) + ')'


# --------------------------------------------------------------------- geometry
def geo():
    print('=== geo_width (800x200 over 4x1 units, pixel 0.005) ===')
    im = load('geo_width')
    if im is not None:
        a = im['C'][:, :, 3]
        px = 0.005
        rows = slice(40, 160)

        def col(x):
            return int(round((x + 2.0) / px))
        for name, x, w in (('lin_w02', -1.6, 0.2), ('lin_w0025', -1.1, 0.0025), ('lin_w0005', -0.8, 0.0005),
                           ('bsp_w02', -0.4, 0.2), ('lin_w02_scale2', 1.0, 0.2)):
            c = col(x)
            hw = int(w * (2 if 'scale2' in name else 1) / 2 / px) + 6
            band = a[rows, max(c - hw, 0):c + hw + 1]
            width = band.sum(axis=1).mean() * px
            print('  %-16s authored %.4f  projected width %.5f  ratio %.3f  (row std %.5f)' % (
                name, w, width, width / w, band.sum(axis=1).std() * px))
        c = col(0.3)
        area = a[:, c - 40:c + 40].sum() * px * px
        print('  point w0.2: alpha area %.5f; disc of diameter 0.2: %.5f; of radius 0.2: %.5f' % (
            area, math.pi * 0.01, math.pi * 0.04))
        c = col(1.6)
        # widths 0.1 -> 0.3 over y in [-2, 2]: at image row r, y = 0.5 - (r + 0.5) * px
        for r in (20, 100, 180):
            y = 0.5 - (r + 0.5) * px
            print('  lin_vw row %d (y=%.3f): projected %.4f, linear width %.4f' % (
                r, y, a[r, c - 70:c + 70].sum() * px, 0.1 + 0.2 * (y + 2) / 4))
        uv = im.get('uv')
        if uv is not None:
            c = col(-1.6)
            r = 100
            print('  hituv across lin_w02 at row %d (x offsets -0.09..0.09):' % r,
                  [f3(uv[r, c + k]) for k in (-18, -9, 0, 9, 18)])
            print('  hituv along lin_w02 (rows 0, 100, 199):', [f3(uv[rr, c]) for rr in (0, 100, 199)])
            pr = im.get('primid')
            el = im.get('element')
            print('  primid / element at the centre:', pr[r, c] if pr is not None else None, el[r, c] if el is not None else None)

    print('=== geo_caps (200x200 over [-0.25,0.25]^2, pixel 0.0025; ends at y = 0, width 0.1) ===')
    im = load('geo_caps')
    if im is not None:
        a = im['C'][:, :, 3]
        px = 0.0025
        # image row r has y = 0.25 - (r + 0.5) px; y > 0 for r < 100
        for name, x in (('lin_end', -0.12), ('bsp_end', 0.12)):
            c = int(round((x + 0.25) / px))
            beyond = a[:100, c - 30:c + 30].sum() * px * px
            prof = a[:100, c].sum() * px
            print('  %-8s coverage beyond the end point: area %.6f (hemisphere cap %.6f, square cap %.6f); '
                  'centre-line extent %.4f (radius 0.05)' % (name, beyond, math.pi * 0.05 ** 2 / 2, 0.1 * 0.05, prof))
        uv = im.get('uv')
        if uv is not None:
            c = int(round((-0.12 + 0.25) / px))
            print('  hituv lin_end at rows 150, 102, 98 (y=-0.126, -0.006, +0.004):', [f3(uv[r, c]) for r in (150, 102, 98)])
            N = im.get('N')
            print('  N lin_end at rows 98, 90:', [f3(N[r, c]) for r in (98, 90)])

    print('=== geo_ribbon (normals tilted 60 deg; projected width of width 0.2) ===')
    im = load('geo_ribbon')
    if im is not None:
        a = im['C'][:, :, 3]
        px = 0.005
        names = ['bsp_style_unset', 'bsp_style_0', 'bsp_style_1', 'bsp_style_2', 'bsp_style_3', 'lin_style_2']
        for i, nm in enumerate(names):
            x = -2.5 + i
            c = int(round((x + 3.0) / px))
            band = a[20:80, c - 60:c + 60]
            w = band.sum(axis=1).mean() * px
            N = im['N'][50, c - 60:c + 60]
            cov = a[50, c - 60:c + 60] > 0.99
            ns = N[cov]
            print('  %-16s projected %.4f (tube 0.2, ribbon 0.2 cos60 = 0.1)  N at left/centre/right: %s %s %s' % (
                nm, w, f3(ns[0]) if len(ns) else '-', f3(ns[len(ns) // 2]) if len(ns) else '-',
                f3(ns[-1]) if len(ns) else '-'))

    print('=== geo_shade (distant light from the camera, irradiance 1; default material) ===')
    im = load('geo_shade')
    if im is not None:
        C = im['C']
        px = 0.005
        for nm, x, w in (('tube_nocolor', -1.5, 0.4), ('tube_white', -0.5, 0.4), ('flat_white', 0.5, 0.4),
                         ('quad_white', 1.5, 0.4)):
            c = int(round((x + 2.0) / px))
            half = int(round(w / 2 / px))
            band = C[50:150, c - half - 3:c + half + 3, :]
            alpha = band[:, :, 3].sum()
            mean = band[:, :, :3].sum(axis=(0, 1)) / max(alpha, 1e-9)
            prof = C[100, c - half:c + half, 0]
            xs = (np.arange(-half, half) + 0.5) / half  # offset / radius
            lam = np.sqrt(np.clip(1 - xs ** 2, 0, 1)) / math.pi
            print('  %-13s mean radiance over coverage %s; centre %.4f; profile/(cos/pi) at |x|/r = 0, 0.5, 0.9: %s' % (
                nm, f3(mean), C[100, c, 0], ', '.join('%.3f' % (prof[k] / lam[k]) for k in
                                                       (half, half + half // 2, half + int(0.9 * half)))))
        N = im['N']
        c = int(round((-0.5 + 2.0) / px))
        print('  tube N across (x/r = -0.9, -0.5, 0, 0.5, 0.9):',
              [f3(N[100, c + int(k * 40)]) for k in (-0.9, -0.5, 0, 0.5, 0.9)])
        Ng = im['Ng']
        print('  tube Ng across:', [f3(Ng[100, c + int(k * 40)]) for k in (-0.9, 0, 0.9)])
        uv = im['uv']
        print('  tube hituv across:', [f3(uv[100, c + int(k * 40)]) for k in (-0.9, -0.5, 0, 0.5, 0.9)])
        P = im['P']
        print('  tube P across:', [f3(P[100, c + int(k * 40)]) for k in (-0.9, 0, 0.9)])
        c = int(round((0.5 + 2.0) / px))
        print('  flat N across:', [f3(N[100, c + int(k * 40)]) for k in (-0.9, 0, 0.9)],
              'hituv', [f3(uv[100, c + int(k * 40)]) for k in (-0.9, 0, 0.9)])

    print('=== geo_shade_side (light from 60 deg to the side): diffuse/glossy split of the default material ===')
    im = load('geo_shade_side')
    if im is not None:
        C, RD, RG, N, uv = im['C'], im['RD'], im['RG'], im['N'], im['uv']
        px = 0.005
        L = np.array([math.sin(math.radians(60)), 0, 0.5])
        for nm, x in (('tube_white', -0.5), ('flat_white', 0.5), ('quad_white', 1.5)):
            c = int(round((x + 2) / px))
            for k in (-0.4, 0.0, 0.4, 0.8):
                cc = c + int(k * 40)
                lam = 0.5 * max(float(np.dot(N[150, cc], L)), 0.0) / math.pi
                print('  %-10s x/r %+.1f  C %.4f  RD %.4f  RG %.4f  Lambert(0.5) %.4f  RD/Lambert %.3f' % (
                    nm, k, C[150, cc, 0], RD[150, cc, 0], RG[150, cc, 0], lam, RD[150, cc, 0] / max(lam, 1e-9)))

        def col(xx):
            return int(round((xx + 2) / px))
        r = int((0.5 - 0.4) / px)
        print('  lin3 (3 segments, x = -1.9 -1.1 -0.9 1.9) hituv at x = -1.8, -1.0, -0.8, 0.1, 1.0:',
              [f3(uv[r, col(xx)]) for xx in (-1.8, -1.0, -0.8, 0.1, 1.0)])
        r = int((0.5 - 0.47) / px)
        print('  bsp5 (2 segments, x from -0.95 to 0.95) hituv at x = -0.9, -0.1, 0.1, 0.9:',
              [f3(uv[r, col(xx)]) for xx in (-0.9, -0.1, 0.1, 0.9)])

    print('=== geo_ids: three curves in one prim; a NurbsCurves prim ===')
    im = load('geo_ids')
    if im is not None:
        a = im['C'][:, :, 3]
        px = 0.005
        for x in (-1.5, -1.0, -0.5):
            for y in (-0.3, 0.2):
                rr, cc = int(round((0.5 - y) / px)), int(round((x + 2) / px))
                print('  curve at x=%.1f y=%+.1f: primid %d element %d hituv %s' % (
                    x, y, im['primid'][rr, cc, 0], im['element'][rr, cc, 0], f3(im['uv'][rr, cc])))
        c0 = int(round(2.0 / px))
        CPn = np.array([(0.1, -0.4), (0.35, 0.9), (1.15, -0.9), (1.6, 0.4)])
        t = np.linspace(0, 1, 1000)[:, None]
        ys, xs = np.nonzero(a[:, c0:] > 0.5)
        Px, Py = -2 + (xs + c0 + 0.5) * px, 0.5 - (ys + 0.5) * px
        for nm, cv in (('control hull (linear)', np.vstack([CPn[i] + (CPn[i + 1] - CPn[i]) * t for i in range(3)])),
                       ('Bezier (the exact NURBS)', bez(CPn, t))):
            dmax = np.min(np.hypot(Px[:, None] - cv[None, ::2, 0], Py[:, None] - cv[None, ::2, 1]), axis=1).max()
            ins = np.abs(cv[:, 1]) < 0.49
            cov = a[((0.5 - cv[ins, 1]) / px).astype(int), ((cv[ins, 0] + 2) / px).astype(int)] > 0.5
            print('  NurbsCurves vs %-24s: max distance of covered pixels %.4f, in-frame curve covered %.3f' % (
                nm, dmax, cov.mean()))

    print('=== cubic bases: rendered coverage vs candidate curves (width 0.02; distance bound 0.0114) ===')
    CP = np.array([(-0.9, -0.4), (-0.4, 0.9), (0.4, -0.9), (0.9, 0.4)])
    t = np.linspace(0, 1, 1500)[:, None]

    def spl(P, f):
        return np.vstack([f(P[i:i + 4], t) for i in range(len(P) - 3)])
    refl = np.vstack([2 * CP[0] - CP[1], CP, 2 * CP[-1] - CP[-2]])
    cands = {
        'geo_basis_bezier_nonperiodic': {'exact': bez(CP, t)},
        'geo_basis_bspline_nonperiodic': {'exact': spl(CP, bsp)},
        'geo_basis_bspline_pinned': {'phantom 2P0-P1 (USD)': spl(refl, bsp),
                                     'ends repeated twice (Hdsi)': spl(np.vstack([CP[0], CP[0], CP, CP[-1], CP[-1]]), bsp)},
        'geo_basis_catmullRom_pinned': {'phantom 2P0-P1 (USD)': spl(refl, crom),
                                        'ends repeated once (Hdsi)': spl(np.vstack([CP[0], CP, CP[-1]]), crom)},
        'geo_basis_bezier_wave': {'exact': exact_curve('wave', CP)},
    }
    for name, hyps in cands.items():
        im = load(name)
        if im is None:
            continue
        a = im['C'][:, :, 3]
        H, W = a.shape
        px = 2.0 / W
        ys, xs = np.nonzero(a > 0.5)
        Px = -1.0 + (xs + 0.5) * px
        Py = 0.5 - (ys + 0.5) * px
        for hn, cv in hyps.items():
            dist = np.min(np.hypot(Px[:, None] - cv[None, ::2, 0], Py[:, None] - cv[None, ::2, 1]), axis=1)
            ins = np.abs(cv[:, 1]) < 0.49
            cov = a[((0.5 - cv[ins, 1]) / px).astype(int).clip(0, H - 1), ((cv[ins, 0] + 1) / px).astype(int).clip(0, W - 1)]
            print('  %-30s %-28s max distance %.4f  in-frame curve covered %.3f  alpha area %.5f  length*w %.5f' % (
                name, hn, dist.max(), (cov > 0.5).mean(), a.sum() * px * px,
                curve_length(cv[ins]) * (0.05 if 'wave' in name else 0.02)))


def exact_curve(name, CP, n=4000):
    t = np.linspace(0, 1, n)[:, None]
    if 'wave' in name:
        pts = []
        for k in range(4):
            x0 = -0.8 + 0.4 * k
            pts += [(x0, -0.3), (x0, 0.3), (x0 + 0.2, 0.3)] if k % 2 == 0 else [(x0, 0.3), (x0, -0.3), (x0 + 0.2, -0.3)]
        pts.append((0.8, 0.3 if len(pts) % 2 else -0.3))
        P = np.array(pts[:13])
        segs = []
        for s in range(4):
            q = P[3 * s:3 * s + 4]
            segs.append(bez(q, t))
        return np.vstack(segs)
    if 'bezier' in name:
        return bez(CP, t)
    if 'bspline' in name:
        P = CP
        if 'pinned' in name:
            P = np.vstack([CP[0], CP[0], CP, CP[-1], CP[-1]])
        segs = [bsp(P[i:i + 4], t) for i in range(len(P) - 3)]
        return np.vstack(segs)
    if 'catmull' in name:
        P = np.vstack([CP[0], CP, CP[-1]])  # pinned: end points repeated once
        segs = [crom(P[i:i + 4], t) for i in range(len(P) - 3)]
        return np.vstack(segs)


def bez(q, t):
    return ((1 - t) ** 3) * q[0] + 3 * t * (1 - t) ** 2 * q[1] + 3 * t * t * (1 - t) * q[2] + t ** 3 * q[3]


def bsp(q, t):
    b0 = (1 - t) ** 3 / 6
    b1 = (3 * t ** 3 - 6 * t ** 2 + 4) / 6
    b2 = (-3 * t ** 3 + 3 * t ** 2 + 3 * t + 1) / 6
    b3 = t ** 3 / 6
    return b0 * q[0] + b1 * q[1] + b2 * q[2] + b3 * q[3]


def crom(q, t):
    return 0.5 * ((2 * q[1]) + (-q[0] + q[2]) * t + (2 * q[0] - 5 * q[1] + 4 * q[2] - q[3]) * t ** 2 +
                  (-q[0] + 3 * q[1] - 3 * q[2] + q[3]) * t ** 3)


def curve_length(c):
    return np.hypot(*np.diff(c, axis=0).T).sum()


# ------------------------------------------------------------------------ hair
def fiber_stats(im, aovs):
    """Mean radiance of every AOV over the fibre's coverage (alpha-weighted)."""
    a = im['C'][:, :, 3]
    alpha = a.sum()
    out = {}
    for k in aovs:
        if k in im:
            out[k] = im[k][:, :, :3].sum(axis=(0, 1)) / alpha
    return out, a


def fur():
    print('=== fibre in a white furnace (dome 1; limits 64): mean radiance over the fibre = directional albedo ===')
    res = {}
    for mat in ('mx_s0', 'mx_sa', 'kma_s0', 'kma_sa', 'kma_def', 'kma_m08', 'kma_base', 'kma_diffuse'):
        for a in (0, 30, 60):
            nm = 'fur_%s_a%d' % (mat, a)
            im = load(nm)
            if im is None:
                continue
            st, al = fiber_stats(im, ['C', 'RD', 'RG', 'RS', 'TD', 'TG', 'TS', 'direct', 'indirect'])
            res[nm] = {k: v.tolist() for k, v in st.items()}
            prof = im['C'][20, 50:150, :3]
            print('  %-20s C %s | RD %s RG %s TD %s TG %s RS %s TS %s | direct %s indirect %s | alpha width %.4f' % (
                nm, f3(st['C']), f3(st['RD']), f3(st['RG']), f3(st['TD']), f3(st['TG']), f3(st['RS']), f3(st['TS']),
                f3(st['direct']), f3(st['indirect']), al[5:35].sum(axis=1).mean() * 0.005))
            if mat == 'mx_sa' and a == 0:
                print('     profile across (h = -0.9, -0.5, 0, 0.5, 0.9):',
                      [f3(prof[int(50 + 50 * h)]) for h in (-0.9, -0.5, 0.0, 0.5, 0.9)])
    with open(os.path.join(d, 'fur_results.json'), 'w') as f:
        json.dump(res, f, indent=1)


def dl():
    print('=== fibre under six distant lights (irradiance 1 each, LPE per light) ===')
    res = {}
    for mat in ('mx_sa', 'mx_sa_tilt', 'mx_sa_r2', 'mx_sa_ior', 'kma_sa', 'kma_def', 'kma_def_shift0', 'kma_rough',
                'kma_coat', 'kma_diffuse', 'kma_tilt'):
        for a in (0, 30):
            nm = 'dl_%s_a%d' % (mat, a)
            im = load(nm)
            if im is None:
                continue
            keys = ['C', 'l0', 'l1', 'l2', 'l3', 'l4', 'l5', 'RD', 'RG', 'TD', 'TG', 'RS', 'TS']
            st, al = fiber_stats(im, keys)
            res[nm] = {k: v.tolist() for k, v in st.items()}
            print('  %-22s C %s' % (nm, f3(st['C'])))
            print('      ' + '  '.join('%s %s' % (k, f3(st[k])) for k in ('l0', 'l1', 'l2', 'l3', 'l4', 'l5')))
            if 'RD' in st:
                print('      ' + '  '.join('%s %s' % (k, f3(st[k])) for k in ('RD', 'RG', 'RS', 'TD', 'TG', 'TS') if k in st))
    with open(os.path.join(d, 'dl_results.json'), 'w') as f:
        json.dump(res, f, indent=1)


def sweep():
    """Karma Hair melanin / thickness sweep and the IOR-1 furnaces (a0, dome 1)."""
    print('=== Karma Hair colour controls: furnace a0 (mean over the fibre) ===')
    keys = ('fur_kma_mel_', 'fur_kma_thick_', 'fur_kma_combo', 'fur_kma_base05', 'fur_kma_tint', 'fur_kma_mrand')
    names = sorted(set(n[:-4] for n in os.listdir(d) if n.endswith('.exr') and (n.startswith(keys) or 'ior1' in n)))
    res = {}
    for nm in names:
        if not nm.endswith('_a0'):
            continue
        im = load(nm)
        if im is None:
            continue
        st, al = fiber_stats(im, ['C', 'RG', 'TG', 'indirect'])
        res[nm] = st['C'].tolist()
        print('  %-26s C %s  RG %s  TG %s  indirect %s' % (nm, f3(st['C']), f3(st['RG']), f3(st['TG']),
                                                            f3(st['indirect'])))
    with open(os.path.join(d, 'sweep_results.json'), 'w') as f:
        json.dump(res, f, indent=1)


def ball():
    print('=== hair ball (2000 fibres, kma_s0) in a white furnace, ray limits varied ===')
    for k in ('ball_high', 'ball_default', 'ball_refl0', 'ball_refr0', 'ball_diff0', 'ballmxs0_high',
              'ballmxs0_default', 'ballkmadef_high', 'ballkmadef_default'):
        im = load(k)
        if im is None:
            continue
        st, al = fiber_stats(im, ['C', 'RD', 'RG', 'RS', 'TD', 'TG', 'TS', 'direct', 'indirect'])
        print('  %-18s C %s | RD %s RG %s RS %s TD %s TG %s TS %s | direct %s indirect %s | coverage %.1f px' % (
            k, f3(st['C']), f3(st['RD']), f3(st['RG']), f3(st['RS']), f3(st['TD']), f3(st['TG']), f3(st['TS']),
            f3(st['direct']), f3(st['indirect']), al.sum()))
        full = al > 0.999
        print('           pixels with alpha 1: %d, their mean C %s' % (full.sum(), f3(im['C'][full][:, :3].mean(axis=0))))


for g in groups:
    globals()[g]()
