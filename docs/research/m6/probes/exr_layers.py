"""Lists the parts (subimages) and channels of a multi-part EXR with pixel
statistics, as a Markdown table. Uses the OpenImageIO module shipped with
Houdini's Python ($HFS/python313/python.exe -I exr_layers.py <file.exr>).
"""
import os
import sys

HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
os.add_dll_directory(os.path.join(HFS, 'bin'))
import OpenImageIO as oiio  # noqa: E402


def main(path):
    inp = oiio.ImageInput.open(path)
    if not inp:
        sys.exit('cannot open %s: %s' % (path, oiio.geterror()))
    print('| part | name | channels | formats | min | max | mean | nonzero px |')
    print('|---|---|---|---|---|---|---|---|')
    sub = 0
    while inp.seek_subimage(sub, 0):
        spec = inp.spec()
        name = spec.getattribute('name') or ''
        fmts = sorted({str(f) for f in spec.channelformats} or {str(spec.format)})
        pixels = inp.read_image(sub, 0, 0, spec.nchannels, 'float')
        mins, maxs, means = [], [], []
        nonzero = 0
        if pixels is not None:
            flat = pixels.reshape(-1, spec.nchannels)
            mins = ['%.4g' % v for v in flat.min(axis=0)]
            maxs = ['%.4g' % v for v in flat.max(axis=0)]
            means = ['%.4g' % v for v in flat.mean(axis=0)]
            nonzero = int((abs(flat).sum(axis=1) > 0).sum())
        print('| %d | %s | %s | %s | %s | %s | %s | %d |' % (sub, name, ', '.join(spec.channelnames), '/'.join(fmts),
                                                          ' '.join(mins), ' '.join(maxs), ' '.join(means), nonzero))
        sub += 1
    inp.close()


main(sys.argv[1])
