import os, sys
import hou
vex = os.path.join(sys.argv[1], 'builder_dbg.vex')
pts = [hou.Vector3(0.37 * k, 0.11 * k, 0.05 * k) for k in range(5)]
res = hou.runVex(vex, {'P': pts, 'tint': [hou.Vector3(-4, -2, -1)] * 5, 'nova_wi': [hou.Vector3(0, 0.6, 0.8)] * 5})
for key in ('dbg_noise', 'dbg_base', 'dbg_f', 'dbg_blend', 'dbg_p2', 'nova_eval'):
    v = res[key]
    print(key, [tuple(round(x, 4) for x in e) for e in v] if isinstance(v[0], (tuple, list)) or hasattr(v[0], '__len__') else v)
# Furnace of the full bsdf at a lane with a known base colour.
import math
n = 8192
PT = hou.Vector3(0.37 * 4, 0.11 * 4, 0.05 * 4)
wis = []
for i in range(n):
    z = 2.0 * (i + 0.5) / n - 1.0
    v = (i * 0.618034) % 1.0
    r = math.sqrt(max(0.0, 1 - z * z))
    wis.append(hou.Vector3(r * math.cos(2 * math.pi * v), r * math.sin(2 * math.pi * v), z))
res = hou.runVex(vex, {'P': [PT] * n, 'tint': [hou.Vector3(-4, -2, -1)] * n, 'nova_wi': wis})
tot = [sum(e[c] for e in res['nova_eval']) / n * 4 for c in range(3)]
print('base', tuple(round(x, 4) for x in res['dbg_base'][0]), 'integral eval / pi', tuple(round(t, 4) for t in tot))
sx = [((i * 0.7548777) % 1.0) for i in range(n)]
sy = [((i * 0.5698403) % 1.0) for i in range(n)]
res = hou.runVex(vex, {'P': [PT] * n, 'tint': [hou.Vector3(-4, -2, -1)] * n, 'nova_sx': sx, 'nova_sy': sy})
res2 = hou.runVex(vex, {'P': [PT] * n, 'tint': [hou.Vector3(-4, -2, -1)] * n, 'nova_wi': res['nova_sdir']})
acc = [0.0] * 3
for k in range(n):
    p = res2['nova_pdf'][k]
    if p > 0:
        for c in range(3):
            acc[c] += res2['nova_eval'][k][c] / p
print('E[eval/pdf] =', tuple(round(a / n, 4) for a in acc), 'types', sorted(set(res['nova_stype'])))
# A per lane: pdf_vex(dir) * weight / eval_vex(dir) along sampled directions, vs albedo(F).
est = []
for k in range(0, n, n // 16):
    e = res2['nova_eval'][k]
    w = res['nova_seval'][k]
    p = res2['nova_pdf'][k]
    if p > 0 and max(e) > 1e-6:
        c = max(range(3), key=lambda i: e[i])
        est.append(round(p * w[c] / e[c], 4))
print('A estimates along samples:', est)
print('albedo(F) at PT:', tuple(round(x, 4) for x in res['nova_albedo'][0]),
      'sum', round(sum(res['nova_albedo'][0]), 4), 'lum', round(0.2126 * res['nova_albedo'][0][0] + 0.7152 * res['nova_albedo'][0][1] + 0.0722 * res['nova_albedo'][0][2], 4))
print('sample weights (first 4):', [tuple(round(x, 4) for x in res['nova_seval'][k]) for k in range(4)])
