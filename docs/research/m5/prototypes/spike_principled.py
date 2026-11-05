"""M5 spike, part 4 (hython): the Principled Shader's surface VFL through the
CVEX transformation. Reads the HDA section through Houdini's opdef: reader."""
import os
import re
import subprocess
import sys
import time

import hou

D = sys.argv[1]
HFS = os.environ.get('HFS') or hou.text.expandString('$HFS')
code = hou.readFile('opdef:/Vop/principledshader::2.0?SurfaceVflCode')
print('SurfaceVflCode: %d chars' % len(code))
with open(os.path.join(D, 'principled_surface.vfl'), 'w', encoding='utf-8', newline='\n') as f:
    f.write(code)

m = re.search(r'^surface\s*\n?\s*(\w+)\s*\(', code, re.M)
print('surface function:', m.group(1) if m else None)
for fn in ('pbrlighting', 'getcomponents', 'renderstate', 'isshadowray', 'getsamplestore', 'rayimport', 'getraylevel',
           'trace(', 'occlusion(', 'illuminance', 'getlight', 'simport', 'dimport', 'objectstate', 'isshadingRHS',
           'layer.P = P;', 'layer.N = normalize(N);', 'frontface(', 'Du(', 'Dv(', 'dPds', 'getglobalraylevel',
           'getphotonlight', 'sample_light', 'shadow_light', 'getsmoothP', 'computenormal', 'area(', 'rayhittest'):
    print('  %-26s %d' % (fn, code.count(fn)))

PRE = '''#define getraylevel() 0
#define getglobalraylevel() 0
#define isshadowray() 0
#define rayimport(name, value) 0
'''
GLOBALS = ('vector P = 0; vector I = {0, 0, -1}; vector N = {0, 0, 1}; vector Ng = {0, 0, 1}; '
           'float s = 0; float t = 0; vector dPds = {1, 0, 0}; vector dPdt = {0, 1, 0}; float Time = 0; '
           'vector Eye = 0; vector nova_wi = {0, 0, 1}; float nova_sx = 0.5; float nova_sy = 0.5; '
           'export vector Cf = 0; export vector Of = 1; export float Af = 1; '
           'export vector nova_eval = 0; export float nova_pdf = 0; export vector nova_albedo = 0; ')
TAIL = '''
    {
        float nova_p = 0;
        nova_eval = eval_bsdf(F, -normalize(I), nova_wi, nova_p, bouncemask("all"));
        nova_pdf = nova_p;
        nova_albedo = albedo(F, -normalize(I), bouncemask("all"));
    }
'''
c = code.replace('layer.P = P;', 'layer.P = 0;').replace('layer.N = normalize(N);', 'layer.N = {0, 0, 1};')
c = c.replace('import pbrlighting;', '')


def drop_call(text, fname):
    out = text
    while True:
        k = out.find(fname + '(')
        if k < 0:
            return out
        depth, j = 1, k + len(fname) + 1
        while depth:
            depth += {'(': 1, ')': -1}.get(out[j], 0)
            j += 1
        semi = out.index(';', j)
        out = out[:k] + out[semi + 1:]


c = drop_call(c, 'pbrlighting')
LIBDEF = ('#define Ng set(0, 0, 1)\n#define dPds set(1, 0, 0)\n#define dPdt set(0, 1, 0)\n'
          '#define isshadingRHS() 1\n#define objectstate(name, value) 0\n')
LIBUNDEF = '#undef Ng\n#undef dPds\n#undef dPdt\n'
m = re.search(r'^surface\s*\n?\s*(\w+)\s*\(', c, re.M)
depth, i = 1, m.end()
while depth:
    depth += {'(': 1, ')': -1}.get(c[i], 0)
    i += 1
params = c[m.end():i - 1].strip()
body_open = c.index('{', i - 1)
body_close = c.rindex('}')
shim = 'vector frontface(vector n; vector i) { return dot(n, i) > 0 ? -n : n; }\n'
new = (PRE + LIBDEF + c[:m.start()] + LIBUNDEF + shim + 'cvex\n' + m.group(1) + '(' + GLOBALS + params + ')\n{\n    bsdf F;\n'
       + c[body_open + 1:body_close] + TAIL + '}\n')
src = os.path.join(D, 'principled_cvex.vfl')
with open(src, 'w', encoding='utf-8', newline='\n') as f:
    f.write(new)
t0 = time.perf_counter()
r = subprocess.run([os.path.join(HFS, 'bin', 'vcc.exe'), '-o', os.path.join(D, 'principled_cvex.vex'), src],
                   capture_output=True, text=True)
print('vcc %.1f s, rc %d' % (time.perf_counter() - t0, r.returncode))
errs = sorted(set(l for l in (r.stdout + r.stderr).splitlines() if 'Error' in l))
print('%d distinct errors' % len(errs))
for e in errs[:40]:
    print('  ', e[-200:])
