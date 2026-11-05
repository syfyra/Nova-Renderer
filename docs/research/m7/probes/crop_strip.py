"""Side-by-side crops (sRGB PNG) of the beauty of several EXRs, for eyeballing.

    $HFS/python313/python.exe -I crop_strip.py out.png x y w h scale img1.exr img2.exr ...

Each crop is (x, y, w, h) of part "C" (or the only part), clamped to [0, 1]
after a plain sRGB transfer, nearest-neighbour enlarged by `scale`.
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
            inp.close()
            return px[..., :3]
        sub += 1
    inp.close()
    return first[..., :3]


def srgb(x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)


out, x, y, w, h, scale = sys.argv[1], *map(int, sys.argv[2:7])
crops = []
for p in sys.argv[7:]:
    img = beauty(p)[y:y + h, x:x + w]
    img = np.repeat(np.repeat(srgb(img), scale, 0), scale, 1)
    crops.append(img)
    crops.append(np.ones((img.shape[0], 4, 3)))
strip = np.concatenate(crops[:-1], axis=1).astype(np.float32)
spec = oiio.ImageSpec(strip.shape[1], strip.shape[0], 3, 'uint8')
o = oiio.ImageOutput.create(out)
o.open(out, spec)
o.write_image(strip)
o.close()
print('wrote', out, strip.shape)
