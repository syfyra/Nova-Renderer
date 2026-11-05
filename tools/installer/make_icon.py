"""Draws Nova's mark (a four-pointed star with a violet-to-cyan gradient and a
soft glow on a dark rounded square) and writes icons/icon.ico (16-256 px,
PNG-compressed entries) and icons/preview.png (the 256 px one) for the
installer. The setup window draws the same mark as SVG (ui/index.html).

Run with Houdini's Python (numpy and OpenImageIO):

  "%HFS%/python313/python.exe" -I tools/installer/make_icon.py
"""

import os
import struct
import sys
import tempfile

HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
os.add_dll_directory(os.path.join(HFS, 'bin'))
import numpy as np  # noqa: E402
import OpenImageIO as oiio  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
VIOLET = np.array([0.55, 0.36, 0.98])
CYAN = np.array([0.13, 0.83, 0.93])


def mark(size, background=True):
    """RGBA float image of the mark, supersampled 4x."""
    ss = 4
    n = size * ss
    c = (np.arange(n) + 0.5) / n * 2.0 - 1.0
    x, y = np.meshgrid(c, -c)
    rgba = np.zeros((n, n, 4))
    if background:
        # A rounded square, a faint vertical gradient.
        r = 0.36
        qx, qy = np.maximum(np.abs(x) - (1 - r), 0), np.maximum(np.abs(y) - (1 - r), 0)
        inside = np.sqrt(qx * qx + qy * qy) <= r
        shade = 0.055 + 0.03 * (y + 1) / 2
        rgba[..., 0] = np.where(inside, shade * 0.9, 0)
        rgba[..., 1] = np.where(inside, shade * 0.95, 0)
        rgba[..., 2] = np.where(inside, shade * 1.6, 0)
        rgba[..., 3] = inside
    # The star: |x|^q + |y|^q <= s^q (four concave sides, sharp points).
    q, s = 0.55, 0.74 if background else 0.92
    d = (np.abs(x) ** q + np.abs(y) ** q) ** (1 / q) / s
    t = np.clip((x - y) * 0.5 + 0.5, 0, 1)[..., None]
    colour = VIOLET * (1 - t) + CYAN * t
    star = (d <= 1.0).astype(float)
    glow = np.exp(-np.maximum(d - 1.0, 0) * 4.0) * 0.3 * (1 - star)
    core = np.exp(-(x * x + y * y) / 0.02)[..., None] * 0.6  # a bright centre
    lit = np.clip(colour + core, 0, 1)
    for k in range(3):
        rgba[..., k] = rgba[..., k] * (1 - star - glow) + lit[..., k] * star + colour[..., k] * glow
    rgba[..., 3] = np.clip(rgba[..., 3] + star + glow, 0, 1)
    # Box filter down to the size.
    return rgba.reshape(size, ss, size, ss, 4).mean(axis=(1, 3))


def png_bytes(image):
    path = os.path.join(tempfile.mkdtemp(), 'mark.png')
    spec = oiio.ImageSpec(image.shape[1], image.shape[0], 4, oiio.UINT8)
    out = oiio.ImageOutput.create(path)
    out.open(path, spec)
    # Premultiplied in the drawing; PNG wants straight alpha.
    a = image[..., 3:4]
    straight = np.where(a > 0, image[..., :3] / np.maximum(a, 1e-6), 0)
    out.write_image(np.concatenate([np.clip(straight, 0, 1), a], axis=2).astype(np.float32))
    out.close()
    with open(path, 'rb') as f:
        return f.read()


def main():
    os.makedirs(os.path.join(HERE, 'icons'), exist_ok=True)
    sizes = [256, 64, 48, 32, 24, 16]
    images = [png_bytes(mark(s)) for s in sizes]
    header = struct.pack('<HHH', 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries, blobs = b'', b''
    for s, data in zip(sizes, images):
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    with open(os.path.join(HERE, 'icons', 'icon.ico'), 'wb') as f:
        f.write(header + entries + blobs)
    with open(os.path.join(HERE, 'icons', 'preview.png'), 'wb') as f:
        f.write(images[0])
    print('wrote icons/icon.ico (%s px) and icons/preview.png' % ', '.join(map(str, sizes)))


if __name__ == '__main__':
    sys.exit(main())
