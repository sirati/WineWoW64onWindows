#!/usr/bin/env python3
"""Extract Wine's DNS WoW64 marshalling without changing its algorithms."""
from pathlib import Path
import re
source = Path('wine-11.0-source/dlls/ws2_32/unixlib.c').read_text()
header = Path('wine-11.0-source/dlls/ws2_32/ws2_32_private.h').read_text()
params = header[header.index('struct getaddrinfo_params'):header.rindex('#endif')]
start = source.index('static int hostent_from_unix(')
end = source.index('\n}', start) + 2
host = source[start:end]
start = source.index('typedef ULONG PTR32;', source.index('#ifdef _WIN64'))
end = source.index('\n#endif  /* _WIN64 */', start)
Path('prototype/out/wine-socket-dns.inc').write_text(params + '\n' + host + '\n' + source[start:end] + '\n')
socket = Path('wine-11.0-source/dlls/ws2_32/socket.c').read_text()
start = socket.index('static DWORD NtStatusToWSAError(')
end = socket.index('\n}', start)
pairs = re.findall(r'\{\s*(STATUS_\w+)\s*,\s*(\w+)\s*\}', socket[start:end])
Path('prototype/out/wine-socket-errors.inc').write_text('/* Original Wine error pairs, searched in reverse by the native backend. */\n' +
    '\n'.join('{%s,%s},' % p for p in pairs) + '\n')
afd = Path('wine-11.0-source/include/wine/afd.h').read_text()
options = []
for name in re.findall(r'#define IOCTL_AFD_WINE_((?:GET|SET)_(?:SO|IP|IPV6|TCP)_\w+)', afd):
    direction, option = name.split('_', 1)
    level = {'SO': 'SOL_SOCKET', 'IP': 'IPPROTO_IP', 'IPV6': 'IPPROTO_IPV6', 'TCP': 'IPPROTO_TCP'}[option.split('_')[0]]
    options += [f'#ifdef {option}', f'{{IOCTL_AFD_WINE_{name},{level},{option},{int(direction == "SET")}}},', '#endif']
Path('prototype/out/wine-socket-options.inc').write_text('\n'.join(options) + '\n')
maps = []
for name in ['afd_poll_flag_from_win32', 'afd_poll_flag_to_win32']:
    start = socket.index('static unsigned int ' + name + '(')
    end = socket.index('\n}', start) + 2
    maps.append(socket[start:end])
Path('prototype/out/wine-socket-events.inc').write_text('\n'.join(maps) + '\n')
socket_unix = Path('wine-11.0-source/dlls/ntdll/unix/socket.c').read_text()
start = socket_unix.index('struct cmsghdr_32\n')
end = socket_unix.index('\n}', socket_unix.index('static int wow64_translate_control(', start)) + 2
Path('prototype/out/wine-socket-control.inc').write_text(socket_unix[start:end] + '\n')
