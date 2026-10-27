#!/usr/bin/env python3
"""Reject ABI drift when rebuilding a pinned Wine DLL with another linker."""
import struct
import sys
from pathlib import Path

def exports(path):
    data = Path(path).read_bytes()
    pe, = struct.unpack_from('<I', data, 0x3c)
    count, = struct.unpack_from('<H', data, pe + 6)
    opt_size, = struct.unpack_from('<H', data, pe + 20)
    rva, size = struct.unpack_from('<II', data, pe + 24 + 96)
    assert rva and size, f'{path}: missing export directory'
    sections = [struct.unpack_from('<8sIIII', data, pe + 24 + opt_size + i * 40) for i in range(count)]
    def offset(addr):
        for _, virtual_size, virtual, raw_size, raw in sections:
            if virtual <= addr < virtual + max(virtual_size, raw_size):
                return raw + addr - virtual
        raise ValueError(f'{path}: invalid RVA {addr:x}')
    def string(addr):
        start = offset(addr)
        return data[start:data.index(b'\0', start)].decode('ascii')
    base, funcs, names, addr_funcs, addr_names, ordinals = struct.unpack_from('<IIIIII', data, offset(rva) + 16)
    result = {}
    for i in range(names):
        name = string(struct.unpack_from('<I', data, offset(addr_names) + i * 4)[0])
        ordinal, = struct.unpack_from('<H', data, offset(ordinals) + i * 2)
        address, = struct.unpack_from('<I', data, offset(addr_funcs) + ordinal * 4)
        result[name] = (base + ordinal, string(address).lower() if rva <= address < rva + size else None)
    return result

before, after = map(exports, sys.argv[1:])
differences = {name: (before.get(name), after.get(name)) for name in before.keys() | after.keys()
               if before.get(name) != after.get(name)}
assert not differences, f'Wine export ABI mismatch: {differences}'
print(f'Wine export ABI verified: {len(after)} names, ordinals and forwarders')
