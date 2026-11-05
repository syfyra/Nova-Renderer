"""Renders Nova's icon, icons/icon.svg (the O of the Nova logo on the logo's
off-white), into icons/icon.ico (16-256 px, PNG-compressed entries) and
icons/preview.png (the 256 px one) for the installer. The setup window's title
bar draws the same O as SVG (ui/index.html).

Run with Houdini's Python (Qt's SVG renderer from Houdini's PySide6):

  "%HFS%/python313/python.exe" -I tools/installer/make_icon.py
"""

import os
import struct
import sys

HFS = os.environ.get('HFS', r'C:\Program Files\Side Effects Software\Houdini 22.0.466')
os.add_dll_directory(os.path.join(HFS, 'bin'))
sys.path.insert(0, os.path.join(HFS, 'python313', 'lib', 'site-packages-forced'))
from PySide6 import QtCore, QtGui, QtSvg  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [256, 64, 48, 32, 24, 16]


def png_bytes(renderer, size):
    """The SVG rendered at size x size, as PNG (straight alpha)."""
    image = QtGui.QImage(size, size, QtGui.QImage.Format_ARGB32_Premultiplied)
    image.fill(0)
    painter = QtGui.QPainter(image)
    painter.setRenderHint(QtGui.QPainter.Antialiasing)
    renderer.render(painter, QtCore.QRectF(0, 0, size, size))
    painter.end()
    data = QtCore.QByteArray()
    buffer = QtCore.QBuffer(data)
    buffer.open(QtCore.QIODevice.WriteOnly)
    image.convertToFormat(QtGui.QImage.Format_ARGB32).save(buffer, 'PNG')
    buffer.close()
    return bytes(data)


def main():
    app = QtGui.QGuiApplication(['make_icon', '-platform', 'offscreen'])  # noqa: F841 (Qt needs one)
    renderer = QtSvg.QSvgRenderer(os.path.join(HERE, 'icons', 'icon.svg'))
    if not renderer.isValid():
        raise RuntimeError('icons/icon.svg is not a valid SVG')
    images = [png_bytes(renderer, s) for s in SIZES]
    header = struct.pack('<HHH', 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    entries, blobs = b'', b''
    for s, data in zip(SIZES, images):
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    with open(os.path.join(HERE, 'icons', 'icon.ico'), 'wb') as f:
        f.write(header + entries + blobs)
    with open(os.path.join(HERE, 'icons', 'preview.png'), 'wb') as f:
        f.write(images[0])
    print('wrote icons/icon.ico (%s px) and icons/preview.png' % ', '.join(map(str, SIZES)))


if __name__ == '__main__':
    sys.exit(main())
