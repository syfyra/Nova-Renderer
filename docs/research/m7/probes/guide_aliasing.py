"""Are the guide planes anti-aliased like the beauty?

    $HFS/python313/python.exe -I guide_aliasing.py img.exr [img2.exr ...]

For each image: along every horizontal pair of neighbours, a "step" is a
place where the plane changes by more than a threshold. If the plane is
point-sampled (pixel centre), a step goes from one value to the other in one
pixel, so pixels next to steps hold either side's value. If it is filtered
like the beauty, edge pixels hold blends. The script reports, for the beauty,
albedo and N, the fraction of edge pixels whose value lies strictly between
its left and right neighbours (a blend), which is high for filtered planes.
"""
import os
import sys

import numpy as np

HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
os.add_dll_directory(os.path.join(HFS, 'bin'))
import OpenImageIO as oiio  # noqa: E402


def parts(path):
    inp = oiio.ImageInput.open(path)
    out = {}
    sub = 0
    while inp.seek_subimage(sub, 0):
        s = inp.spec()
        px = np.asarray(inp.read_image(sub, 0, 0, s.nchannels, 'float')).reshape(s.height, s.width, s.nchannels)
        out[s.getattribute('name') or 'part%d' % sub] = px[..., :3]
        sub += 1
    inp.close()
    return out


def blend_fraction(p, thr):
    left, mid, right = p[:, :-2], p[:, 1:-1], p[:, 2:]
    step = np.abs(right - left).max(-1) > thr
    lo = np.minimum(left, right) + 1e-4
    hi = np.maximum(left, right) - 1e-4
    between = np.all((mid > lo) & (mid < hi) | (np.abs(right - left) < thr), axis=-1)
    return int(step.sum()), float(between[step].mean()) if step.any() else float('nan')


for path in sys.argv[1:]:
    ps = parts(path)
    print(path)
    for name, thr in (('C', 0.1), ('albedo', 0.1), ('albedo_hd', 0.1), ('N', 0.3)):
        if name in ps:
            n, f = blend_fraction(ps[name], thr)
            print('  %-7s steps %6d  blended edge pixels %.3f' % (name, n, f))
