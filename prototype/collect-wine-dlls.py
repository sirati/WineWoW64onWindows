#!/usr/bin/env python3
"""Copy preserved stock Wine DLLs and their static import dependencies."""
import json
import pathlib
import shutil
import struct
import sys

root = pathlib.Path(sys.argv[1])
out = pathlib.Path("prototype/stock-wine/i386-windows")
files = {p.name.lower(): p for p in root.iterdir() if p.suffix.lower() in ('.dll', '.drv')}
seen = set()
todo = ["ntdll.dll", "kernelbase.dll", "kernel32.dll", "msvcrt.dll", "oleaut32.dll",
        "user32.dll", "advapi32.dll", "ws2_32.dll", "version.dll", "msvcr100.dll",
        "opengl32.dll", "winmm.dll", "comdlg32.dll", "wsock32.dll", "dsound.dll",
        "bcryptprimitives.dll", "userenv.dll"]

def dependencies(path):
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
    result = []
    rva = u32(opt + 104)
    if rva:
        at = offset(rva)
        while u32(at + 12):
            name = offset(u32(at + 12))
            result.append(data[name:data.index(0, name)].decode().lower())
            at += 20
    # Wine resolves forwarded exports on demand. Their DLLs must be in the
    # distribution too, even though they have no import-table entry.
    rva, size = u32(opt + 96), u32(opt + 100)
    if rva:
        exports = offset(rva)
        functions = offset(u32(exports + 28))
        for i in range(u32(exports + 20)):
            target = u32(functions + i * 4)
            if rva <= target < rva + size:
                name = offset(target)
                forward = data[name:data.index(0, name)].decode()
                module = forward.rsplit('.', 1)[0].lower()
                # Wine also exports entry points for its separate 16-bit
                # runtime. This port runs x86 guests only.
                if module.endswith(('.exe16', '.dll16', '.drv16')):
                    continue
                name = module if '.' in module else module + '.dll'
                # These names resolve through the existing API-set map.
                if name not in files and name.startswith(('api-ms-', 'ext-ms-')):
                    continue
                result.append(name)
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
    todo.extend(dependencies(source))
pathlib.Path("evidence/wine-import-dependencies.json").write_text(json.dumps(sorted(seen), indent=2) + "\n")
print("Wine dependency DLLs:", ", ".join(sorted(seen)))
