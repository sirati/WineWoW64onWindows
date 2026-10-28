#!/usr/bin/env python3
"""Reuse pinned Wine's DLL paths and builtin unixlib cache on Windows.

The PE loader still loads every guest image. Here only the host-library half
of find_builtin_dll is needed: its name normalization, build paths and DLL
path order. Native Windows uses ';', x86_64-native and LoadLibraryExW.
"""
from pathlib import Path
import re

root = Path('wine-11.0-source/dlls/ntdll/unix')
loader = (root / 'loader.c').read_text()
virtual = (root / 'virtual.c').read_text()

def function(source, name):
    match = re.search(r'^(?:static (?:inline )?)?[^\n;{}]*\b' + name +
                      r'\([^;{}]*\)\n\{.*?^\}', source, re.M | re.S)
    if not match:
        raise SystemExit('Missing original Wine function: ' + name)
    return match[0]

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit('Original Wine source changed: ' + old)
    return text.replace(old, new)

paths = function(loader, 'set_dll_path')
paths = replace_once(paths, 'getenv( "WINEDLLPATH" )', 'dll_path_environment')
paths = paths.replace("':'", "';'").replace('strtok( path, ":" )', 'strtok( path, ";" )')
paths = paths.replace('strtok( NULL, ":" )', 'strtok( NULL, ";" )')
helpers = '\n\n'.join(function(loader, name) for name in
                       ('build_path', 'prepend', 'prepend_build_dir_path', 'get_pe_dir', 'get_alt_machine'))
native_dir = replace_once(function(loader, 'get_so_dir'), '"/x86_64-unix"', '"/x86_64-native"')

# These are the original name normalization/allocation and path-prepending
# statements. PE mapping and ELF image loading are already handled elsewhere
# or inapplicable on Windows; do not introduce a second guest loader here.
find = function(loader, 'find_builtin_dll')
name = find[find.index('    if (!exp_name || !exp_name->Length)'):
            find.index('    TRACE( "looking for')]
name = replace_once(name, 'sizeof("/aarch64-windows")', 'sizeof("/x86_64-native")')
parts = [helpers, native_dir, paths, '''static NTSTATUS find_native_unixlib(
    void *module, UNICODE_STRING *nt_name, ANSI_STRING *exp_name )
{
    unsigned int i, pos, len, namepos = 0, maxlen = 0;
    char *ptr = NULL, *file, *ext = NULL;
    const char *pe_dir = get_pe_dir( IMAGE_FILE_MACHINE_I386 );
    const char *so_dir = get_so_dir( IMAGE_FILE_MACHINE_AMD64 );
    const char *pe_build_dir = build_dir;
    const USHORT search_machine = IMAGE_FILE_MACHINE_I386;
    NTSTATUS status = STATUS_DLL_NOT_FOUND;
''' + name + '''
    if (build_dir)
    {
        ptr = prepend_build_dir_path( file + pos, ".dll", so_dir, "/dlls", build_dir );
        if (native_unixlib_exists( ptr )) goto found;
        ptr = prepend_build_dir_path( file + pos, ".exe", so_dir, "/programs", build_dir );
        if (native_unixlib_exists( ptr )) goto found;
    }
    for (i = 0; dll_paths[i]; i++)
    {
        ptr = file + pos;
        ptr = prepend( ptr, so_dir, strlen(so_dir) );
        ptr = prepend( ptr, dll_paths[i], strlen(dll_paths[i]) );
        if (native_unixlib_exists( ptr )) goto found;
    }
    goto done;
found:
    status = load_builtin_unixlib( module, ptr );
done:
    free( file );
    return status;
}''']

cache_struct = re.search(r'^struct builtin_module\n\{.*?^\};\n\n'
                         r'static struct list builtin_modules[^;]+;', virtual, re.M | re.S)[0]
cache = '\n\n'.join([cache_struct] + [function(virtual, name) for name in
    ('add_builtin_module', 'release_builtin_module', 'get_builtin_unix_funcs', 'load_builtin_unixlib')])
# No 64-bit Wine callers exist. Every host table is the WoW64 table.
cache = replace_once(cache, 'wow ? "__wine_unix_call_wow64_funcs" : "__wine_unix_call_funcs"',
                     '"__wine_unix_call_wow64_funcs"')
Path('prototype/out/wine-unixlib-cache.inc').write_text(cache + '\n')
Path('prototype/out/wine-unixlib-paths.inc').write_text('\n\n'.join(parts) + '\n')
environment = (root / 'env.c').read_text()
Path('prototype/out/wine-native-dos-path.inc').write_text(function(environment, 'get_dos_path') + '\n')
debug = (root / 'debug.c').read_text()
Path('prototype/out/wine-debug-options.inc').write_text('\n\n'.join(
    function(debug, name) for name in ('add_option', 'parse_options')) + '\n')
