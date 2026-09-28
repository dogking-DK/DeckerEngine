"""Self-authored 2x2 texture fixtures; Python 3, Pillow 11.1.0 for JPEG only.

PNG uses deterministic stdlib chunks. JPEG is an RGB solid red image, quality 95,
subsampling disabled; tests allow lossy rounding instead of requiring encoder bytes.
No third-party image content is used.
"""
from pathlib import Path
import struct
import zlib
import json
from PIL import Image

root = Path(__file__).resolve().parent


def png(name, depth, color, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    result = b"\x89PNG\r\n\x1a\n"
    result += chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, depth, color, 0, 0, 0))
    result += chunk(b"IDAT", zlib.compress(b"".join(b"\0" + row for row in rows)))
    result += chunk(b"IEND", b"")
    (root / name).write_bytes(result)


png("rgba.png", 8, 6, [bytes([255, 0, 0, 255, 0, 255, 0, 255]),
                        bytes([0, 0, 255, 128, 255, 255, 255, 255])])
png("gray16.png", 16, 0, [bytes([0, 0, 255, 255]), bytes([128, 0, 64, 0])])
Image.new("RGB", (2, 2), (255, 0, 0)).save(root / "red.jpg", quality=95, subsampling=0)

# Portable process fixture with interleaved positions/UVs and an external PNG.
vertices = [(1, 2, 3, 0, 0), (-2, 4, 1, 1, 0), (0, 0, -1, 0, 1)]
binary = b"".join(struct.pack("<5f", *v) for v in vertices) + struct.pack("<3H", 0, 1, 2)
scene = {
    "asset": {"version": "2.0"},
    "buffers": [{"uri": "triangle.bin", "byteLength": len(binary)}],
    "bufferViews": [{"buffer": 0, "byteLength": 60, "byteStride": 20},
                    {"buffer": 0, "byteOffset": 60, "byteLength": 6}],
    "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                  {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 3, "type": "VEC2"},
                  {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]}],
    "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
    "textures": [{"source": 0}], "images": [{"uri": "rgba.png"}],
}
(root / "triangle.bin").write_bytes(binary)
(root / "triangle.gltf").write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")

# Embedded JPEG variant, no external dependencies.
binary += b"\0" * (-len(binary) % 4)
jpeg = (root / "red.jpg").read_bytes()
scene["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(jpeg)})
binary += jpeg
scene["buffers"] = [{"byteLength": len(binary)}]
scene["images"] = [{"bufferView": 2, "mimeType": "image/jpeg"}]
text = json.dumps(scene, separators=(",", ":")).encode()
text += b" " * (-len(text) % 4)
binary += b"\0" * (-len(binary) % 4)
glb = struct.pack("<5I", 0x46546C67, 2, 28 + len(text) + len(binary), len(text), 0x4E4F534A)
glb += text + struct.pack("<2I", len(binary), 0x004E4942) + binary
(root / "triangle.glb").write_bytes(glb)
