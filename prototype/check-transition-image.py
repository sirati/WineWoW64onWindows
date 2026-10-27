#!/usr/bin/env python3
"""Reject transition images that require executable-page writes after mapping."""
from pathlib import Path
import struct
import sys

image = Path(sys.argv[1]).read_bytes()
u16 = lambda at: struct.unpack_from('<H', image, at)[0]
u32 = lambda at: struct.unpack_from('<I', image, at)[0]
pe = u32(0x3c)
assert image[pe:pe + 4] == b'PE\0\0' and u16(pe + 4) == 0x14c
optional = pe + 24
assert u16(optional) == 0x10b
assert u16(optional + 70) & 0x140 == 0x140, 'ASLR and NX must be enabled'
sections = []
for i in range(u16(pe + 6)):
    at = optional + u16(pe + 20) + i * 40
    sections.append((u32(at + 12), max(u32(at + 8), u32(at + 16)),
                     u32(at + 20), u32(at + 36)))

def section(rva):
    return next(s for s in sections if s[0] <= rva < s[0] + s[1])

def offset(rva):
    start, _, raw, _ = section(rva)
    return raw + rva - start

for _, _, _, flags in sections:
    assert flags & 0xa0000000 != 0xa0000000, 'Writable executable section'

exports = offset(u32(optional + 96))
functions = offset(u32(exports + 28))
names = offset(u32(exports + 32))
ordinals = offset(u32(exports + 36))
found = {}
for i in range(u32(exports + 24)):
    at = offset(u32(names + i * 4))
    name = image[at:image.index(0, at)].decode()
    found[name] = u32(functions + u16(ordinals + i * 2) * 4)
for name in ('probe_gate_entries', 'probe_gate_trampolines'):
    assert section(found[name])[3] & 0xa0000000 == 0x20000000, name
for name in ('probe_gate_far', 'probe_gate_targets'):
    assert section(found[name])[3] & 0xa0000000 == 0x80000000, name

relocations = u32(optional + 96 + 5 * 8)
length = u32(optional + 100 + 5 * 8)
assert relocations and length, 'Keep a data relocation for image rebasing'
at, end = offset(relocations), offset(relocations) + length
while at < end:
    page, size = u32(at), u32(at + 4)
    assert size >= 8 and at + size <= end
    for entry in range(at + 8, at + size, 2):
        relocation = u16(entry)
        if not relocation >> 12:
            continue
        assert relocation >> 12 == 3, 'Unexpected PE32 relocation type'
        assert not section(page + (relocation & 0xfff))[3] & 0x20000000, 'Executable relocation'
    at += size
print('Transition image: RX code, RW targets, no executable base relocations')
