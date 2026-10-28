#!/usr/bin/env python3
"""Check the native PE unwind chain used by Wine's protected syscall wrapper.

This checks linked metadata only. Windows execution must separately establish
that conversion faults return status and leave both guest threads usable.
"""
from pathlib import Path
import struct
import sys

data = Path(sys.argv[1]).read_bytes()
u16 = lambda at: struct.unpack_from('<H', data, at)[0]
u32 = lambda at: struct.unpack_from('<I', data, at)[0]
pe = u32(0x3c)
assert data[pe:pe + 4] == b'PE\0\0' and u16(pe + 4) == 0x8664, 'Expected AMD64 PE'
optional = pe + 24
assert u16(optional) == 0x20b, 'Expected PE32+'
sections = []
for i in range(u16(pe + 6)):
    at = optional + u16(pe + 20) + i * 40
    sections.append((u32(at + 12), u32(at + 8), u32(at + 20), u32(at + 16)))


def offset(rva, size=1):
    for virtual, _, raw, raw_size in sections:
        if virtual <= rva and rva + size <= virtual + raw_size:
            return raw + rva - virtual
    raise ValueError(f'RVA {rva:#x} with size {size:#x} has no file data')


# The cross build retains COFF names for Wine's private symbols and debugging.
symbols_at, symbol_count = u32(pe + 12), u32(pe + 16)
assert symbols_at and symbol_count, 'Keep COFF symbols for unwind verification'
strings = symbols_at + symbol_count * 18
symbols = {}
i = 0
while i < symbol_count:
    at = symbols_at + i * 18
    if u32(at) == 0:
        name_at = strings + u32(at + 4)
        name = data[name_at:data.index(b'\0', name_at)].decode('ascii')
    else:
        name = data[at:at + 8].split(b'\0', 1)[0].decode('ascii')
    section = struct.unpack_from('<h', data, at + 12)[0]
    if 1 <= section <= len(sections):
        symbols[name] = sections[section - 1][0] + u32(at + 8)
    i += 1 + data[at + 17]

directory = optional + 112 + 3 * 8  # IMAGE_DIRECTORY_ENTRY_EXCEPTION
pdata_rva, pdata_size = u32(directory), u32(directory + 4)
assert pdata_rva and pdata_size and pdata_size % 12 == 0, 'Missing AMD64 function table'
table = offset(pdata_rva, pdata_size)
functions = [struct.unpack_from('<III', data, at)
             for at in range(table, table + pdata_size, 12)]

for name in ('wow64_syscall', 'dispatch_nt', 'wow64_NtSetInformationThread', 'probe_NtOpenFile'):
    rva = symbols[name]
    matches = [(begin, end, unwind) for begin, end, unwind in functions if begin <= rva < end]
    assert len(matches) == 1, f'{name}: missing or ambiguous unwind range'
    begin, end, unwind = matches[0]
    info = offset(unwind, 4)
    assert data[info] & 7 == 1, f'{name}: unexpected UNWIND_INFO version'
    if name == 'wow64_syscall':
        assert data[info] >> 3 == 1, 'wow64_syscall must have UNW_FLAG_EHANDLER'
        handler_at = offset(unwind + 4 + ((data[info + 2] + 1) & ~1) * 2, 4)
        assert u32(handler_at) == symbols['wow64_syscall_handler'], 'Wrong protected syscall handler'
    print(f'Native unwind: {name} {begin:#x}..{end:#x} info={unwind:#x}')
