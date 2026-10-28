#!/usr/bin/env python3
"""Copy preserved stock Wine DLLs and their static import dependencies."""
import json
import pathlib
import shutil
import struct
import sys

root = pathlib.Path(sys.argv[1])
out = pathlib.Path("prototype/stock-wine/i386-windows")
files = {p.name.lower(): p for p in root.glob("*.dll")}
seen = set()
todo = ["ntdll.dll", "kernelbase.dll", "kernel32.dll", "msvcrt.dll", "oleaut32.dll",
        "user32.dll", "advapi32.dll", "ws2_32.dll", "version.dll", "msvcr100.dll",
        "opengl32.dll", "winmm.dll", "comdlg32.dll", "wsock32.dll", "dsound.dll",
        "bcryptprimitives.dll", "userenv.dll"]

def imports(path):
    data = path.read_bytes()
    u32 = lambda at: struct.unpack_from("<I", data, at)[0]
    pe = u32(0x3c)
    opt = pe + 24
    sections = []
    for i in range(struct.unpack_from("<H", data, pe + 6)[0]):
        section = opt + struct.unpack_from("<H", data, pe + 20)[0] + i * 40
        sections.append((u32(section + 12), max(u32(section + 8), u32(section + 16)), u32(section + 20)))
    def offset(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError(rva)
    rva = u32(opt + 104)
    if not rva:
        return []
    at = offset(rva)
    result = []
    while u32(at + 12):
        name = offset(u32(at + 12))
        result.append(data[name:data.index(0, name)].decode().lower())
        at += 20
    return result

while todo:
    name = todo.pop()
    if name in seen:
        continue
    seen.add(name)
    if name not in files:
        raise SystemExit(f"Missing Wine DLL {name}")
    source = files[name]
    if not (out / source.name).exists():
        shutil.copyfile(source, out / source.name)
    todo.extend(imports(source))
pathlib.Path("evidence/wine-import-dependencies.json").write_text(json.dumps(sorted(seen), indent=2) + "\n")
print("Wine dependency DLLs:", ", ".join(sorted(seen)))
