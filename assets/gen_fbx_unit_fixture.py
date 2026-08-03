#!/usr/bin/env python3
"""FBX unit-scale fixture: the asset class that silently rendered black.

What this guards (spec 11.2, paying spec 11.1's recorded debt): FBX declares
its length unit as UnitScaleFactor RELATIVE TO CENTIMETRES, and the importer
bakes it to metres via assimp's GlobalScale pass -- the engine mirrors
assimp's raw * 0.01 conversion in import.c declared_unit_scale(). Nothing
pinned that mirror against a vendored-assimp change, and no gate had ever
exercised the FBX light-import path at all, which is how a cm-scale FBX
whose point lights sat 1200 "metres" from its model shipped black for weeks
(spec 11.1). This fixture is both pins at once: a cm-declared file whose
light must land at an exact metre position and actually reach the clusterer.

The file is ASCII FBX 7.4 -- plain text, so anyone can regenerate or read it
(the same reason the area goldens refuse gitignored HDRs). Geometry is the
assimp test suite's own unit cube (cubes_nonames.fbx) scaled to 100 cm, with
its authored per-face normals kept verbatim: the engine's import flags carry
no GenNormals and dereference mNormals unconditionally, so a normal-less
mesh would crash, not degrade. The light is a point light 200 cm above the
cube; after the 0.01x bake the gate expects it at exactly (0, 2, 0) metres.

Authored light Intensity is 5000 in FBX units: assimp folds Intensity/100
into the light color (FBXConverter), giving color (50,50,50), and import.c
re-expresses a peak > 1 color as engine intensity -- so the gate's pinned
arriving intensity is 50 cd, comfortably under the 10,000 cd unitless-format
ceiling. If either conversion changes, the gate trips; that is the point.

Regenerate with: python3 assets/gen_fbx_unit_fixture.py
"""

import os

# The assimp test cube: 24 per-face vertices at +/-0.5, quads with the FBX
# negative-terminated last index, 72 per-polygon-vertex normals. Scaled x100
# here so the cube is 100 cm in a file that declares centimetres.
CUBE_VERTS = [
    -0.5, -0.5, 0.5, 0.5, -0.5, 0.5, -0.5, 0.5, 0.5, 0.5, 0.5, 0.5,
    -0.5, 0.5, -0.5, 0.5, 0.5, -0.5, -0.5, -0.5, -0.5, 0.5, -0.5, -0.5,
]
CUBE_INDICES = "0,1,3,-3,2,3,5,-5,4,5,7,-7,6,7,1,-1,1,7,5,-4,6,0,2,-5"
CUBE_NORMALS = (
    "0,0,1,0,0,1,0,0,1,0,0,1,"
    "0,1,0,0,1,0,0,1,0,0,1,0,"
    "0,0,-1,0,0,-1,0,0,-1,0,0,-1,"
    "0,-1,0,0,-1,0,0,-1,0,0,-1,0,"
    "1,0,0,1,0,0,1,0,0,1,0,0,"
    "-1,0,0,-1,0,0,-1,0,0,-1,0,0"
)

GEO_ID = 1000
CUBE_ID = 2000
LAMP_ID = 3000
LIGHT_ATTR_ID = 4000

LIGHT_POS_CM = (0, 200, 0)  # -> (0, 2, 0) metres after the 0.01x bake
LIGHT_INTENSITY = 5000      # -> /100 -> color 50 -> engine 50 cd


def main():
    verts = ",".join(f"{v * 100:g}" for v in CUBE_VERTS)
    pos = ",".join(str(c) for c in LIGHT_POS_CM)

    fbx = f"""; FBX 7.4.0 project file
; assets/gen_fbx_unit_fixture.py -- see the generator docstring

FBXHeaderExtension:  {{
\tFBXHeaderVersion: 1003
\tFBXVersion: 7400
\tCreator: "cetra gen_fbx_unit_fixture.py"
}}
GlobalSettings:  {{
\tVersion: 1000
\tProperties70:  {{
\t\tP: "UpAxis", "int", "Integer", "",1
\t\tP: "UpAxisSign", "int", "Integer", "",1
\t\tP: "FrontAxis", "int", "Integer", "",2
\t\tP: "FrontAxisSign", "int", "Integer", "",1
\t\tP: "CoordAxis", "int", "Integer", "",0
\t\tP: "CoordAxisSign", "int", "Integer", "",1
\t\tP: "UnitScaleFactor", "double", "Number", "",1
\t\tP: "OriginalUnitScaleFactor", "double", "Number", "",1
\t}}
}}
Objects:  {{
\tGeometry: {GEO_ID}, "Geometry::", "Mesh" {{
\t\tVertices: *24 {{
\t\t\ta: {verts}
\t\t}}
\t\tPolygonVertexIndex: *24 {{
\t\t\ta: {CUBE_INDICES}
\t\t}}
\t\tGeometryVersion: 124
\t\tLayerElementNormal: 0 {{
\t\t\tVersion: 102
\t\t\tName: ""
\t\t\tMappingInformationType: "ByPolygonVertex"
\t\t\tReferenceInformationType: "Direct"
\t\t\tNormals: *72 {{
\t\t\t\ta: {CUBE_NORMALS}
\t\t\t}}
\t\t}}
\t\tLayer: 0 {{
\t\t\tVersion: 100
\t\t\tLayerElement:  {{
\t\t\t\tType: "LayerElementNormal"
\t\t\t\tTypedIndex: 0
\t\t\t}}
\t\t}}
\t}}
\tModel: {CUBE_ID}, "Model::unit_cube", "Mesh" {{
\t\tVersion: 232
\t\tProperties70:  {{
\t\t\tP: "Lcl Translation", "Lcl Translation", "", "A",0,0,0
\t\t}}
\t\tShading: T
\t\tCulling: "CullingOff"
\t}}
\tModel: {LAMP_ID}, "Model::unit_lamp", "Light" {{
\t\tVersion: 232
\t\tProperties70:  {{
\t\t\tP: "Lcl Translation", "Lcl Translation", "", "A",{pos}
\t\t}}
\t\tShading: T
\t\tCulling: "CullingOff"
\t}}
\tNodeAttribute: {LIGHT_ATTR_ID}, "NodeAttribute::unit_lamp", "Light" {{
\t\tTypeFlags: "Light"
\t\tGeometryVersion: 124
\t\tProperties70:  {{
\t\t\tP: "LightType", "enum", "", "",0
\t\t\tP: "Color", "Color", "", "A",1,1,1
\t\t\tP: "Intensity", "Number", "", "A",{LIGHT_INTENSITY}
\t\t}}
\t}}
}}
Connections:  {{
\tC: "OO",{CUBE_ID},0
\tC: "OO",{GEO_ID},{CUBE_ID}
\tC: "OO",{LAMP_ID},0
\tC: "OO",{LIGHT_ATTR_ID},{LAMP_ID}
}}
"""

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fbx_unit_fixture.fbx")
    with open(out, "w") as f:
        f.write(fbx)
    print("wrote", out, f"({os.path.getsize(out)} bytes)")


if __name__ == "__main__":
    main()
