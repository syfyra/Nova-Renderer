# M9 research probe (docs/research/m9/sss.md): measures the renders of
# sss_renders.ps1. Reads every part of husk's multi-part EXRs with
# OpenImageIO and prints, per scene and AOV, the statistic scenes.json asks
# for (disc mean, all-pixel mean, mu bins, x profile).
#
#   "C:\Program Files\Side Effects Software\Houdini 22.0.466\python313\python.exe" sss_measure.py <outdir> [renderer ...]
#
# Prints a text table and writes <outdir>/<renderer>/measure.json.
import json
import math
import os
import sys

os.add_dll_directory(r'C:\Program Files\Side Effects Software\Houdini 22.0.466\bin')
import OpenImageIO as oiio  # noqa: E402

outdir = os.path.abspath(sys.argv[1])
renderers = sys.argv[2:] or ['BRAY_HdKarma']
scenes = json.load(open(os.path.join(outdir, 'scenes.json'), encoding='utf-8'))


def read_parts(path):
    inp = oiio.ImageInput.open(path)
    if not inp:
        return None
    parts = {}
    sub = 0
    while inp.seek_subimage(sub, 0):
        spec = inp.spec()
        px = inp.read_image(sub, 0, 0, spec.nchannels, oiio.FLOAT)
        name = spec.get_string_attribute('name') or str(sub)
        parts[name] = (spec.width, spec.height, spec.nchannels, list(spec.channelnames), px)
        sub += 1
    inp.close()
    return parts


def stats(vals):
    n = len(vals)
    if n == 0:
        return (float('nan'), float('nan'), 0)
    m = sum(vals) / n
    v = sum((x - m) ** 2 for x in vals) / max(1, n - 1)
    return (m, math.sqrt(v / n), n)


def measure(meas, w, h, px):
    kind = meas.get('kind', 'all')
    width = meas.get('width', 1.0)
    height = width * h / w
    out = {}
    pix = []  # (x, y, rgb)
    for j in range(h):
        for i in range(w):
            x = (i + 0.5) / w * width - width / 2
            y = height / 2 - (j + 0.5) / h * height
            p = px[j][i]
            pix.append((x, y, (float(p[0]), float(p[1]), float(p[2]))))
    if kind in ('disc', 'all', 'center'):
        if kind == 'disc':
            rmax = meas['inner'] * meas['radius']
            sel = [c for (x, y, c) in pix if x * x + y * y < rmax * rmax]
        else:
            sel = [c for (_, _, c) in pix]
        out['mean'] = [stats([c[k] for c in sel]) for k in range(3)]
    elif kind == 'mu':
        bins = {}
        for (x, y, c) in pix:
            r2 = x * x + y * y
            if r2 >= 0.98:
                continue
            mu = math.sqrt(1 - r2)
            b = min(9, int(mu * 10))
            bins.setdefault(b, []).append((mu, c))
        out['mu'] = {}
        for b in sorted(bins):
            lst = bins[b]
            out['mu']['%.2f' % (sum(m for m, _ in lst) / len(lst))] = [stats([c[k] for _, c in lst]) for k in range(3)]
    elif kind == 'xprofile':
        cols = {}
        for (x, y, c) in pix:
            cols.setdefault(round(x, 5), []).append(c)
        out['x'] = {('%.4f' % x): [stats([c[k] for c in v]) for k in range(3)] for x, v in sorted(cols.items())}
    return out


for rend in renderers:
    rdir = os.path.join(outdir, rend)
    result = {}
    print('#### renderer', rend)
    for name, meas in scenes.items():
        path = os.path.join(rdir, name + '.exr')
        if not os.path.exists(path):
            continue
        parts = read_parts(path)
        if parts is None:
            print(name, 'unreadable')
            continue
        result[name] = {}
        for pname, (w, h, nc, chans, px) in parts.items():
            result[name][pname] = measure(meas, w, h, px)
        # text summary
        print('== %s  %s' % (name, json.dumps(meas.get('params', {}))))
        for pname, r in result[name].items():
            if 'mean' in r:
                print('   %-10s %s' % (pname, '  '.join('%.4f+-%.4f' % (m, se) for (m, se, n) in r['mean'])))
            elif 'mu' in r:
                print('   %-10s mu: %s' % (pname, '  '.join('%s:(%s)' % (mu, ','.join('%.4f' % s[0] for s in v)) for mu, v in r['mu'].items())))
            elif 'x' in r:
                xs = list(r['x'].items())
                step = max(1, len(xs) // 16)
                print('   %-10s x: %s' % (pname, '  '.join('%s:(%s)' % (x, ','.join('%.4f' % s[0] for s in v)) for x, v in xs[::step])))
    with open(os.path.join(rdir, 'measure.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(result, f, indent=1)
