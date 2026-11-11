"""CPU against XPU over the images of a broken_stages.py or extreme_settings.py run with
--devices cpu,xpu: the scenes whose means differ by more than a tolerance, largest first.

  $HFS/python313/python.exe device_parity.py <HFS> <out>/img <out>/img [tolerance, default 0.1]
"""
import os
import sys

hfs = sys.argv[1]
a_dir, b_dir = sys.argv[2], sys.argv[3]
os.add_dll_directory(os.path.join(hfs, "bin"))
import numpy as np
import OpenImageIO as oiio


def load(path):
    inp = oiio.ImageInput.open(path)
    if not inp:
        return None
    spec = inp.spec()
    px = inp.read_image(0, 0, 0, spec.nchannels, oiio.FLOAT)
    inp.close()
    return px


rows = []
for f in sorted(os.listdir(a_dir)):
    if not f.endswith("_cpu.exr"):
        continue
    g = os.path.join(b_dir, f.replace("_cpu.exr", "_xpu.exr"))
    if not os.path.exists(g):
        continue
    a, b = load(os.path.join(a_dir, f)), load(g)
    if a is None or b is None:
        continue
    if a.shape != b.shape:
        rows.append((9e9, f, "shape %s vs %s" % (a.shape, b.shape)))
        continue
    ma, mb = a[..., :3].mean(axis=(0, 1)), b[..., :3].mean(axis=(0, 1))
    alpha = (a[..., 3].mean(), b[..., 3].mean()) if a.shape[2] > 3 else (0, 0)
    denom = np.maximum(np.maximum(np.abs(ma), np.abs(mb)), 1e-3)
    rel = float(np.max(np.abs(ma - mb) / denom))
    rows.append((rel, f[:-8], "cpu %s xpu %s alpha %.3f/%.3f" % (np.round(ma, 4), np.round(mb, 4), alpha[0], alpha[1])))
rows.sort(reverse=True)
tolerance = float(sys.argv[4]) if len(sys.argv) > 4 else 0.1
for rel, name, text in rows:
    if rel > tolerance:
        print("%8.3f %-34s %s" % (rel, name, text))
print(len(rows), "compared")
