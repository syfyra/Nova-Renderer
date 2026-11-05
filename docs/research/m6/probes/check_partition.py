"""Checks how Karma's beauty-component AOVs add up, on the probe render
(karma_aovs.exr from probe_karma_aovs.py + husk). Prints mean absolute
differences per relation, relative to the mean beauty.

    $HFS/python313/python.exe -I check_partition.py karma_aovs.exr
"""
import os
import sys

import numpy as np

HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
os.add_dll_directory(os.path.join(HFS, 'bin'))
import OpenImageIO as oiio  # noqa: E402


def load(path):
    planes = {}
    inp = oiio.ImageInput.open(path)
    sub = 0
    while inp.seek_subimage(sub, 0):
        spec = inp.spec()
        px = inp.read_image(sub, 0, 0, spec.nchannels, 'float')
        planes[spec.getattribute('name')] = np.asarray(px, dtype=np.float64).reshape(spec.height, spec.width, -1)
        sub += 1
    inp.close()
    return planes


def rgb(p, name):
    return p[name][..., :3]


def report(title, a, b, ref):
    d = np.abs(a - b).mean()
    print('%-70s mean|diff| = %.5f  (%.2f %% of mean beauty)' % (title, d, 100.0 * d / ref))


p = load(sys.argv[1])
C = rgb(p, 'C')
ref = C.mean()
first_events = ['visiblelights', 'combineddiffuse', 'combinedglossyreflection', 'glossytransmission', 'sss',
                'combinedvolume', 'combinedemission']
report('C  vs  CL + C<RD>.*L + C<RG>.*L + C<TG>.*L + C<TD>.*L + CV.*L + C.*O',
       C, sum(rgb(p, n) for n in first_events), ref)
report('combineddiffuse  vs  directdiffuse + indirectdiffuse', rgb(p, 'combineddiffuse'),
       rgb(p, 'directdiffuse') + rgb(p, 'indirectdiffuse'), ref)
report('combinedglossyreflection  vs  direct + indirect', rgb(p, 'combinedglossyreflection'),
       rgb(p, 'directglossyreflection') + rgb(p, 'indirectglossyreflection'), ref)
report('combinedemission  vs  directemission + indirectemission', rgb(p, 'combinedemission'),
       rgb(p, 'directemission') + rgb(p, 'indirectemission'), ref)
report('C  vs  C_key + C_fill + C_emission (+ untagged dome, not split)', C,
       rgb(p, 'C_key') + rgb(p, 'C_fill') + rgb(p, 'C_emission'), ref)
report('C_key  vs  custom C.*<L.\'key\'> ... combineddiffuse_key vs key_diffuse', rgb(p, 'combineddiffuse_key'),
       rgb(p, 'key_diffuse'), ref)
report('C_fill  vs  custom fill_all (C.*<L.\'fill\'>)', rgb(p, 'C_fill'), rgb(p, 'fill_all'), ref)
report('albedo (CDA) vs albedodiffuse (CDA)', rgb(p, 'albedo'), rgb(p, 'albedodiffuse'), ref)
cg = rgb(p, 'combinedglossyreflection')
coat = rgb(p, 'coat')
print('coat <= combinedglossyreflection everywhere: %s (max excess %.4g)' % (bool((coat <= cg + 1e-3).all()),
                                                                            float((coat - cg).max())))
bu = rgb(p, 'beautyunshadowed')
print('beautyunshadowed >= C (mean %.4f vs %.4f)' % (bu.mean(), ref))
sh = rgb(p, 'shadow')
mask = bu.sum(axis=2) > 1e-3
est = 1.0 - C.sum(axis=2)[mask] / bu.sum(axis=2)[mask]
print('shadow vs 1 - C/unshadowed (where unshadowed > 0): mean shadow %.4f, mean (1 - C/U) %.4f'
      % (sh.mean(axis=2)[mask].mean(), est.mean()))
alpha = p['C'][..., 3]
at = rgb(p, 'alphatransmission').mean(axis=2)
print('alphatransmission (!C<TG>*B) vs C alpha: mean %.4f vs %.4f, mean|diff| %.4f'
      % (at.mean(), alpha.mean(), np.abs(at - alpha).mean()))
