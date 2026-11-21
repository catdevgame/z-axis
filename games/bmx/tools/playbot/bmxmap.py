#!/usr/bin/env python3
"""BMX XXX's levels as the player sees them: dm2map.py over BMX's level files. Its objects mostly hold no TGFN
(TIFN, indexed normals, instead): a face's normal comes from its corners, in the winding TGFN has where both exist.

    bmxmap.py build [LEVEL ...]   the eight levels by default; pickles in the scratch folder, never in a repository
    bmxmap.py info LEVEL
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
CORE = os.environ.get('PLAYBOT_CORE', zpaths.TOOL_DIRS['playbot'])   # (core.py: tools/playbot)
sys.path.insert(0, CORE)
SCRATCH = os.environ.get('BMXCORPUS', os.path.join(zpaths.ZAXIS_SCRATCH, 'bmxcorpus'))
os.environ['PLAYDM2'] = SCRATCH
os.environ.setdefault('DM2_DISC', os.environ.get('BMX_DISC', os.path.join(zpaths.BMX_DATA, 'disc')))
import dm2map  # noqa: E402

LEVELS = ('CHL_SYRA', 'CHL_SHEP', 'CHL_DAM', 'CHL_MNCO', 'CHL_MULY', 'CHL_NASA', 'CMP_RMPG', 'CMP_ROOT')


def face_normal(verts, corners):
    if max(corners[:3]) >= len(verts):     # (flags 0x33xx: see objects_of)
        return (0.0, 0.0, 0.0)
    a, b, c = (verts[i] for i in corners[:3])
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    length = math.sqrt(sum(x * x for x in n)) or 1.0
    return tuple(x / length for x in n)


FACE_COLLIDES = 0x2000   # the engine's kFaceCollides (engine/zge/collision/collision.h)


def with_normals(geo):
    """The level's chunks, each object's TGFN written from its corners where it has none, and {id of a TGCI chunk's
    data: its part's face flags}. The collision trees (TGCI) are left as they are: dm2_level.tree_faces reads BMX
    XXX's own (ZGE4: offset leaves, a name hash first)."""
    import dm2_level as dl
    root = dl.parse(geo)[0]
    flags = {}
    for obj in root.kids:
        if obj.tag != b'TGEO':
            continue
        parts = [k for k in obj.kids if k.tag == b'TGPT']
        if not parts:
            continue
        streams = {k.tag: k.data for k in parts[-1].kids}
        nv, nf = struct.unpack_from('<HH', streams[b'TGPH'], 4)
        for tree in (k for k in obj.kids if k.tag == b'TGCI'):
            flags[id(tree.data)] = [struct.unpack_from('<H', streams[b'TGFP'], 8 * i)[0] for i in range(nf)]
        if any(k.tag == b'TGFN' for k in list(parts[-1].kids) + list(obj.kids)):
            continue
        verts = [struct.unpack_from('<3f', streams[b'TGVP'], 12 * i) for i in range(nv)]
        data = b''.join(struct.pack('<3f', *face_normal(verts, struct.unpack_from('<4H', streams[b'TGFP'], 8 * i)[1:]))
                        for i in range(nf))
        parts[-1].kids.append(Chunk(b'TGFN', data))
    return root, flags


class Chunk:
    def __init__(self, tag, data):
        self.tag, self.data, self.kids = tag, data, []


_objects_of = dm2map.objects_of


def objects_of(geo):
    """dm2map.objects_of over BMX XXX's level: the faces its trees list that collide. BMX XXX's trees also list faces
    without FACE_COLLIDES (0x5300: the hanging lights of CHL_DAM's hall, 2 units over its floor), which the engine's
    segment and sphere tests pass over (engine/zge/collision/faces.cpp: unless the query asks for every face, as only
    the rider's edge walks do); DM2's list only colliding ones."""
    import dm2_level as dl
    root, flags = with_normals(geo)
    parse, tree_faces = dl.parse, dl.tree_faces

    def colliding(data):
        listed = tree_faces(data, offset_leaves=True)     # (ZGE4's form, whatever the record's first word)
        face_flags = flags.get(id(data))
        if face_flags is None:
            return listed
        return {i for i in listed if i < len(face_flags) and face_flags[i] & FACE_COLLIDES}

    dl.parse, dl.tree_faces = (lambda data: [root]), colliding
    try:
        objects = _objects_of(geo)
    finally:
        dl.parse, dl.tree_faces = parse, tree_faces
    # (faces with flags 0x33xx index past the part's vertices: left out, 1 in 150; what they are is not known)
    return {k: (verts, [f for f in faces if max(f[0]) < len(verts)]) for k, (verts, faces) in objects.items()}


dm2map.objects_of = objects_of

if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'build':
        for level in a[1:] or LEVELS:
            dm2map.build(level)
    elif a and a[0] == 'info':
        lv = dm2map.Level(a[1])
        print(lv.name, len(lv.nodes), 'cells', len(lv.rails), 'rails', len(lv.ramps), 'ramp faces')
