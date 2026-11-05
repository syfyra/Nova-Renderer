"""M5 spike, part 3 (hython): evaluate the CVEX-compiled builder bsdf."""
import math
import os
import sys

import hou

D = sys.argv[1]
vex = os.path.join(D, sys.argv[2] if len(sys.argv) > 2 else 'builder_cvex5.vex')
n = 8192
PT = hou.Vector3(0.37, 0.11, 0.05)


def sphere_dirs(count):
    out = []
    for i in range(count):
        z = 2.0 * (i + 0.5) / count - 1.0
        v = (i * 0.618034) % 1.0
        r = math.sqrt(max(0.0, 1 - z * z))
        out.append(hou.Vector3(r * math.cos(2 * math.pi * v), r * math.sin(2 * math.pi * v), z))
    return out


for label, view in (('normal view', hou.Vector3(0, 0, -1)), ('60 deg view', hou.Vector3(math.sin(math.radians(60)), 0, -math.cos(math.radians(60))))):
    wis = sphere_dirs(n)
    res = hou.runVex(vex, {'nova_wi': wis, 'I': [view] * n, 'P': [PT] * n})
    tot = [sum(e[c] for e in res['nova_eval']) / n * 4 * math.pi / math.pi for c in range(3)]
    print('%s: integral eval / pi = %s; albedo() = %s; Of = %s' % (label, tuple(round(t, 4) for t in tot),
          tuple(round(x, 4) for x in res['nova_albedo'][0]), str(res['Of'])[:60]))
    sx = [((i * 0.7548777) % 1.0) for i in range(n)]
    sy = [((i * 0.5698403) % 1.0) for i in range(n)]
    res = hou.runVex(vex, {'nova_sx': sx, 'nova_sy': sy, 'I': [view] * n, 'P': [PT] * n})
    dirs = res['nova_sdir']
    res2 = hou.runVex(vex, {'nova_wi': dirs, 'I': [view] * n, 'P': [PT] * n})
    acc = [0.0, 0.0, 0.0]
    zero = 0
    for k in range(n):
        p = res2['nova_pdf'][k]
        if p > 0:
            for c in range(3):
                acc[c] += res2['nova_eval'][k][c] / p
        else:
            zero += 1
    print('   E[eval/pdf] over sampled dirs = %s (%d zero-pdf); mean sample eval = %s; types %s' % (
        tuple(round(a / n, 4) for a in acc), zero,
        tuple(round(sum(e[c] for e in res['nova_seval']) / n, 4) for c in range(3)),
        sorted(set(res['nova_stype']))))
# Spatial variation from the noise pattern: albedo at a few P.
res = hou.runVex(vex, {'P': [hou.Vector3(0.37 * k, 0.11 * k, 0.05 * k) for k in range(6)]})
print('albedo vs P:', [tuple(round(x, 3) for x in a) for a in res['nova_albedo']])
# Timing: 32k lanes.
import time
m = 32768
args = {'nova_wi': sphere_dirs(m), 'P': [hou.Vector3((k % 97) * 0.01, (k % 89) * 0.01, 0) for k in range(m)]}
t0 = time.perf_counter()
hou.runVex(vex, args)
print('runVex 32768 lanes: %.1f ms' % ((time.perf_counter() - t0) * 1000))
# The diffuse colour at PT: eval at normal incidence for light along N, view along N.
res = hou.runVex(vex, {'nova_wi': [hou.Vector3(0, 0, 1)], 'P': [PT]})
print('eval(N, N) at PT:', tuple(round(x, 4) for x in res['nova_eval'][0]), 'pdf', round(res['nova_pdf'][0], 4))
