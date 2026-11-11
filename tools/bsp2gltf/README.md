# bsp2gltf

Turns a Jedi Academy `RBSP` map into glTF 2.0. Python 3.8 or newer. No packages to install.

The layout matches OpenJK `code/qcommon/qfiles.h`: ident `RBSP`, version 1, 18 lumps, 80-byte draw verts, 148-byte surfaces.

## Command

```
python tools\bsp2gltf\bsp2gltf.py --base "<GameData>\base" --map t1_rail --out "<dir>"
```

Or a loose bsp:

```
python tools\bsp2gltf\bsp2gltf.py --bsp "<path>\t1_rail.bsp" --out "<dir>"
```

`--base` is searched for `*.pk3`. Names are sorted A to Z. A later pk3 replaces the same `maps/<map>.bsp` from an earlier one. The files are opened read-only.

`--patch-level` (default 4) is how many times each 3x3 Bezier patch is split. `--scale` (default 1) is glTF units per game unit, stored in the glTF JSON. `--keep-system` keeps `textures/system/*` (caulk, clip, nodraw, skip, hint). Those are dropped by default.

## Files written

- `<map>.gltf` and `<map>.bin`. One node per model. The world node is `world`. Inline models are `*1`, `*2`, and so on, each with its own mesh. One primitive per shader. Material names are the shader paths. Vertices are `POSITION`, `NORMAL`, `TEXCOORD_0` (texture), `TEXCOORD_1` (lightmap slot 0), `COLOR_0`.
- `<map>_entities.json`. Every entity key and value, plus a `models` list for the world and each `*N`: mins, maxs, and the surface and triangle counts written into the glTF. Mins and maxs stay in game units (+X forward, +Y left, +Z up).
- `<map>_report.txt`. The same summary that is printed on screen: triangles for the world and each `*N`, surface counts by type, skipped system triangles, and shaders sorted by name.

glTF positions are `(x, z, -y)` times `--scale`, so Y is up. Triangle order is unchanged because that flip keeps the front face counter-clockwise.

Flares are not exported. A missing map or a file that is not RBSP version 1 exits with an error.
