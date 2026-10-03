"""What the cat tools read back out of a .glb (spec 13.17): its JSON, its float accessors, and
each clip's length and travel -- the file the engine plays being the record of what it plays.
No Blender here, so the Blender script and the plain-Python ones share it."""

import json
import struct
import sys


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        sys.exit(f"{path} is not a GLB")
    off, js, binary = 12, None, None
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        chunk = data[off + 8: off + 8 + clen]
        if ctype == 0x4E4F534A:
            js = json.loads(chunk)
        elif ctype == 0x004E4942:
            binary = chunk
        off += 8 + clen
    return js, binary


def accessor(js, binary, index):
    acc = js["accessors"][index]
    view = js["bufferViews"][acc["bufferView"]]
    comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[acc["type"]]
    if acc["componentType"] != 5126:
        sys.exit("only float accessors are read")
    start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride", comps * 4)
    return [struct.unpack_from(f"<{comps}f", binary, start + i * stride) for i in range(acc["count"])]


def hips_track(js, binary, animation):
    """A clip's Hips translation: its key times and its values, or None without one."""
    hips = next(i for i, n in enumerate(js["nodes"]) if n.get("name") == "Hips")
    for ch in animation["channels"]:
        if ch["target"]["node"] == hips and ch["target"]["path"] == "translation":
            sampler = animation["samplers"][ch["sampler"]]
            return ([t for (t,) in accessor(js, binary, sampler["input"])],
                    accessor(js, binary, sampler["output"]))
    return None


def clips(path):
    """Each clip in the file as (name, seconds, travel): its length, and how far Hips goes along
    model z from its first key to its last."""
    js, binary = read_glb(path)
    out = []
    for a in js.get("animations", []):
        track = hips_track(js, binary, a)
        if track:
            times, values = track
            out.append((a["name"], times[-1] - times[0], values[-1][2] - values[0][2]))
    return out
