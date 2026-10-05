"""Render the user-approved B design into a real multi-resolution Windows ICO."""
import hashlib
import json
from pathlib import Path
import struct
from PySide6.QtCore import QBuffer, QIODevice, QRectF
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import QApplication

ROOT = Path(__file__).resolve().parents[1]
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def main():
    app = QApplication([])
    source = ROOT / 'src/installer_assets/concepts/b-coupler.svg'
    renderer = QSvgRenderer(str(source))
    if not renderer.isValid():
        raise ValueError('Approved B SVG is invalid')
    blobs = []
    master = None
    for size in SIZES:
        image = QImage(size, size, QImage.Format_ARGB32_Premultiplied)
        image.fill(QColor(0, 0, 0, 0))
        painter = QPainter(image)
        painter.setRenderHint(QPainter.Antialiasing)
        renderer.render(painter, QRectF(0, 0, size, size))
        painter.end()
        buffer = QBuffer(); buffer.open(QIODevice.WriteOnly)
        if not image.save(buffer, 'PNG'):
            raise RuntimeError(f'Could not encode {size}px icon')
        blobs.append(bytes(buffer.data()))
        master = image
    directory = bytearray(struct.pack('<HHH', 0, 1, len(SIZES)))
    offset = 6 + 16 * len(SIZES)
    for size, blob in zip(SIZES, blobs):
        directory += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    output = ROOT / 'src/installer_assets/installer.ico'
    output.write_bytes(bytes(directory) + b''.join(blobs))
    preview = ROOT / 'build/installer-design/installer-b-final.png'
    preview.parent.mkdir(parents=True, exist_ok=True)
    assert master.save(str(preview))
    result = {'design': 'B coupler; explicitly selected by user', 'sizes': SIZES,
              'source': str(source), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'file': str(output), 'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
              'bytes': output.stat().st_size, 'preview': str(preview)}
    (ROOT / 'evidence/installer-icon-b.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
