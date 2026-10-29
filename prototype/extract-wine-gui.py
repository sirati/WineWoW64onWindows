#!/usr/bin/env python3
"""Select Wine's own GUI-server and client routines for the in-process build.

Wine's window, queue, class, hook, atom, winstation, user, region,
clipboard, object, handle, directory, event and symlink server units compile
unchanged. Their remaining Unix-server dependencies are taken verbatim from
the original sources here, so only the transport is adapted in gui_server.c.
"""
from pathlib import Path
import re

wine = Path('wine-11.0-source')
out = Path('prototype/out')

def fn(source, name):
    """Return one top-level definition, including a multiline signature."""
    # Skip comment lines that name the function, as Wine's headers do.
    match = re.search(r'^(?![ \t]*[/*])[^\n;#{}]*\b' + re.escape(name) + r'\s*\([^;{]*\)\s*\n\{.*?^\}',
                      source, re.M | re.S)
    if not match:
        raise SystemExit('Wine routine missing: ' + name)
    return match[0]

def block(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise SystemExit('Wine definition missing: ' + pattern)
    return match[0]

server = {name: (wine / 'server' / (name + '.c')).read_text()
          for name in ('fd', 'request', 'unicode', 'thread', 'token', 'mapping', 'process')}

routines = []
# fd.c: the server main loop's timeout list.
fd = server['fd']
routines.append(block(fd, r'^struct timeout_user\n\{.*?^\};'))
routines.append(block(fd, r'^static struct list abs_timeout_list.*?\n'))
routines.append(block(fd, r'^static struct list rel_timeout_list.*?\n'))
routines.append(block(fd, r'^timeout_t current_time;\ntimeout_t monotonic_time;\n'))
routines.append(block(fd, r'^static const timeout_t user_shared_data_timeout.*?\n'))
for name in ('add_timeout_user', 'remove_timeout_user', 'get_timeout_str', 'get_next_timeout'):
    routines.append(fn(fd, name))
# request.c: request data accessors used by the handlers.
request = server['request']
routines.append(fn(request, 'set_reply_data_size'))
routines.append(block(request, r'^static const struct object_attributes empty_attributes;\n'))
for name in ('get_req_object_attributes', 'get_req_data_after_objattr'):
    routines.append(fn(request, name))
# unicode.c: case-insensitive object names over Wine's l_intl.nls table.
unicode = server['unicode']
routines.append(block(unicode, r'^static unsigned short \*casemap;\n'))
for name in ('to_lower', 'memicmp_strW', 'hash_strW', 'ascii_to_unicode_str', 'dump_strW'):
    routines.append(fn(unicode, name))
# thread.c: sync signalling.
for name in ('signal_sync', 'reset_sync'):
    routines.append(fn(server['thread'], name))
# token.c: descriptor validation and label handling for named objects.
token = server['token']
for name in ('acl_is_valid', 'sd_is_valid', 'extract_security_labels', 'replace_security_labels'):
    routines.append(fn(token, name))
# process.c: the input-idle event request used by NtUserWaitForInputIdle.
routines.append(block(server['process'], r'^DECL_HANDLER\(get_process_idle_event\)\n\{.*?^\}'))
(out / 'wine-gui-server-routines.inc').write_text('\n\n'.join(routines) + '\n')

# mapping.c: the shared session allocator. Mapping creation and growth are
# native sections in gui_server.c.
mapping = server['mapping']
session = [block(mapping, r'^struct session_block\n\{.*?^\};'),
           block(mapping, r'^struct session_object\n\{.*?^\};'),
           block(mapping, r'^struct session\n\{.*?^\};'),
           block(mapping, r'^session_shm_t \*shared_session;\n'),
           block(mapping, r'^static struct session session =\n\{.*?^\};'),
           fn(mapping, 'round_size')]
for name in ('find_free_session_block', 'find_free_session_object', 'alloc_shared_object',
             'free_shared_object', 'invalidate_shared_object', 'get_shared_object_locator'):
    session.append(fn(mapping, name))
(out / 'wine-gui-session.inc').write_text('\n\n'.join(session) + '\n')

# Dispatch only the requests implemented by the compiled server units.
units = ('atom', 'class', 'clipboard', 'directory', 'event', 'handle', 'hook', 'object',
         'queue', 'region', 'symlink', 'user', 'window', 'winstation')
handlers = sorted({name for unit in units
                   for name in re.findall(r'^DECL_HANDLER\((\w+)\)', (wine / 'server' / (unit + '.c')).read_text(), re.M)}
                  | {'get_process_idle_event'})
(out / 'wine-gui-request-table.inc').write_text(
    ''.join(f'DECL_HANDLER({name});\n' for name in handlers) +
    'static const req_handler req_handlers[REQ_NB_REQUESTS] =\n{\n' +
    ''.join(f'    [REQ_{name}] = (req_handler)req_{name},\n' for name in handlers) + '};\n')

# ntdll/unix/sync.c: Wine's client side of the directory and atom requests.
sync = (wine / 'dlls/ntdll/unix/sync.c').read_text()
client = [fn(sync, 'validate_open_object_attributes'), fn(sync, 'NtOpenDirectoryObject'),
          block(sync, r'^#define MAX_ATOM_LEN.*?\n#define IS_INTATOM.*?\n'),
          fn(sync, 'is_integral_atom'), fn(sync, 'integral_atom_name')]
for name in ('NtAddAtom', 'NtDeleteAtom', 'NtFindAtom', 'NtQueryInformationAtom'):
    client.append(fn(sync, name))
(out / 'wine-gui-client.inc').write_text('\n\n'.join(client) + '\n')

# wow64/syscall.c: the original x86 argument thunks for the atom services.
syscall = (wine / 'dlls/wow64/syscall.c').read_text()
(out / 'wine-gui-atom-thunks.inc').write_text('\n\n'.join(
    fn(syscall, name) for name in ('wow64_NtAddAtom', 'wow64_NtDeleteAtom', 'wow64_NtFindAtom',
                                   'wow64_NtQueryInformationAtom')) + '\n')
# wow64/process.c: the original x86-to-x64 process parameter conversion.
process = (wine / 'dlls/wow64/process.c').read_text()
(out / 'wine-gui-process-params.inc').write_text(fn(process, 'process_params_32to64') + '\n')
print(f'Wine GUI server: {len(handlers)} request handlers selected')
