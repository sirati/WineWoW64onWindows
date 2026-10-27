#!/usr/bin/env python3
"""Compare runtime trees, ignoring PE checksums and DWARF compiler seed text."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def normalize(data):
    image = bytearray(data)
    ignored = []
    if len(image) < 64 or image[:2] != b'MZ':
        return image, ignored
    u16 = lambda offset: struct.unpack_from('<H', image, offset)[0]
    u32 = lambda offset: struct.unpack_from('<I', image, offset)[0]
    pe = u32(0x3c)
    if image[pe:pe + 4] != b'PE\0\0':
        return image, ignored
    optional = pe + 24
    if u16(optional) not in (0x10b, 0x20b):
        return image, ignored
    checksum = optional + 64
    image[checksum:checksum + 4] = b'\0' * 4
    ignored.append({'kind': 'PE checksum', 'offset': checksum, 'size': 4})
    strings = u32(pe + 12) + 18 * u32(pe + 16)
    sections = optional + u16(pe + 20)
    for index in range(u16(pe + 6)):
        header = sections + 40 * index
        name = bytes(image[header:header + 8]).rstrip(b'\0')
        if name.startswith(b'/'):
            start = strings + int(name[1:])
            name = bytes(image[start:image.index(0, start)])
        if name != b'.debug_info':
            continue
        start, size = u32(header + 20), u32(header + 16)
        for match in re.finditer(rb'-frandom-seed=([0-9a-z]{10})(?=\x00| )', bytes(image[start:start + size])):
            offset = start + match.start(1)
            image[offset:offset + 10] = b'0' * 10
            ignored.append({'kind': 'DWARF compiler seed', 'offset': offset, 'size': 10})
    return image, ignored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    args = parser.parse_args()
    roots = (args.reference, args.candidate)
    files = [{p.relative_to(root) for p in root.rglob('*') if p.is_file()} for root in roots]
    rows = []
    for path in sorted(files[0] | files[1]):
        row = {'path': path.as_posix()}
        if path not in files[0] or path not in files[1]:
            row.update(equivalent=False, reason='file absent from one tree')
        else:
            data = [(root / path).read_bytes() for root in roots]
            normalized = [normalize(contents) for contents in data]
            row.update(equivalent=normalized[0][0] == normalized[1][0],
                       identical=data[0] == data[1],
                       referenceSHA256=hashlib.sha256(data[0]).hexdigest(),
                       candidateSHA256=hashlib.sha256(data[1]).hexdigest(),
                       referenceIgnored=normalized[0][1], candidateIgnored=normalized[1][1])
        rows.append(row)
    equivalent = bool(rows) and all(row['equivalent'] for row in rows)
    print(json.dumps({'equivalent': equivalent, 'files': rows}, indent=2))
    return 0 if equivalent else 1


if __name__ == '__main__':
    raise SystemExit(main())
