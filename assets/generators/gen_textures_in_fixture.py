#!/usr/bin/env python3
"""Generate the start-up texture instrument -- assets textures_in_fixture.* (spec 14.8).

SIXTEEN TILES ON A WALL, each with an image of its own, and the count is the point.

The engine's loader decodes a model's images on worker threads and the main thread
uploads at most five a frame. So a scene with more images than that cannot show them
all on its first frame unless the run waits for them before it starts -- whatever the
machine's speed. Every fixture in the corpus carries three images or fewer, and three
small images decode during the start-up's shader compiles, so on an idle machine
their first frame comes out whole by luck and no arm could tell a run that waits from
one that does not.

The images are EMBEDDED as data URIs, which the importer streams exactly as it does a
file (load_texture_from_memory_async), so the fixture is one glTF and not seventeen
files. Each is a two-colour checker of its own hue, so every tile is a different
image and a tile drawn without its texture -- white, its base colour -- is unlike it.

The scene is otherwise still: a fixed camera, one directional light casting nothing,
exposure pinned. Its frame 1 and its frame 30 then differ by the textures alone.

Regenerate with: python3 assets/generators/gen_textures_in_fixture.py
"""

import base64
import colorsys
import io

from PIL import Image
from fixture_paths import asset_ref
from gltf_pieces import bounds, pack, write_json

GRID = 4        # GRID x GRID tiles, so sixteen images: more than three frames' uploads
TEX = 16        # texels a side; the checker's squares are TEX / 4
TILE = 0.9      # a tile's side, on a pitch of 1 so the gaps show the background
UPLOADS_PER_FRAME = 5  # async_loader_process_pending's per-frame cap in engine.c


def _image(i):
    """Tile i's checker: its hue at full and at a third of its value, as PNG bytes."""
    hue = i / (GRID * GRID)
    light = [round(c * 255) for c in colorsys.hsv_to_rgb(hue, 0.8, 0.95)]
    dark = [round(c * 255) for c in colorsys.hsv_to_rgb(hue, 0.8, 0.32)]
    img = Image.new("RGB", (TEX, TEX))
    square = TEX // 4
    img.putdata([tuple(light if ((x // square) + (y // square)) % 2 == 0 else dark)
                 for y in range(TEX) for x in range(TEX)])
    out = io.BytesIO()
    img.save(out, format="PNG")
    return out.getvalue()


# One quad facing +Z, centred on the origin, a tile's side across; every tile's mesh
# shares it and differs only in its material.
h = TILE * 0.5
pos = [(-h, -h, 0.0), (h, -h, 0.0), (h, h, 0.0), (-h, h, 0.0)]
nrm = [(0.0, 0.0, 1.0)] * 4
uv = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
idx = [(0,), (1,), (2,), (0,), (2,), (3,)]

_chunks = [(pack("<3f", pos), 34962), (pack("<3f", nrm), 34962), (pack("<2f", uv), 34962),
           (pack("<H", idx), 34963)]


def _gltf():
    data, views = b"", []
    for blob, target in _chunks:
        views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob),
                      "target": target})
        data += blob
    mn, mx = bounds(pos)
    n = GRID * GRID
    offset = (GRID - 1) * 0.5
    return {
        "asset": {"version": "2.0", "generator": "gen_textures_in_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(n))}],
        "nodes": [{"name": f"tile_{i:02d}", "mesh": i,
                   "translation": [i % GRID - offset, offset - i // GRID, 0.0]}
                  for i in range(n)],
        "meshes": [{"name": f"tile_{i:02d}", "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3, "material": i}]} for i in range(n)],
        "materials": [{"name": f"tile_{i:02d}", "pbrMetallicRoughness": {
            "baseColorTexture": {"index": i}, "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "metallicFactor": 0.0, "roughnessFactor": 1.0}} for i in range(n)],
        "textures": [{"source": i} for i in range(n)],
        "images": [{"uri": "data:image/png;base64," + base64.b64encode(_image(i)).decode("ascii")}
                   for i in range(n)],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": mn, "max": mx},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
        "bufferViews": views,
        "buffers": [{"uri": "data:application/octet-stream;base64,"
                     + base64.b64encode(data).decode("ascii"), "byteLength": len(data)}],
    }


CSCN = {
    "models": [{"path": asset_ref("textures_in_fixture.gltf")}],
    # 6 back at 45 degrees sees 2.49 either side of the centre, so the 4-wide grid fits
    # with the background round it.
    "camera": {"eye": [0.0, 0.0, 6.0], "target": [0.0, 0.0, 0.0], "fov": 45},
    "lights": [{"name": "TilesSun", "type": "directional", "direction": [0.2, -0.3, -1.0],
                "color": [1.0, 1.0, 1.0], "intensity": 3.0, "cast_shadows": False}],
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
}


def main():
    images = [_image(i) for i in range(GRID * GRID)]
    # The property the fixture exists for: more images than the first frame can upload,
    # with a margin of frames, so a run that does not wait cannot show them all by luck.
    assert len(images) > 3 * UPLOADS_PER_FRAME, \
        "too few images: the first frames could upload them all and the arm would pass"
    assert len(set(images)) == len(images), "two tiles share an image, so neither names its tile"
    write_json("textures_in_fixture.gltf", _gltf())
    write_json("textures_in_fixture.cscn", CSCN)
    print(f"wrote textures_in_fixture.gltf and .cscn, {len(images)} embedded {TEX}x{TEX} images")


if __name__ == "__main__":
    main()
