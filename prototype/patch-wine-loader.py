#!/usr/bin/env python3
"""Apply the native-Windows file-access change to the pinned Wine PE build.

Source equivalent in ntdll/loader.c, open_dll_file:
  GENERIC_READ | SYNCHRONIZE -> GENERIC_READ | GENERIC_EXECUTE | SYNCHRONIZE
Wine's Unix section implementation permits read-only handles for SEC_IMAGE;
Windows NtCreateSection requires FILE_EXECUTE as well. Keep the preserved
stock DLL unchanged and reject unexpected compiler output.
"""
import pathlib
import struct
import sys

path = pathlib.Path(sys.argv[1])
data = bytearray(path.read_bytes())
u16 = lambda at: struct.unpack_from("<H", data, at)[0]
u32 = lambda at: struct.unpack_from("<I", data, at)[0]
pe = u32(0x3c)
symbol_at, symbol_count = u32(pe + 12), u32(pe + 16)
strings = symbol_at + symbol_count * 18
section_table = pe + 24 + u16(pe + 20)
symbol = None
index = 0
while index < symbol_count:
    at = symbol_at + index * 18
    if u32(at) == 0:
        start = strings + u32(at + 4)
        name = bytes(data[start:data.index(0, start)])
    else:
        name = bytes(data[at:at + 8]).split(b"\0")[0]
    if name == b"_open_dll_file.part.0":
        section = struct.unpack_from("<h", data, at + 12)[0]
        header = section_table + (section - 1) * 40
        symbol = u32(header + 20) + u32(at + 8)
        break
    index += 1 + data[at + 17]
if symbol is None:
    raise SystemExit("Wine open_dll_file symbol missing")
old = bytes.fromhex("c7 44 24 04 00 00 10 80")
new = bytes.fromhex("c7 44 24 04 00 00 10 a0")
body = data[symbol:symbol + 256]
if body.count(old) != 1:
    raise SystemExit("Wine loader access instruction changed")
at = symbol + body.index(old)
data[at:at + len(old)] = new
path.write_bytes(data)
print(f"Wine loader FILE_EXECUTE port patch at file offset {at:#x}")
