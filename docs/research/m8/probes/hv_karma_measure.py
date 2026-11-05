# M8 research probe (houdini-volumes.md): reads the EXRs rendered by
# hv_karma_renders.ps1 and prints the measurements used in the report.
# Runs with Houdini's Python (it ships numpy and OpenImageIO):
#
#   & "$env:HFS\python313\python.exe" -I docs\research\m8\probes\hv_karma_measure.py <outdir of hv_karma_scenes.py>
#
# Calibration scenes (80x80, orthographic, 20 px per unit): three unit cubes at
# x = -1.5, 0, 1.5 -> columns 3..17, 33..47, 63..77 and rows 33..47 (3 px margin).
import glob
import json
import math
import os
import re
import sys

os.add_dll_directory(os.path.join(os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466'), 'bin'))
import numpy as np  # noqa: E402
import OpenImageIO as oiio  # noqa: E402

d = os.path.abspath(sys.argv[1])


def load(name):
    p = os.path.join(d, name + '.exr')
    if not os.path.exists(p):
        return None
    buf = oiio.ImageBuf(p)
    return buf.get_pixels(oiio.FLOAT)


def wall(name):
    p = os.path.join(d, name + '.log')
    if not os.path.exists(p):
        return None
    m = re.findall(r'Total Wall Clock Time: (\d+):(\d+):([\d.]+)', open(p, encoding='utf-8-sig', errors='replace').read())
    if not m:
        return None
    h, mi, s = m[-1]
    return int(h) * 3600 + int(mi) * 60 + float(s)


COLS = {'left': slice(3, 18), 'mid': slice(33, 48), 'right': slice(63, 78)}
ROWS = slice(33, 48)


def boxes(img):
    out = {}
    for k, c in COLS.items():
        reg = img[ROWS, c, :]
        out[k] = (reg.mean(axis=(0, 1)), reg.std(axis=(0, 1)))
    return out


def fmt(v):
    return '(' + ', '.join('%.4f' % x for x in v) + ')'


def mx_blackbody(T):
    # MaterialX 1.39 mx_blackbody (Houdini 22 library): Kang et al. Planckian
    # locus fit -> CIE xyY with Y = 1 -> linear Rec.709.
    T = min(max(T, 800.0), 25000.0)
    t = 1000.0 / T
    if T < 4000.0:
        xc = -0.2661239 * t ** 3 - 0.2343580 * t ** 2 + 0.8776956 * t + 0.179910
    else:
        xc = -3.0258469 * t ** 3 + 2.1070379 * t ** 2 + 0.2226347 * t + 0.240390
    if T < 2222.0:
        yc = -1.1063814 * xc ** 3 - 1.34811020 * xc ** 2 + 2.18555832 * xc - 0.20219683
    elif T < 4000.0:
        yc = -0.9549476 * xc ** 3 - 1.37418593 * xc ** 2 + 2.09137015 * xc - 0.16748867
    else:
        yc = 3.0817580 * xc ** 3 - 5.87338670 * xc ** 2 + 3.75112997 * xc - 0.37001483
    X, Y, Z = xc / yc, 1.0, (1.0 - xc - yc) / yc
    r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z
    g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z
    b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z
    return np.maximum(np.array([r, g, b]), 0.0)


times = {}
for f in glob.glob(os.path.join(d, 'times_*.json')):
    times.update(json.load(open(f)))

print('== calibration (bare = no material | pyro = Karma Pyro Material defaults | mtlx = ND_volume + anisotropic_vdf(scattering = density))')
for name in ('cal_alpha', 'cal_furnace_vl0', 'cal_furnace_vl64'):
    img = load(name)
    if img is None:
        continue
    print('-- %s  render %.2f s' % (name, wall(name) or -1))
    for k, lab in (('left', 'bare'), ('mid', 'pyro'), ('right', 'mtlx')):
        m, s = boxes(img)[k]
        extra = ''
        if name == 'cal_alpha' and m[3] < 1.0:
            extra = '  -> sigma_t = -ln(1 - A) = %.4f per unit density' % (-math.log(max(1e-9, 1.0 - m[3])))
        print('   %-5s RGBA mean %s std %s%s' % (lab, fmt(m), fmt(s), extra))
    bg = img[2:8, 20:28, :].mean(axis=(0, 1))
    print('   background %s' % fmt(bg))

print('== emission cubes (density 0.001, temperature 0.5 | 1 | 2, one unit deep)')
for name in ('cal_emit_ramp', 'cal_emit_sr1', 'cal_emit_sr005', 'cal_emit_bb', 'cal_emit_smokeoff', 'cal_emit_bare'):
    img = load(name)
    if img is None:
        continue
    print('-- %s  render %.2f s' % (name, wall(name) or -1))
    for k, T in (('left', 0.5), ('mid', 1.0), ('right', 2.0)):
        m, s = boxes(img)[k]
        bb = mx_blackbody(T * 2500.0)
        print('   T=%.1f (%5.0f K) RGB mean %s std %s | mx_blackbody %s ratio %s' % (
            T, T * 2500.0, fmt(m[:3]), fmt(s[:3]), fmt(bb), fmt(m[:3] / np.maximum(bb, 1e-6))))

img = load('cal_emit_nodensity')
if img is not None:
    print('-- cal_emit_nodensity  render %.2f s (fire on, temperature 1)' % (wall('cal_emit_nodensity') or -1))
    for k, lab in (('left', 'no density field'), ('mid', 'no density, sampling field = temperature'), ('right', 'density 0.001 (reference)')):
        m, s = boxes(img)[k]
        print('   %-42s RGBA mean %s' % (lab, fmt(m)))

print('== single scattering, normalized distant light (irradiance 1) from straight above, volume limit 0')
for name, g in (('cal_distant', 0.0), ('cal_distant_g05', 0.5)):
    img = load(name)
    if img is None:
        continue
    rows = np.arange(80)[ROWS]
    y = 2.0 - (rows + 0.5) * 0.05
    for k, lab in (('left', 'bare'), ('mid', 'pyro'), ('right', 'mtlx')):
        gg = 0.0 if lab == 'bare' else g
        p = (1 - gg * gg) / (4 * math.pi * (1 + gg * gg) ** 1.5)  # HG at 90 degrees
        expect = (p * np.exp(-(0.5 - y)) * (1 - math.exp(-1.0))).mean()
        m, s = boxes(img)[k]
        print('   %-15s %-5s g=%.1f mean R %.5f (std %.5f) expected single-scatter %.5f ratio %.4f  render %.2f s' % (
            name, lab, gg, m[0], s[0], expect, m[0] / expect, wall(name) or -1))


def cie_fit(lam):
    # Wyman, Sloan, Shirley 2013 multi-lobe fit of the CIE 1931 2-degree CMFs.
    def g(x, mu, s1, s2):
        s = np.where(x < mu, s1, s2)
        return np.exp(-0.5 * ((x - mu) / s) ** 2)
    X = 1.056 * g(lam, 599.8, 37.9, 31.0) + 0.362 * g(lam, 442.0, 16.0, 26.7) - 0.065 * g(lam, 501.1, 20.4, 26.2)
    Y = 0.821 * g(lam, 568.8, 46.9, 40.5) + 0.286 * g(lam, 530.9, 16.3, 31.1)
    Z = 1.217 * g(lam, 437.0, 11.8, 36.0) + 0.681 * g(lam, 459.0, 26.0, 13.8)
    return X, Y, Z


def planck_xyz(T):
    lam = np.arange(360.0, 831.0, 1.0)
    l = lam * 1e-9
    B = 2 * 6.62607015e-34 * 2.99792458e8 ** 2 / l ** 5 / (np.exp(6.62607015e-34 * 2.99792458e8 / (l * 1.380649e-23 * T)) - 1)
    X, Y, Z = cie_fit(lam)
    return np.array([(B * X).sum(), (B * Y).sum(), (B * Z).sum()])


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


sweep = json.load(open(os.path.join(d, 'sweep.json'))) if os.path.exists(os.path.join(d, 'sweep.json')) else []
for name in ('cal_bb_mtlx', 'cal_bb_pyro'):
    img = load(name)
    if img is None or not sweep:
        continue
    print('-- %s  render %.2f s  (Y = Rec.709 luminance; Y_planck = photometric luminance of a Planck radiator, CIE fit)' % (name, wall(name) or -1))
    res = []
    for i, t in enumerate(sweep):
        cx = int(round((-5.25 + 1.5 * i + 6.0) * 20))
        reg = img[13:28, cx - 7:cx + 8, :3].reshape(-1, 3).mean(axis=0)
        T = t * 2500.0
        res.append((T, reg, mx_blackbody(T), planck_xyz(T)[1]))
    ref = [r for r in res if abs(r[0] - 2500.0) < 1][0]
    for T, m, bb, Yp in res:
        print('   %6.0f K RGB %s Y %.5f | mx %s Y_mx %.4f | Y/Y_mx %.4f | (T/2500)^4 x Y(2500) %.5f | Y_planck x Y(2500)/Y_planck(2500) %.5f | rgb/|rgb| %s vs mx %s' % (
            T, fmt(m), lum(m), fmt(bb), lum(bb), lum(m) / lum(bb), (T / 2500.0) ** 4 * lum(ref[1]), Yp * lum(ref[1]) / ref[3],
            fmt(m / max(1e-12, m.sum())), fmt(bb / bb.sum())))

print('== pyro sim renders (200x300, Karma defaults unless noted)')
for name in ('pyro_bare', 'pyro_bare_vl4', 'pyro_mat', 'pyro_mat_vl4', 'pyro_fire', 'pyro_fire_bb', 'pyro_fire_vl4', 'pyro_fire_sr1', 'pyro_mtlx'):
    img = load(name)
    if img is None:
        continue
    rgb = img[..., :3]
    a = img[..., 3]
    lum = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]
    print('   %-15s render %6.2f s (husk total %6.2f s)  mean RGB %s max lum %.3f  mean alpha %.4f  covered px (A>0.01) %d' % (
        name, wall(name) or -1, times.get(name, -1), fmt(rgb.mean(axis=(0, 1))), lum.max(), a.mean(), int((a > 0.01).sum())))
