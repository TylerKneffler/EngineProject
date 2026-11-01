"""Bake saved TerrainObjectLayer tile indexes and affine records in place.

Uses the engine's versioned little-endian cache format; source XML formatting
is preserved. Run with scene/prefab paths, or --check to verify existing caches.
"""
import argparse
import math
from pathlib import Path
import re
import struct
import xml.etree.ElementTree as ET

MASK = (1 << 64) - 1
OFFSET = 1469598103934665603


def hash_bytes(value, data):
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & MASK
    return value


def f32(value):
    return struct.unpack('<f', struct.pack('<f', float(value)))[0]


def vector(entry, name, fallback):
    node = entry.find(name)
    return [f32(child.text) for child in node] if node is not None else fallback


def bake_layer(node, span):
    entries = list(node.find('TerrainObjectLayer.placements'))
    signature, used, ids, records = OFFSET, set(), [], []
    for index, entry in enumerate(entries):
        asset = entry.attrib.get('asset', '').encode()
        position = vector(entry, 'position', [0., 0., 0.])
        normal = vector(entry, 'normal', [0., 1., 0.])
        yaw, scale = f32(entry.attrib.get('yawDegrees', 0)), f32(entry.attrib.get('scale', 1))
        identity = int(entry.attrib.get('id', 0))
        position_bytes = struct.pack('<3f', *position)
        if not identity or identity in used:
            identity = hash_bytes(hash_bytes(hash_bytes(OFFSET, asset), position_bytes), struct.pack('<I', index)) or 1
            while identity in used:
                identity = (identity + 1) & MASK
        used.add(identity)
        ids.append(identity)
        for data in (asset, position_bytes, struct.pack('<3f', *normal), struct.pack('<ffQ', yaw, scale, identity)):
            signature = hash_bytes(signature, data)
        length = math.sqrt(sum(v*v for v in normal))
        nx, ny, nz = [v/length for v in normal] if length > 1e-5 else [0., 1., 0.]
        # rotation(up, normal) multiplied by angleAxis(yaw, up), then normalized.
        align = (1., 0., 0., 0.) if ny < -0.999999 else (nz, 0., -nx, 1.+ny)
        norm = math.sqrt(sum(v*v for v in align))
        ax, ay, az, aw = [v/norm for v in align]
        sy, cy = math.sin(math.radians(yaw)/2), math.cos(math.radians(yaw)/2)
        x, y, z, w = ax*cy-az*sy, aw*sy+ay*cy, az*cy+ax*sy, aw*cy-ay*sy
        tile_x, tile_z = math.floor(position[0]/span), math.floor(position[2]/span)
        rows = [(1-2*(y*y+z*z))*scale, 2*(x*y-z*w)*scale, 2*(x*z+y*w)*scale, position[0]-tile_x*span,
                2*(x*y+z*w)*scale, (1-2*(x*x+z*z))*scale, 2*(y*z-x*w)*scale, position[1],
                2*(x*z-y*w)*scale, 2*(y*z+x*w)*scale, (1-2*(x*x+y*y))*scale, position[2]-tile_z*span]
        records.append(struct.pack('<Iii12f', index, tile_x, tile_z, *rows))
    return ids, (struct.pack('<IfIQ', 0x31494c54, span, len(entries), signature) + b''.join(records)).hex()


def bake(path, span=4., check=False):
    text = path.read_text(encoding='utf-8')
    nodes = [node for node in ET.fromstring(text).iter() if node.tag.split('}')[-1] == 'TerrainObjectLayer']
    pattern = re.compile(r'<(?P<tag>(?:\w+:)?TerrainObjectLayer)\b[^>]*>.*?</(?P=tag)>', re.S)
    matches = list(pattern.finditer(text))
    if len(matches) != len(nodes):
        raise ValueError(f'{path}: cannot map terrain layers to source XML')
    replacements = []
    for match, node in zip(matches, nodes):
        ids, blob = bake_layer(node, span)
        block = match.group()
        existing = node.attrib.get('TerrainObjectLayer.bakedInstances')
        entries = list(node.find('TerrainObjectLayer.placements'))
        if check:
            if existing != blob or any(entry.attrib.get('id') != str(identity) for entry, identity in zip(entries, ids)):
                raise ValueError(f'{path}: stale or missing instance cache')
            continue
        head_end = block.index('>')
        head = re.sub(r'\sTerrainObjectLayer\.bakedInstances="[^"]*"', '', block[:head_end])
        block = head + f' TerrainObjectLayer.bakedInstances="{blob}"' + block[head_end:]
        start = block.index('<TerrainObjectLayer.placements>')
        end = block.index('</TerrainObjectLayer.placements>', start)
        iterator = iter(ids)
        def assign_id(item):
            tag = re.sub(r'\sid="[^"]*"', '', item.group())
            return tag[:-1] + f' id="{next(iterator)}">'
        placements = re.sub(r'<item\s[^>]*\basset="[^>]*>', assign_id, block[start:end])
        block = block[:start] + placements + block[end:]
        replacements.append((match.start(), match.end(), block))
    for start, end, block in reversed(replacements):
        text = text[:start] + block + text[end:]
    if not check:
        with path.open('w', encoding='utf-8', newline='') as output:
            output.write(text)
    print(f'{path}: {len(nodes)} layer caches {"verified" if check else "baked"}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path)
    parser.add_argument('--tile-size', type=float, default=4.)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if not math.isfinite(args.tile_size) or args.tile_size < 1:
        parser.error('--tile-size must be finite and at least 1')
    for path in args.paths:
        bake(path, f32(args.tile_size), args.check)
