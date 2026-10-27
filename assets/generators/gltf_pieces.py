"""A scene of placed primitives as a glTF, for the fixture generators that build one.

Each shape is packed once, each (shape, material) pair is one mesh, and each piece is a node
whose translation is its centre and whose scale is its size -- so a shape is authored at unit
size and a piece says where it is and how big.
"""
import base64
import json
import math
import struct

from fixture_paths import asset_path

# A unit cube centred on the origin, one quad per face so each face carries its own normal.
CUBE_FACES = [
    ((1, 0, 0), [(0.5, -0.5, 0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5)]),
    ((-1, 0, 0), [(-0.5, -0.5, -0.5), (-0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (-0.5, 0.5, -0.5)]),
    ((0, 1, 0), [(-0.5, 0.5, 0.5), (0.5, 0.5, 0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5)]),
    ((0, -1, 0), [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5)]),
    ((0, 0, 1), [(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]),
    ((0, 0, -1), [(0.5, -0.5, -0.5), (-0.5, -0.5, -0.5), (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5)]),
]


def cube():
    """The unit cube as (positions, normals, indices)."""
    positions, normals, indices = [], [], []
    for n, quad in CUBE_FACES:
        base = len(positions)
        positions += quad
        normals += [n] * 4
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, indices


def sphere(lat=12, lon=16):
    """A UV sphere of diameter 1, so a node's scale is the ball's diameter."""
    positions, normals, indices = [], [], []
    for i in range(lat + 1):
        theta = math.pi * i / lat
        for j in range(lon + 1):
            phi = 2.0 * math.pi * j / lon
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
            positions.append(tuple(0.5 * c for c in n))
            normals.append(n)
    for i in range(lat):
        for j in range(lon):
            a = i * (lon + 1) + j
            b = a + lon + 1
            indices += [a, a + 1, b, a + 1, b + 1, b]
    return positions, normals, indices


def pack(fmt, rows):
    return b"".join(struct.pack(fmt, *row) for row in rows)


def bounds(points):
    return ([min(p[i] for p in points) for i in range(3)],
            [max(p[i] for p in points) for i in range(3)])


def gltf_of(generator, shapes, materials, pieces):
    """`pieces`, each (name, shape, material, centre, size), as a glTF.

    `shapes` maps a shape's name to its (positions, normals, indices) and `materials` a
    material's name to its (base colour, metallic, roughness); a material's index is its place
    in `materials`.
    """
    data, buffer_views, accessors = b"", [], []
    shape_access = {}
    for shape, (positions, normals, indices) in shapes.items():
        chunks = [("POSITION", pack("<3f", positions), "VEC3", len(positions), bounds(positions),
                   5126, 34962),
                  ("NORMAL", pack("<3f", normals), "VEC3", len(normals), None, 5126, 34962),
                  (None, pack("<H", [(i,) for i in indices]), "SCALAR", len(indices), None,
                   5123, 34963)]
        attributes = {}
        for name, blob, kind, count, b, component, target in chunks:
            buffer_views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob),
                                 "target": target})
            data += blob
            accessor = {"bufferView": len(buffer_views) - 1, "componentType": component,
                        "count": count, "type": kind}
            if b:
                accessor["min"], accessor["max"] = b
            accessors.append(accessor)
            if name:
                attributes[name] = len(accessors) - 1
        shape_access[shape] = (attributes, len(accessors) - 1)

    names = list(materials)
    meshes, mesh_of = [], {}
    for _, shape, material, *_ in pieces:
        if (shape, material) not in mesh_of:
            attributes, index = shape_access[shape]
            mesh_of[(shape, material)] = len(meshes)
            meshes.append({"name": f"{material}_{shape}", "primitives": [{
                "attributes": attributes, "indices": index,
                "material": names.index(material)}]})
    nodes = [{"name": name, "mesh": mesh_of[(shape, material)], "translation": list(centre),
              "scale": list(size)} for name, shape, material, centre, size in pieces]
    materials_out = [{"name": m, "pbrMetallicRoughness": {
        "baseColorFactor": materials[m][0], "metallicFactor": materials[m][1],
        "roughnessFactor": materials[m][2]}} for m in names]
    return {
        "asset": {"version": "2.0", "generator": generator},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials_out,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"uri": "data:application/octet-stream;base64,"
                     + base64.b64encode(data).decode("ascii"), "byteLength": len(data)}],
    }


def write_json(name, obj):
    with open(asset_path(name), "w") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")
