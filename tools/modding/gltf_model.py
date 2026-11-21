#!/usr/bin/env python3
"""glTF 2.0 models and PNG pictures read and written with the standard library
alone: what ai_level_convert.py needs of a model made in Blender (or by a
script) and nothing more.

Reading: a .gltf (JSON, with .bin buffers beside it or data: URIs) or a .glb;
nodes with their world transforms (translation, rotation, scale or a matrix,
through the hierarchy), meshes as triangle lists and line lists per primitive
(TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN; LINES, LINE_STRIP, LINE_LOOP) with
positions, normals, UVs and colours, materials with their base colour and
texture, and 8-bit PNG images (greyscale, RGB, palette, with or without alpha;
no interlace). glTF is right-handed with +y up; the caller maps the axes.

Writing: a Builder makes a .gltf with one .bin beside it from meshes given as
Python lists, and write_png writes RGBA rows: the way the example park and the
tests make their models.

Python 3.8+, standard library only.
"""
import base64
import json
import math
import os
import struct
import zlib
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


# ---- PNG ---------------------------------------------------------------------------

def read_png(data):
    """(width, height, rows) of an 8-bit PNG, each row a list of (r, g, b, a) tuples."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    at, chunks, idat, palette, trns = 8, {}, [], None, None
    while at < len(data):
        size, tag = struct.unpack_from(">I4s", data, at)
        body = data[at + 8:at + 8 + size]
        if tag == b"IHDR":
            chunks["IHDR"] = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif tag == b"tRNS":
            trns = body
        at += 12 + size
    width, height, depth, colour, _, _, interlace = chunks["IHDR"]
    if depth != 8 or interlace:
        raise ValueError("a PNG of %d bits%s: only 8-bit, non-interlaced PNGs are read" % (depth, ", interlaced" if interlace else ""))
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        filt = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        previous = line
        if colour == 0:
            row = [(v, v, v, 255) for v in line]
        elif colour == 2:
            row = [(line[i], line[i + 1], line[i + 2], 255) for i in range(0, stride, 3)]
        elif colour == 3:
            row = [palette[v] + ((trns[v] if trns and v < len(trns) else 255),) for v in line]
        elif colour == 4:
            row = [(line[i], line[i], line[i], line[i + 1]) for i in range(0, stride, 2)]
        else:
            row = [tuple(line[i:i + 4]) for i in range(0, stride, 4)]
        rows.append(row)
    return width, height, rows


def write_png(rows):
    """An 8-bit RGBA PNG of rows of (r, g, b, a)."""
    height, width = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(v for px in row for v in px) for row in rows)

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# ---- glTF reading -------------------------------------------------------------------

COMPONENT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


@dataclass
class Primitive:
    """One primitive of a mesh: its triangles (vertex index triples, counter-clockwise front) or line segments
    (index pairs), the vertex streams it indexes, and its material (an index into Model.materials, or None)."""
    positions: List[Tuple[float, float, float]]
    triangles: List[Tuple[int, int, int]] = field(default_factory=list)
    lines: List[Tuple[int, int]] = field(default_factory=list)
    normals: Optional[List[Tuple[float, float, float]]] = None
    uvs: Optional[List[Tuple[float, float]]] = None
    colours: Optional[List[Tuple[float, float, float, float]]] = None
    material: Optional[int] = None


@dataclass
class Mesh:
    name: str
    primitives: List[Primitive]
    extras: dict = field(default_factory=dict)


@dataclass
class Material:
    name: str
    base_colour: Tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0)
    texture: Optional[int] = None     # an index into Model.images
    extras: dict = field(default_factory=dict)


@dataclass
class Image:
    name: str
    width: int
    height: int
    rows: list                        # rows of (r, g, b, a), the top row first


@dataclass
class Node:
    """A node with its world transform: translation, rotation (a quaternion x, y, z, w) and scale, the
    hierarchy composed. A shear or a non-uniform scale under a rotation is not representable this way:
    those are composed into `matrix` only, and the decomposition is the nearest."""
    name: str
    translation: Tuple[float, float, float]
    rotation: Tuple[float, float, float, float]
    scale: Tuple[float, float, float]
    matrix: List[float]               # column-major 4x4, world
    mesh: Optional[int] = None
    extras: dict = field(default_factory=dict)
    children: List[int] = field(default_factory=list)


@dataclass
class Model:
    nodes: List[Node]
    meshes: List[Mesh]
    materials: List[Material]
    images: List[Image]


def quat_multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def quat_rotate(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx))


def compose(translation, rotation, scale):
    """A column-major 4x4 of T * R * S."""
    x, y, z, w = rotation
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    m = [0.0] * 16
    for col in range(3):
        for row in range(3):
            m[col * 4 + row] = r[row][col] * scale[col]
    m[12], m[13], m[14], m[15] = translation[0], translation[1], translation[2], 1.0
    return m


def multiply(a, b):
    out = [0.0] * 16
    for col in range(4):
        for row in range(4):
            out[col * 4 + row] = sum(a[k * 4 + row] * b[col * 4 + k] for k in range(4))
    return out


def decompose(m):
    """(translation, rotation, scale) of a column-major T * R * S matrix (the scale's sign from the determinant)."""
    translation = (m[12], m[13], m[14])
    cols = [(m[0], m[1], m[2]), (m[4], m[5], m[6]), (m[8], m[9], m[10])]
    scale = [math.sqrt(sum(c * c for c in col)) or 1.0 for col in cols]
    det = (cols[0][0] * (cols[1][1] * cols[2][2] - cols[1][2] * cols[2][1]) - cols[0][1] * (cols[1][0] * cols[2][2] - cols[1][2] * cols[2][0]) +
           cols[0][2] * (cols[1][0] * cols[2][1] - cols[1][1] * cols[2][0]))
    if det < 0:
        scale[0] = -scale[0]
    r = [[cols[c][row] / scale[c] for c in range(3)] for row in range(3)]
    trace = r[0][0] + r[1][1] + r[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        q = ((r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, 0.25 * s)
    elif r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        s = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2
        q = (0.25 * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s, (r[2][1] - r[1][2]) / s)
    elif r[1][1] > r[2][2]:
        s = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2
        q = ((r[0][1] + r[1][0]) / s, 0.25 * s, (r[1][2] + r[2][1]) / s, (r[0][2] - r[2][0]) / s)
    else:
        s = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2
        q = ((r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25 * s, (r[1][0] - r[0][1]) / s)
    return translation, q, tuple(scale)


class Reader:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            data = f.read()
        self.glb_bin = None
        if data[:4] == b"glTF":
            _, _, length = struct.unpack_from("<4sII", data, 0)
            at, doc = 12, None
            while at + 8 <= min(length, len(data)):
                size, kind = struct.unpack_from("<II", data, at)
                body = data[at + 8:at + 8 + size]
                if kind == 0x4E4F534A:
                    doc = json.loads(body.decode("utf-8"))
                elif kind == 0x004E4942:
                    self.glb_bin = body
                at += 8 + size
            self.doc = doc
        else:
            self.doc = json.loads(data.decode("utf-8"))
        self.buffers = [self.load_buffer(b, i) for i, b in enumerate(self.doc.get("buffers", []))]

    def load_uri(self, uri):
        if uri.startswith("data:"):
            return base64.b64decode(uri.split(",", 1)[1])
        with open(os.path.join(os.path.dirname(self.path), uri.replace("%20", " ")), "rb") as f:
            return f.read()

    def load_buffer(self, buffer, index):
        if "uri" not in buffer:
            if self.glb_bin is None or index != 0:
                raise ValueError("buffer %d has no uri and is not the GLB's binary chunk" % index)
            return self.glb_bin
        return self.load_uri(buffer["uri"])

    def view_bytes(self, view_index):
        view = self.doc["bufferViews"][view_index]
        data = self.buffers[view["buffer"]]
        at = view.get("byteOffset", 0)
        return data[at:at + view["byteLength"]], view.get("byteStride")

    def accessor(self, index):
        """The accessor's elements as tuples (scalars as plain numbers), normalized integers scaled to 0..1."""
        acc = self.doc["accessors"][index]
        fmt, size = COMPONENT[acc["componentType"]]
        width = WIDTH[acc["type"]]
        count = acc["count"]
        if "bufferView" not in acc:
            return [tuple([0.0] * width) if width > 1 else 0 for _ in range(count)]
        data, stride = self.view_bytes(acc["bufferView"])
        at = acc.get("byteOffset", 0)
        stride = stride or size * width
        out = []
        norm = acc.get("normalized", False)
        scale = {"B": 255.0, "H": 65535.0, "b": 127.0, "h": 32767.0}.get(fmt, 1.0) if norm else None
        for i in range(count):
            e = struct.unpack_from("<%d%s" % (width, fmt), data, at + i * stride)
            if scale:
                e = tuple(max(-1.0, v / scale) for v in e)
            out.append(e if width > 1 else e[0])
        return out

    def model(self):
        doc = self.doc
        images = []
        for i, img in enumerate(doc.get("images", [])):
            if "bufferView" in img:
                data, _ = self.view_bytes(img["bufferView"])
            else:
                data = self.load_uri(img["uri"])
            name = img.get("name") or os.path.splitext(os.path.basename(img.get("uri", "image_%d" % i)))[0]
            try:
                w, h, rows = read_png(data)
            except ValueError as e:
                raise ValueError("image %r: %s (the converter reads PNGs; save JPEGs as PNG)" % (name, e))
            images.append(Image(name, w, h, rows))
        materials = []
        for i, m in enumerate(doc.get("materials", [])):
            pbr = m.get("pbrMetallicRoughness", {})
            texture = None
            if "baseColorTexture" in pbr:
                texture = doc["textures"][pbr["baseColorTexture"]["index"]].get("source")
            materials.append(Material(m.get("name", "material_%d" % i), tuple(pbr.get("baseColorFactor", [1, 1, 1, 1])), texture, m.get("extras", {})))
        meshes = []
        for i, m in enumerate(doc.get("meshes", [])):
            prims = []
            for p in m.get("primitives", []):
                attrs = p["attributes"]
                positions = self.accessor(attrs["POSITION"])
                indices = self.accessor(p["indices"]) if "indices" in p else list(range(len(positions)))
                mode = p.get("mode", 4)
                prim = Primitive(positions, material=p.get("material"))
                if mode == 4:
                    prim.triangles = [tuple(indices[k:k + 3]) for k in range(0, len(indices) - 2, 3)]
                elif mode == 5:
                    prim.triangles = [(indices[k], indices[k + 1], indices[k + 2]) if k % 2 == 0 else (indices[k + 1], indices[k], indices[k + 2])
                                      for k in range(len(indices) - 2)]
                elif mode == 6:
                    prim.triangles = [(indices[0], indices[k], indices[k + 1]) for k in range(1, len(indices) - 1)]
                elif mode == 1:
                    prim.lines = [tuple(indices[k:k + 2]) for k in range(0, len(indices) - 1, 2)]
                elif mode == 3:
                    prim.lines = [(indices[k], indices[k + 1]) for k in range(len(indices) - 1)]
                elif mode == 2:
                    prim.lines = [(indices[k], indices[(k + 1) % len(indices)]) for k in range(len(indices))]
                else:
                    continue   # points
                prim.triangles = [t for t in prim.triangles if len(set(t)) == 3]
                if "NORMAL" in attrs:
                    prim.normals = self.accessor(attrs["NORMAL"])
                if "TEXCOORD_0" in attrs:
                    prim.uvs = self.accessor(attrs["TEXCOORD_0"])
                if "COLOR_0" in attrs:
                    prim.colours = [tuple(c) + ((1.0,) if len(c) == 3 else ()) for c in self.accessor(attrs["COLOR_0"])]
                prims.append(prim)
            meshes.append(Mesh(m.get("name", "mesh_%d" % i), prims, m.get("extras", {})))
        nodes = [None] * len(doc.get("nodes", []))
        identity = compose((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0), (1.0, 1.0, 1.0))

        def visit(index, parent):
            n = doc["nodes"][index]
            local = n["matrix"] if "matrix" in n else compose(tuple(n.get("translation", [0, 0, 0])), tuple(n.get("rotation", [0, 0, 0, 1])),
                                                              tuple(n.get("scale", [1, 1, 1])))
            world = multiply(parent, local)
            t, r, s = decompose(world)
            nodes[index] = Node(n.get("name", "node_%d" % index), t, r, s, world, n.get("mesh"), n.get("extras", {}), n.get("children", []))
            for c in n.get("children", []):
                visit(c, world)

        scenes = doc.get("scenes", [])
        roots = scenes[doc.get("scene", 0)]["nodes"] if scenes else [i for i in range(len(nodes))]
        for r in roots:
            visit(r, identity)
        for i, n in enumerate(nodes):
            if n is None:
                visit(i, identity)
        return Model(nodes, meshes, materials, images)


def read_model(path):
    return Reader(path).model()


# ---- glTF writing -------------------------------------------------------------------

class Builder:
    """A .gltf with one .bin: meshes of positions, normals, UVs and triangle or line indices, materials with
    a base colour and an optional PNG texture (a file beside the model), nodes with a transform and extras."""

    def __init__(self):
        self.bin = bytearray()
        self.views, self.accessors, self.meshes, self.materials, self.textures, self.images, self.nodes = [], [], [], [], [], [], []
        self.files = {}    # name -> bytes written beside the .gltf

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin += b"\0"
        self.views.append({"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data), **({"target": target} if target else {})})
        self.bin += data
        return len(self.views) - 1

    def accessor(self, elements, kind, component=5126, target=34962):
        width = WIDTH[kind]
        fmt = COMPONENT[component][0]
        flat = [v for e in elements for v in (e if width > 1 else (e,))]
        data = struct.pack("<%d%s" % (len(flat), fmt), *flat)
        acc = {"bufferView": self.view(data, target), "componentType": component, "count": len(elements), "type": kind}
        if component == 5126 and width > 1:
            acc["min"] = [min(e[i] for e in elements) for i in range(width)]
            acc["max"] = [max(e[i] for e in elements) for i in range(width)]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def image(self, name, rows):
        self.files[name + ".png"] = write_png(rows)
        self.images.append({"uri": name + ".png", "name": name})
        self.textures.append({"source": len(self.images) - 1})
        return len(self.textures) - 1

    def material(self, name, colour=(1.0, 1.0, 1.0, 1.0), texture=None, extras=None):
        pbr = {"baseColorFactor": list(colour), "metallicFactor": 0.0, "roughnessFactor": 1.0}
        if texture is not None:
            pbr["baseColorTexture"] = {"index": texture}
        m = {"name": name, "pbrMetallicRoughness": pbr}
        if extras:
            m["extras"] = extras
        self.materials.append(m)
        return len(self.materials) - 1

    def mesh(self, name, primitives, extras=None):
        """primitives: dicts of positions, normals?, uvs?, colours?, triangles | lines, material?."""
        prims = []
        for p in primitives:
            attrs = {"POSITION": self.accessor(p["positions"], "VEC3")}
            if p.get("normals"):
                attrs["NORMAL"] = self.accessor(p["normals"], "VEC3")
            if p.get("uvs"):
                attrs["TEXCOORD_0"] = self.accessor(p["uvs"], "VEC2")
            if p.get("colours"):
                attrs["COLOR_0"] = self.accessor(p["colours"], "VEC4")   # (r, g, b, a) floats, 1.0 the texture as it is
            if "lines" in p:
                flat = [i for seg in p["lines"] for i in seg]
                prim = {"attributes": attrs, "indices": self.accessor(flat, "SCALAR", 5125, 34963), "mode": 1}
            else:
                flat = [i for tri in p["triangles"] for i in tri]
                prim = {"attributes": attrs, "indices": self.accessor(flat, "SCALAR", 5125, 34963), "mode": 4}
            if p.get("material") is not None:
                prim["material"] = p["material"]
            prims.append(prim)
        m = {"name": name, "primitives": prims}
        if extras:
            m["extras"] = extras
        self.meshes.append(m)
        return len(self.meshes) - 1

    def node(self, name, mesh=None, translation=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0, 1.0), scale=(1.0, 1.0, 1.0), extras=None):
        n = {"name": name}
        if mesh is not None:
            n["mesh"] = mesh
        if tuple(translation) != (0.0, 0.0, 0.0):
            n["translation"] = list(translation)
        if tuple(rotation) != (0.0, 0.0, 0.0, 1.0):
            n["rotation"] = list(rotation)
        if tuple(scale) != (1.0, 1.0, 1.0):
            n["scale"] = list(scale)
        if extras:
            n["extras"] = extras
        self.nodes.append(n)
        return len(self.nodes) - 1

    def write(self, path):
        stem = os.path.splitext(os.path.basename(path))[0]
        doc = {"asset": {"version": "2.0", "generator": "gltf_model.py"}, "scene": 0, "scenes": [{"nodes": list(range(len(self.nodes)))}],
               "nodes": self.nodes, "meshes": self.meshes, "materials": self.materials, "accessors": self.accessors,
               "bufferViews": self.views, "buffers": [{"uri": stem + ".bin", "byteLength": len(self.bin)}]}
        if self.images:
            doc["images"], doc["textures"] = self.images, self.textures
        folder = os.path.dirname(os.path.abspath(path))
        os.makedirs(folder, exist_ok=True)
        with open(path, "w") as f:
            json.dump(doc, f, indent=1)
        with open(os.path.join(folder, stem + ".bin"), "wb") as f:
            f.write(self.bin)
        for name, data in self.files.items():
            with open(os.path.join(folder, name), "wb") as f:
                f.write(data)


def selftest():
    rows = [[(x * 2, y * 2, 128, 255 if x < 50 else 0) for x in range(100)] for y in range(60)]
    w, h, back = read_png(write_png(rows))
    assert (w, h) == (100, 60) and back == rows
    q = (0.0, math.sin(math.radians(45)), 0.0, math.cos(math.radians(45)))   # 90 degrees about y
    t, r, s = decompose(compose((1.0, 2.0, 3.0), q, (2.0, 2.0, 2.0)))
    assert all(abs(a - b) < 1e-6 for a, b in zip(t, (1.0, 2.0, 3.0))) and all(abs(a - b) < 1e-6 for a, b in zip(r, q)) and all(abs(v - 2.0) < 1e-6 for v in s)
    v = quat_rotate(q, (1.0, 0.0, 0.0))
    assert all(abs(a - b) < 1e-6 for a, b in zip(v, (0.0, 0.0, -1.0))), v
    print("gltf_model selftest: ok")


if __name__ == "__main__":
    selftest()
