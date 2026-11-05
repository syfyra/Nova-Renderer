"""Look for tile seams in denoised images.

    $HFS/python313/python.exe -I seam_check.py full.exr tiled1.exr [tiled2.exr ...] [--at 2048]

`full` is a denoise of the whole frame in one invocation. For each other
image, the per-column (and per-row) mean of |tiled - full| / (full + 0.05)
is printed around the tile boundary (--at) next to the image-wide median, and
the jump of the image itself across the boundary column is compared with the
same jump in `full` (a seam is a step that the untiled image does not have).
"""
import os
import sys

import numpy as np

HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
os.add_dll_directory(os.path.join(HFS, 'bin'))
import OpenImageIO as oiio  # noqa: E402


def beauty(path):
    inp = oiio.ImageInput.open(path)
    sub = 0
    first = None
    while inp.seek_subimage(sub, 0):
        s = inp.spec()
        px = np.asarray(inp.read_image(sub, 0, 0, s.nchannels, 'float')).reshape(s.height, s.width, s.nchannels)
        if first is None:
            first = px
        if (s.getattribute('name') or '') == 'C':
            return px[..., :3]
        sub += 1
    return first[..., :3]


args = sys.argv[1:]
at = 2048
if '--at' in args:
    i = args.index('--at')
    at = int(args[i + 1])
    del args[i:i + 2]
full = beauty(args[0])
for path in args[1:]:
    img = beauty(path)
    rel = (np.abs(img - full) / (full + 0.05)).mean(-1)
    col = rel.mean(0)
    row = rel.mean(1)
    print(os.path.basename(path))
    print('  |x - full| rel: image median %.5f mean %.5f' % (np.median(rel), rel.mean()))
    lo, hi = max(0, at - 24), min(full.shape[1], at + 24)
    peak_c = lo + int(np.argmax(col[lo:hi]))
    print('  columns %d..%d: max %.5f at col %d (column median %.5f)' % (lo, hi, col[lo:hi].max(), peak_c, np.median(col)))
    if full.shape[0] > at:
        lo2, hi2 = max(0, at - 24), min(full.shape[0], at + 24)
        peak_r = lo2 + int(np.argmax(row[lo2:hi2]))
        print('  rows %d..%d: max %.5f at row %d (row median %.5f)' % (lo2, hi2, row[lo2:hi2].max(), peak_r, np.median(row)))
    # step across neighbouring columns: tiled vs full
    step_t = np.abs(np.diff(img, axis=1)).mean(-1).mean(0)
    step_f = np.abs(np.diff(full, axis=1)).mean(-1).mean(0)
    ratio = step_t / (step_f + 1e-6)
    k = lo + int(np.argmax(ratio[lo:hi]))
    print('  column-step ratio tiled/full near boundary: max %.3f at col %d|%d (median %.3f)' % (
        ratio[lo:hi].max(), k, k + 1, np.median(ratio)))
    top = np.argsort(col)[-6:][::-1]
    print('  columns with the largest mean deviation: %s' % [(int(c), round(float(col[c]), 5)) for c in top])
    top = np.argsort(row)[-6:][::-1]
    print('  rows with the largest mean deviation: %s' % [(int(r), round(float(row[r]), 5)) for r in top])
