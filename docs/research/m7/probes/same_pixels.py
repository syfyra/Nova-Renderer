"""Are two EXRs' beauty planes bit-identical? (e.g. frames of a static scene)

    $HFS/python313/python.exe -I same_pixels.py a.exr b.exr
"""
import os
import sys

import numpy as np

HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
os.add_dll_directory(os.path.join(HFS, 'bin'))
import OpenImageIO as oiio  # noqa: E402


def first(path):
    i = oiio.ImageInput.open(path)
    s = i.spec()
    px = np.asarray(i.read_image(0, 0, 0, s.nchannels, 'float'))
    i.close()
    return px


a, b = first(sys.argv[1]), first(sys.argv[2])
print('identical' if np.array_equal(a, b) else 'different: mean |a-b| = %g' % float(np.abs(a - b).mean()))
