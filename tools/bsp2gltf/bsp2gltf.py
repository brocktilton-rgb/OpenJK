#!/usr/bin/env python3
"""Convert a Jedi Academy RBSP into glTF 2.0 plus an entity dump.

Python 3.8+ standard library only. Definitions follow code/qcommon/qfiles.h:
ident RBSP, version 1, 18 lumps, 80-byte draw verts, 148-byte surfaces.
"""

from __future__ import annotations

import array
import argparse
import json
import struct
import sys
import zipfile
from pathlib import Path

IDENT = b"RBSP"
VERSION = 1
HEADER_LUMPS = 18
HEADER_SIZE = 8 + HEADER_LUMPS * 8  # 152
DRAWVERT_SIZE = 80  # xyz, st, 4 lightmap st, normal, 4 RGBA
SURFACE_SIZE = 148
SHADER_SIZE = 72  # char[64] + two ints
MODEL_SIZE = 40  # mins, maxs, firstSurface, numSurfaces, firstBrush, numBrushes
INDEX_SIZE = 4

LUMP_ENTITIES = 0
LUMP_SHADERS = 1
LUMP_MODELS = 7
LUMP_DRAWVERTS = 10
LUMP_DRAWINDEXES = 11
LUMP_SURFACES = 13

MST_PLANAR = 1
MST_PATCH = 2
MST_TRIANGLE_SOUP = 3
MST_FLARE = 4
SURFACE_NAMES = {
    MST_PLANAR: "planar",
    MST_PATCH: "patch",
    MST_TRIANGLE_SOUP: "triangle soup",
    MST_FLARE: "flare",
}


class BspError(Exception):
    pass


def _die(message):
    print("error: " + message, file=sys.stderr)
    raise SystemExit(1)


def _u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def _i32(data, offset):
    return struct.unpack_from("<i", data, offset)[0]


def _f32(data, offset):
    return struct.unpack_from("<f", data, offset)[0]


def _vec3(data, offset):
    return struct.unpack_from("<3f", data, offset)


def _lump(data, index):
    offset = 8 + index * 8
    fileofs = _i32(data, offset)
    filelen = _i32(data, offset + 4)
    if fileofs < 0 or filelen < 0 or fileofs + filelen > len(data):
        raise BspError("lump %d is outside the file" % index)
    return data[fileofs:fileofs + filelen]


def _shader_name(raw):
    return raw.split(b"\x00", 1)[0].decode("latin-1", "replace")


def _is_system_shader(name):
    return name.lower().startswith("textures/system/")


def _to_gltf(x, y, z, scale):
    # Q3/JA: +X forward, +Y left, +Z up. glTF is Y-up.
    # Determinant of (x, z, -y) is +1, so triangle winding stays CCW.
    return (x * scale, z * scale, -y * scale)


def _normalize(x, y, z):
    length = (x * x + y * y + z * z) ** 0.5
    if length <= 1e-8:
        return (0.0, 0.0, 1.0)
    return (x / length, y / length, z / length)


class Vert(object):
    def __init__(self, xyz, normal, st, lightmap, color):
        self.xyz = xyz
        self.normal = normal
        self.st = st
        self.lightmap = lightmap
        self.color = color

    def mix(self, other, other2, a, b, c):
        def blend(i):
            return a * i[0] + b * i[1] + c * i[2]

        xyz = tuple(blend(p) for p in zip(self.xyz, other.xyz, other2.xyz))
        normal = _normalize(*(blend(p) for p in zip(self.normal, other.normal, other2.normal)))
        st = tuple(blend(p) for p in zip(self.st, other.st, other2.st))
        lightmap = tuple(blend(p) for p in zip(self.lightmap, other.lightmap, other2.lightmap))
        color = tuple(max(0, min(255, int(round(blend(p))))) for p in zip(self.color, other.color, other2.color))
        return Vert(xyz, normal, st, lightmap, color)


def _bezier(p0, p1, p2, t):
    s = 1.0 - t
    return p0.mix(p1, p2, s * s, 2.0 * s * t, t * t)


def _tessellate_patch(controls, width, height, level):
    if width < 3 or height < 3 or (width % 2) == 0 or (height % 2) == 0:
        raise BspError("patch size %dx%d is not a 3x3 Bezier grid" % (width, height))
    verts = []
    indices = []
    patches_x = (width - 1) // 2
    patches_y = (height - 1) // 2
    n = level + 1
    for py in range(patches_y):
        for px in range(patches_x):
            ctrl = []
            for row in range(3):
                for col in range(3):
                    ctrl.append(controls[(py * 2 + row) * width + (px * 2 + col)])
            rows = []
            for row in range(3):
                curve = []
                for i in range(n):
                    curve.append(_bezier(ctrl[row * 3], ctrl[row * 3 + 1], ctrl[row * 3 + 2], i / float(level)))
                rows.append(curve)
            grid = []
            for j in range(n):
                for i in range(n):
                    grid.append(_bezier(rows[0][i], rows[1][i], rows[2][i], j / float(level)))
            base = len(verts)
            verts.extend(grid)
            for y in range(level):
                for x in range(level):
                    i00 = base + y * n + x
                    i10 = i00 + 1
                    i01 = i00 + n
                    i11 = i01 + 1
                    indices.extend((i00, i10, i01, i01, i10, i11))
    return verts, indices


def _read_vert(data, offset):
    xyz = _vec3(data, offset)
    st = struct.unpack_from("<2f", data, offset + 12)
    lightmap = struct.unpack_from("<2f", data, offset + 20)  # slot 0 of the four
    normal = _vec3(data, offset + 12 + 8 + 32)
    color = struct.unpack_from("<4B", data, offset + 12 + 8 + 32 + 12)
    return Vert(xyz, normal, st, lightmap, color)


def _parse_header(data):
    if len(data) < HEADER_SIZE:
        raise BspError("file is shorter than an RBSP header")
    if data[0:4] != IDENT:
        raise BspError("not an RBSP file (ident %r, expected %r)" % (data[0:4], IDENT))
    version = _i32(data, 4)
    if version != VERSION:
        raise BspError("unsupported RBSP version %d (expected %d)" % (version, VERSION))


def _load_records(blob, size, label):
    if len(blob) % size:
        raise BspError("%s lump length %d is not a multiple of %d" % (label, len(blob), size))
    return [blob[i:i + size] for i in range(0, len(blob), size)]


def _parse_entities(blob):
    text = blob.split(b"\x00", 1)[0].decode("latin-1", "replace")
    entities = []
    i = 0
    n = len(text)
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        if text[i] != "{":
            raise BspError("entity lump has junk before '{'")
        i += 1
        ent = {}
        while True:
            while i < n and text[i].isspace():
                i += 1
            if i >= n:
                raise BspError("entity lump ended inside an entity")
            if text[i] == "}":
                i += 1
                break
            if text[i] != '"':
                raise BspError("entity key is not quoted")
            key, i = _parse_quoted(text, i)
            while i < n and text[i].isspace():
                i += 1
            if i >= n or text[i] != '"':
                raise BspError("entity value for %r is not quoted" % key)
            value, i = _parse_quoted(text, i)
            ent[key] = value
        entities.append(ent)
    return entities


def _parse_quoted(text, i):
    if text[i] != '"':
        raise BspError("expected quote")
    i += 1
    out = []
    while i < len(text):
        ch = text[i]
        if ch == "\\":
            if i + 1 < len(text):
                out.append(text[i + 1])
                i += 2
                continue
        if ch == '"':
            return "".join(out), i + 1
        out.append(ch)
        i += 1
    raise BspError("unterminated entity string")


class Bucket(object):
    def __init__(self):
        self.verts = []
        self.indices = []

    def add(self, verts, indices):
        base = len(self.verts)
        self.verts.extend(verts)
        self.indices.extend(base + i for i in indices)


def _read_ints(blob):
    values = array.array("i")
    values.frombytes(blob)
    if sys.byteorder != "little":
        values.byteswap()
    return list(values)


def _pack_floats(values):
    raw = array.array("f", values)
    if sys.byteorder != "little":
        raw.byteswap()
    return raw.tobytes()


def _surface_geometry(surf, verts, indexes, level):
    surface_type = _i32(surf, 8)
    if surface_type == MST_FLARE:
        return [], []
    first_vert = _i32(surf, 12)
    num_verts = _i32(surf, 16)
    first_index = _i32(surf, 20)
    num_indexes = _i32(surf, 24)
    if first_vert < 0 or num_verts < 0 or first_vert + num_verts > len(verts):
        raise BspError("surface vertex range is outside the drawvert lump")
    local = verts[first_vert:first_vert + num_verts]
    if surface_type in (MST_PLANAR, MST_TRIANGLE_SOUP):
        if num_indexes < 0 or first_index < 0 or first_index + num_indexes > len(indexes):
            raise BspError("surface index range is outside the drawindex lump")
        if num_indexes % 3:
            raise BspError("surface index count %d is not a multiple of 3" % num_indexes)
        local_indexes = indexes[first_index:first_index + num_indexes]
        for index in local_indexes:
            if index < 0 or index >= num_verts:
                raise BspError("surface index %d is outside its %d vertices" % (index, num_verts))
        return local, local_indexes
    if surface_type == MST_PATCH:
        width = _i32(surf, SURFACE_SIZE - 8)
        height = _i32(surf, SURFACE_SIZE - 4)
        if num_verts != width * height:
            raise BspError("patch has %d verts but size %dx%d" % (num_verts, width, height))
        return _tessellate_patch(local, width, height, level)
    raise BspError("unknown surface type %d" % surface_type)


def convert(data, out_dir, map_name, patch_level, scale, keep_system):
    _parse_header(data)
    shaders = _load_records(_lump(data, LUMP_SHADERS), SHADER_SIZE, "shaders")
    models = _load_records(_lump(data, LUMP_MODELS), MODEL_SIZE, "models")
    vert_blob = _lump(data, LUMP_DRAWVERTS)
    index_blob = _lump(data, LUMP_DRAWINDEXES)
    surfaces = _load_records(_lump(data, LUMP_SURFACES), SURFACE_SIZE, "surfaces")
    if len(vert_blob) % DRAWVERT_SIZE:
        raise BspError("drawvert lump length %d is not a multiple of %d" % (len(vert_blob), DRAWVERT_SIZE))
    if len(index_blob) % INDEX_SIZE:
        raise BspError("drawindex lump length %d is not a multiple of %d" % (len(index_blob), INDEX_SIZE))
    if not models:
        raise BspError("BSP has no models")
    verts = [_read_vert(vert_blob, i) for i in range(0, len(vert_blob), DRAWVERT_SIZE)]
    indexes = _read_ints(index_blob)
    shader_names = [_shader_name(raw[:64]) for raw in shaders]
    entities = _parse_entities(_lump(data, LUMP_ENTITIES))

    type_counts = {name: 0 for name in SURFACE_NAMES.values()}
    shader_tris = {}
    skipped_tris = 0
    model_info = []
    gltf_models = []

    for model_index, model in enumerate(models):
        mins = _vec3(model, 0)
        maxs = _vec3(model, 12)
        first = _i32(model, 24)
        count = _i32(model, 28)
        if first < 0 or count < 0 or first + count > len(surfaces):
            raise BspError("model %d surface range is outside the surface lump" % model_index)
        buckets = {}
        exported_surfaces = 0
        triangles = 0
        for surf in surfaces[first:first + count]:
            surface_type = _i32(surf, 8)
            type_counts[SURFACE_NAMES.get(surface_type, "other")] = type_counts.get(SURFACE_NAMES.get(surface_type, "other"), 0) + 1
            if surface_type == MST_FLARE:
                continue
            shader_num = _i32(surf, 0)
            if shader_num < 0 or shader_num >= len(shader_names):
                raise BspError("surface shader %d is outside the shader lump" % shader_num)
            shader = shader_names[shader_num] or ("shader_%d" % shader_num)
            geom_verts, geom_indexes = _surface_geometry(surf, verts, indexes, patch_level)
            tri_count = len(geom_indexes) // 3
            if (not keep_system) and _is_system_shader(shader):
                skipped_tris += tri_count
                continue
            if tri_count == 0:
                continue
            exported_surfaces += 1
            triangles += tri_count
            shader_tris[shader] = shader_tris.get(shader, 0) + tri_count
            buckets.setdefault(shader, Bucket()).add(geom_verts, geom_indexes)
        name = "world" if model_index == 0 else "*%d" % model_index
        model_info.append({
            "name": name,
            "index": model_index,
            "mins": list(mins),
            "maxs": list(maxs),
            "surfaces": exported_surfaces,
            "triangles": triangles,
        })
        gltf_models.append((name, buckets, triangles))

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_name = map_name + ".bin"
    gltf_path = out_dir / (map_name + ".gltf")
    bin_path = out_dir / bin_name
    _write_gltf(gltf_path, bin_path, bin_name, gltf_models, scale)
    entity_doc = {
        "map": map_name,
        "scale": scale,
        "axes": "game units, +X forward, +Y left, +Z up, untransformed",
        "entities": entities,
        "models": model_info,
    }
    (out_dir / (map_name + "_entities.json")).write_text(json.dumps(entity_doc, indent=2) + "\n", encoding="utf-8")
    report = _report(map_name, scale, model_info, type_counts, shader_tris, skipped_tris)
    (out_dir / (map_name + "_report.txt")).write_text(report, encoding="utf-8")
    sys.stdout.write(report)


def _report(map_name, scale, model_info, type_counts, shader_tris, skipped_tris):
    lines = ["map: %s" % map_name, "scale: %s" % scale]
    total = 0
    for info in model_info:
        label = "world" if info["index"] == 0 else info["name"]
        lines.append("triangles %s: %d" % (label, info["triangles"]))
        total += info["triangles"]
    lines.append("triangles total: %d" % total)
    lines.append("triangles skipped (textures/system): %d" % skipped_tris)
    for key in ("planar", "patch", "triangle soup", "flare"):
        lines.append("surfaces %s: %d" % (key, type_counts.get(key, 0)))
    lines.append("shaders:")
    for name in sorted(shader_tris):
        lines.append("  %s %d" % (name, shader_tris[name]))
    lines.append("")
    return "\n".join(lines)


def _write_gltf(gltf_path, bin_path, bin_name, gltf_models, scale):
    blob = bytearray()
    buffer_views = []
    accessors = []
    materials = []
    material_of = {}
    meshes = []
    nodes = []

    def align():
        while len(blob) % 4:
            blob.append(0)

    def push_bytes(data, target):
        align()
        offset = len(blob)
        blob.extend(data)
        buffer_views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data), "target": target})
        return len(buffer_views) - 1

    def push_vec3(values):
        flat = []
        mins = [float("inf")] * 3
        maxs = [float("-inf")] * 3
        for x, y, z in values:
            flat.extend((x, y, z))
            mins[0], maxs[0] = min(mins[0], x), max(maxs[0], x)
            mins[1], maxs[1] = min(mins[1], y), max(maxs[1], y)
            mins[2], maxs[2] = min(mins[2], z), max(maxs[2], z)
        view = push_bytes(_pack_floats(flat), 34962)
        accessors.append({
            "bufferView": view, "byteOffset": 0, "componentType": 5126,
            "count": len(values), "type": "VEC3", "min": mins, "max": maxs,
        })
        return len(accessors) - 1

    def push_vec2(values):
        flat = []
        for s, t in values:
            flat.extend((s, t))
        view = push_bytes(_pack_floats(flat), 34962)
        accessors.append({
            "bufferView": view, "byteOffset": 0, "componentType": 5126,
            "count": len(values), "type": "VEC2",
        })
        return len(accessors) - 1

    def push_color(values):
        raw = bytearray()
        for color in values:
            raw += bytes(int(c) & 255 for c in color)
        view = push_bytes(raw, 34962)
        accessors.append({
            "bufferView": view, "byteOffset": 0, "componentType": 5121,
            "count": len(values), "type": "VEC4", "normalized": True,
        })
        return len(accessors) - 1

    def push_indices(values):
        raw = array.array("I", values)
        if sys.byteorder != "little":
            raw.byteswap()
        view = push_bytes(raw.tobytes(), 34963)
        accessors.append({
            "bufferView": view, "byteOffset": 0, "componentType": 5125,
            "count": len(values), "type": "SCALAR",
        })
        return len(accessors) - 1

    for name, buckets, _tris in gltf_models:
        primitives = []
        for shader in sorted(buckets):
            bucket = buckets[shader]
            if shader not in material_of:
                material_of[shader] = len(materials)
                materials.append({
                    "name": shader,
                    "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 1},
                })
            positions = [_to_gltf(v.xyz[0], v.xyz[1], v.xyz[2], scale) for v in bucket.verts]
            normals = [_to_gltf(v.normal[0], v.normal[1], v.normal[2], 1.0) for v in bucket.verts]
            primitives.append({
                "attributes": {
                    "POSITION": push_vec3(positions),
                    "NORMAL": push_vec3(normals),
                    "TEXCOORD_0": push_vec2([v.st for v in bucket.verts]),
                    "TEXCOORD_1": push_vec2([v.lightmap for v in bucket.verts]),
                    "COLOR_0": push_color([v.color for v in bucket.verts]),
                },
                "indices": push_indices(bucket.indices),
                "material": material_of[shader],
                "mode": 4,
            })
        node = {"name": name}
        if primitives:
            meshes.append({"name": name, "primitives": primitives})
            node["mesh"] = len(meshes) - 1
        nodes.append(node)

    document = {
        "asset": {
            "version": "2.0",
            "generator": "bsp2gltf",
            "extras": {
                "scale": scale,
                "coordinateConversion": "gltf = (x, z, -y)",
                "sourceAxes": "+X forward, +Y left, +Z up",
            },
        },
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
    }
    if meshes:
        document["meshes"] = meshes
    if materials:
        document["materials"] = materials
    if accessors:
        document["accessors"] = accessors
        document["bufferViews"] = buffer_views
        document["buffers"] = [{"uri": bin_name, "byteLength": len(blob)}]
    gltf_path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    bin_path.write_bytes(blob)


def _find_in_pk3s(base, map_name):
    root = Path(base)
    if not root.is_dir():
        _die("base directory not found: %s" % root)
    wanted = "maps/%s.bsp" % map_name.lower()
    found = None
    source = None
    for pk3 in sorted(root.glob("*.pk3"), key=lambda path: path.name.lower()):
        try:
            archive = zipfile.ZipFile(str(pk3), "r")
        except zipfile.BadZipFile:
            _die("%s is not a pk3 zip" % pk3)
        with archive:
            for info in archive.infolist():
                name = info.filename.replace("\\", "/").lower()
                if name == wanted:
                    found = archive.read(info)
                    source = "%s:%s" % (pk3.name, info.filename)
    if found is None:
        _die("maps/%s.bsp not found in any pk3 under %s" % (map_name, root))
    return found, source


def main(argv):
    parser = argparse.ArgumentParser(description="Convert a Jedi Academy RBSP to glTF 2.0.")
    parser.add_argument("--base", help="GameData/base directory full of pk3 files")
    parser.add_argument("--map", help="map name without path or extension, for example t1_rail")
    parser.add_argument("--bsp", help="loose .bsp file, instead of searching pk3s")
    parser.add_argument("--out", required=True, help="directory for the gltf, bin, json, and report")
    parser.add_argument("--patch-level", type=int, default=4, help="Bezier subdivisions per 3x3 patch (default 4)")
    parser.add_argument("--scale", type=float, default=1.0, help="glTF units per game unit (default 1)")
    parser.add_argument("--keep-system", action="store_true", help="keep textures/system/* shaders")
    args = parser.parse_args(argv)
    if args.patch_level < 1:
        _die("--patch-level must be at least 1")
    if args.map and (Path(args.map).name != args.map or args.map.startswith(".")):
        _die("--map must be a bare name such as t1_rail")
    if args.bsp:
        path = Path(args.bsp)
        if not path.is_file():
            _die("bsp not found: %s" % path)
        data = path.read_bytes()
        map_name = args.map or path.stem
    elif args.base and args.map:
        data, _source = _find_in_pk3s(args.base, args.map)
        map_name = args.map
    else:
        _die("pass --bsp <file> or both --base <GameData/base> and --map <name>")
    try:
        convert(data, args.out, map_name, args.patch_level, args.scale, args.keep_system)
    except BspError as exc:
        _die(str(exc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
