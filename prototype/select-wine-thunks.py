#!/usr/bin/env python3
"""Select original Wine thunks without pulling in excluded services via .pdata.

PE unwind tables can retain otherwise unused functions in a COFF object.
Keep the original helpers and supported thunk bodies; omit other thunk
definitions from generated translation units before enabling unwind data.
"""
from pathlib import Path
import re

root = Path('wine-11.0-source/dlls/wow64')
backend = Path('prototype/backend.c').read_text()
dispatch = backend[backend.index('static NTSTATUS WINAPI dispatch_nt('):
                   backend.index('extern NTSTATUS wow64_syscall(')]
selected = set(re.findall(r'\b(wow64_\w+)\s*\(', dispatch))
units = ('file', 'process', 'registry', 'security', 'sync', 'system', 'virtual')
pattern = re.compile(r'^NTSTATUS WINAPI (wow64_\w+)\([^;{}]*\)\n\{.*?^\}\n?', re.M | re.S)
sources = {unit: (root / (unit + '.c')).read_text() for unit in units}
definitions = {match[1]: match[0] for source in sources.values() for match in pattern.finditer(source)}
while True:
    dependencies = {name for current in selected & definitions.keys()
                    for name in re.findall(r'\b(wow64_\w+)\s*\(', definitions[current])}
    if dependencies <= selected:
        break
    selected |= dependencies

retained = 0
for unit, source in sources.items():
    def keep(match):
        # Preserve upstream line numbers for debugger/source attribution.
        return match[0] if match[1] in selected else '\n' * match[0].count('\n')
    filtered = pattern.sub(keep, source)
    retained += sum(match[1] in selected for match in pattern.finditer(source))
    Path('prototype/out/wine-' + unit + '.c').write_text(
        '#line 1 "' + (root / (unit + '.c')).as_posix() + '"\n' + filtered)
print(f'Wine thunks: {retained}/{len(definitions)} original definitions selected with native unwind data')

# The native I/O adapter stores the original open options in file_state.
# Reuse the server's mode predicate for completion association/notification.
fd = Path('wine-11.0-source/server/fd.c').read_text()
overlapped = re.search(r'^int is_fd_overlapped\( struct fd \*fd \)\n\{.*?^\}', fd, re.M | re.S)
if not overlapped:
    raise SystemExit('Wine is_fd_overlapped predicate missing')
Path('prototype/out/wine-file-overlapped.inc').write_text(
    'static ' + overlapped[0].replace('struct fd *fd', 'struct file_state *fd') + '\n')

# The PE loader uses this identity even when the filesystem has never assigned
# a Windows object-ID GUID. Keep Wine's device/inode format and size checks;
# the native filesystem supplies its volume serial and stable file index.
file = Path('wine-11.0-source/dlls/ntdll/unix/file.c').read_text()
begin = file.index('    case FSCTL_GET_OBJECT_ID:\n')
end = file.index('\n    case FSCTL_SET_SPARSE:', begin)
identity = file[begin:end]
identity = identity.replace('        int fd, needs_close;\n        struct stat st;',
                            '        struct native_file_identity st;')
unix_query = '''            status = server_get_unix_fd( handle, 0, &fd, &needs_close, NULL, NULL );
            if (status) break;
            fstat( fd, &st );
            if (needs_close) close( fd );'''
assert identity.count(unix_query) == 1, 'Wine file identity query changed'
identity = identity.replace(unix_query, '''            status = query_native_file_identity( handle, &st );
            if (status) break;''')
Path('prototype/out/wine-file-identity.inc').write_text(identity + '\n')

# Retain Wine's per-table service bounds even before every valid entry has a
# native adapter. Unknown valid services and invalid service IDs differ.
limits=[]
for module,first in [('ntdll',0),('win32u',0x1000)]:
    header=Path('wine-11.0-source/dlls')/module/('ntsyscalls.h' if module=='ntdll' else 'win32syscalls.h')
    macro=header.read_text().split('#define ALL_SYSCALLS32',1)[1].split('\n#define',1)[0]
    ids=[int(value,16) for value in re.findall(r'SYSCALL_ENTRY(?:_\w+)?\(\s*(0x[0-9a-f]+),',macro)]
    assert ids and ids==list(range(first,first+len(ids))), 'Wine service IDs are not contiguous'
    limits.append(len(ids))
Path('prototype/out/wine-service-limits.inc').write_text(
    'static const UINT service_limits[4]={'+','.join(map(str,limits))+',0,0};\n')
