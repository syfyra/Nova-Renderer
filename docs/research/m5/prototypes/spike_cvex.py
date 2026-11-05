"""M5 spike, part 2 (hython): can a VEX Material Builder's surface code run
as CVEX, and do eval_bsdf / sample_bsdf / albedo give usable values there?"""
import math
import os
import re
import subprocess
import sys

import hou

D = sys.argv[1]
HFS = os.environ['HFS']
VCC = os.path.join(HFS, 'bin', 'vcc.exe')


def compile_vfl(name, code):
    src = os.path.join(D, name + '.vfl')
    vex = os.path.join(D, name + '.vex')
    with open(src, 'w', encoding='utf-8', newline='\n') as f:
        f.write(code)
    r = subprocess.run([VCC, '-o', vex, src], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(vex):
        print('vcc failed for %s:\n%s%s' % (name, r.stdout[-3000:], r.stderr[-3000:]))
        return None
    if r.stdout.strip() or r.stderr.strip():
        print('vcc messages for %s:\n%s%s' % (name, r.stdout[-1500:], r.stderr[-1500:]))
    return vex


# 1. Conventions on a plain diffuse bsdf.
DIFF = r'''
#include <pbr.h>
cvex diff_probe(vector N = {0, 0, 1}; vector I = {0, 0, -1}; vector wi = {0, 0, 1}; float sx = 0.5; float sy = 0.5;
                export vector ev = 0; export float pdf = 0; export vector alb = 0;
                export vector sdir = 0; export vector seval = 0; export float spdf = 0; export int stype = 0)
{
    bsdf F = diffuse(N);
    vector viewer = -normalize(I);
    float p = 0;
    ev = eval_bsdf(F, viewer, wi, p, PBR_ALL_MASK);
    pdf = p;
    alb = albedo(F, viewer, PBR_ALL_MASK);
    vector d = 0, e = 0; float sp = 0; int t = 0;
    sample_bsdf(F, viewer, d, e, sp, t, sx, sy, PBR_ALL_MASK);
    sdir = d; seval = e; spdf = sp; stype = t;
}
'''
vex = compile_vfl('diff_probe', DIFF)
if vex:
    cosines = [1.0, 0.5, 0.0, -0.5]
    wis = [hou.Vector3(math.sqrt(max(0, 1 - c * c)), 0, c) for c in cosines]
    res = hou.runVex(vex, {'wi': wis, 'sx': [0.3] * 4, 'sy': [0.7] * 4})
    for k, c in enumerate(cosines):
        print('diffuse cos %.2f: eval %s pdf %.4f albedo %s | sample dir %s eval %s pdf %.4f type %d' % (
            c, tuple(round(x, 4) for x in res['ev'][k]), res['pdf'][k], tuple(round(x, 4) for x in res['alb'][k]),
            tuple(round(x, 3) for x in res['sdir'][k]), tuple(round(x, 4) for x in res['seval'][k]), res['spdf'][k],
            res['stype'][k]))
    # Monte Carlo: E[eval / pdf_uniform] over the hemisphere.
    n = 4096
    wis, sxs = [], []
    for i in range(n):
        u, v = (i + 0.5) / n, ((i * 0.618034) % 1.0)
        z = u
        r = math.sqrt(max(0.0, 1 - z * z))
        wis.append(hou.Vector3(r * math.cos(2 * math.pi * v), r * math.sin(2 * math.pi * v), z))
    res = hou.runVex(vex, {'wi': wis})
    s = sum(e[0] for e in res['ev']) / n * 2 * math.pi
    print('diffuse hemisphere integral of eval (uniform MC): %.4f' % s)

# 2. The builder's surface code as CVEX.
code = open(os.path.join(D, 'vexbuilder_surface.vfl'), encoding='utf-8').read()
SHIMS = '''
// Nova: shading-context functions as CVEX stand-ins.
#define getraylevel() __nova_raylevel
#define isshadowray() 0
#define rayimport(name, value) 0
#include <pbr.h>
'''
GLOBALS = ('vector P = 0; vector I = {0, 0, -1}; vector N = {0, 0, 1}; vector Ng = {0, 0, 1}; '
           'float s = 0; float t = 0; vector dPds = {1, 0, 0}; vector dPdt = {0, 1, 0}; float Time = 0; '
           'vector Eye = 0; int __nova_raylevel = 0; '
           'vector nova_wi = {0, 0, 1}; float nova_sx = 0.5; float nova_sy = 0.5; '
           'export vector Cf = 0; export vector Of = 1; export float Af = 1; export vector Ce = 0; '
           'export vector nova_eval = 0; export float nova_pdf = 0; export vector nova_albedo = 0; '
           'export vector nova_sdir = 0; export vector nova_seval = 0; export float nova_spdf = 0; '
           'export int nova_stype = 0; ')
TAIL = '''
    // Nova: evaluate and sample the shader's bsdf.
    {
        vector nova_viewer = -normalize(I);
        float nova_p = 0;
        nova_eval = eval_bsdf(F, nova_viewer, nova_wi, nova_p, PBR_ALL_MASK);
        nova_pdf = nova_p;
        nova_albedo = albedo(F, nova_viewer, PBR_ALL_MASK);
        vector nova_d = 0, nova_e = 0; float nova_sp = 0; int nova_t = 0;
        sample_bsdf(F, nova_viewer, nova_d, nova_e, nova_sp, nova_t, nova_sx, nova_sy, PBR_ALL_MASK);
        nova_sdir = nova_d; nova_seval = nova_e; nova_spdf = nova_sp; nova_stype = nova_t;
    }
'''
m = re.search(r'^surface\s*\n\s*(\w+)\s*\(', code, re.M)
assert m, 'no surface function'
name = m.group(1)
open_paren = m.end()
# Close of the parameter list: match parentheses from open_paren.
depth, i = 1, open_paren
while depth:
    depth += {'(': 1, ')': -1}.get(code[i], 0)
    i += 1
close_paren = i - 1
params = code[open_paren:close_paren].strip()
body_open = code.index('{', close_paren)
body_close = code.rindex('}')
INCLUDE_GLOBALS = '#define P set(0, 0, 0)\n#define N set(0, 0, 1)\n#define I set(0, 0, -1)\n#define Ng set(0, 0, 1)\n'
INCLUDE_UNDEF = '#undef P\n#undef N\n#undef I\n#undef Ng\n'
new = (SHIMS + INCLUDE_GLOBALS + code[:m.start()] + INCLUDE_UNDEF + 'cvex\n' + name + '(' + GLOBALS + params + ')\n{\n    bsdf F;\n'
       + code[body_open + 1:body_close] + TAIL + '}\n')
vex = compile_vfl('builder_cvex', new)
if vex:
    func = hou.runVex
    cosines = [1.0, 0.7, 0.3, 0.0]
    wis = [hou.Vector3(math.sqrt(max(0, 1 - c * c)), 0, c) for c in cosines]
    res = func(vex, {'nova_wi': wis, 'P': [hou.Vector3(0.1 * k, 0.2, 0.0) for k in range(4)],
                     'nova_sx': [0.2, 0.4, 0.6, 0.8], 'nova_sy': [0.5] * 4})
    for k, c in enumerate(cosines):
        print('builder cos %.2f: eval %s pdf %.4f albedo %s Of %s | sample dir %s eval %s pdf %.4f type %d' % (
            c, tuple(round(x, 4) for x in res['nova_eval'][k]), res['nova_pdf'][k],
            tuple(round(x, 4) for x in res['nova_albedo'][k]), tuple(round(x, 3) for x in res['Of'][k]),
            tuple(round(x, 3) for x in res['nova_sdir'][k]), tuple(round(x, 4) for x in res['nova_seval'][k]),
            res['nova_spdf'][k], res['nova_stype'][k]))

if vex:
    n = 8192
    wis = []
    for i in range(n):
        z = 2.0 * (i + 0.5) / n - 1.0
        v = (i * 0.618034) % 1.0
        r = math.sqrt(max(0.0, 1 - z * z))
        wis.append(hou.Vector3(r * math.cos(2 * math.pi * v), r * math.sin(2 * math.pi * v), z))
    res = hou.runVex(vex, {'nova_wi': wis})
    tot = [sum(e[c] for e in res['nova_eval']) / n * 4 * math.pi for c in range(3)]
    print('builder: sphere integral of eval / pi = %s, albedo() = %s' % (
        tuple(round(t / math.pi, 4) for t in tot), tuple(round(x, 4) for x in res['nova_albedo'][0])))
    sx = [((i * 0.7548777) % 1.0) for i in range(n)]
    sy = [((i * 0.5698403) % 1.0) for i in range(n)]
    res = hou.runVex(vex, {'nova_sx': sx, 'nova_sy': sy})
    dirs = res['nova_sdir']
    res2 = hou.runVex(vex, {'nova_wi': dirs})
    acc = [0.0, 0.0, 0.0]
    for k in range(n):
        p = res2['nova_pdf'][k]
        if p > 0:
            for c in range(3):
                acc[c] += res2['nova_eval'][k][c] / p
    print('builder: E[eval/pdf] over sample_bsdf directions = %s; mean sample eval = %s' % (
        tuple(round(a / n, 4) for a in acc), tuple(round(sum(e[c] for e in res['nova_seval']) / n, 4) for c in range(3))))
