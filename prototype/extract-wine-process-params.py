#!/usr/bin/env python3
"""Keep Wine's command-line builder and WoW64 process-parameter conversion."""
from pathlib import Path
import re

source = Path('wine-11.0-source/dlls/ntdll/unix/env.c').read_text()
def function(name):
    match = re.search(r'^static [^\n;{}]*\b' + name +
                      r'\([^;{}]*\)\n\{.*?^\}', source, re.M | re.S)
    if not match:
        raise SystemExit('Missing original Wine function: ' + name)
    return match[0]

start = source.index('#ifdef _WIN64\nstatic inline void dup_unicode_string(')
end = source.index('\n}', start) + 2
Path('prototype/out/wine-process-params.inc').write_text('\n\n'.join([
    function('build_command_line'), source[start:end],
    function('build_wow64_parameters')]) + '\n')

# Preserve Wine's serialized startup format and reconstruction. Only the
# server transport and Unix process spawning are supplied by the native host.
create = re.search(r'^void \*create_startup_info\([^;{}]*\)\n\{.*?^\}', source, re.M | re.S)
assert create, 'Missing Wine startup serializer'
init = source[source.index('void init_startup_info(void)'):]
begin = init.index('    size = (sizeof(*params)')
end = init.index('    free( env );', begin)
reconstruct = '''static RTL_USER_PROCESS_PARAMETERS *rebuild_startup_info(
    const struct startup_info_data *info, SIZE_T info_size, const void *env, SIZE_T env_size)
{
    RTL_USER_PROCESS_PARAMETERS *params = NULL;
    UNICODE_STRING nt_name;
    WCHAR *src, *dst;
    SIZE_T size, env_pos = env_size / sizeof(WCHAR);
    NTSTATUS status;
''' + init[begin:end] + '''    free( nt_name.Buffer );
    return params;
}
'''
Path('prototype/out/wine-startup-info.inc').write_text('\n\n'.join([
    function('copy_unicode_string'), function('copy_dos_path_string'),
    function('get_params_string'), function('append_string'), create[0], reconstruct]) + '\n')

# The initial x86 registers are Wine's own new-WoW64 startup registers.
signal = Path('wine-11.0-source/dlls/ntdll/unix/signal_x86_64.c').read_text()
begin = signal.index('        wow_context->ContextFlags = CONTEXT_I386_ALL;',
                     signal.index('void init_syscall_frame('))
end = signal.index('\n    }', begin)
registers = signal[begin:end].replace('get_wow_teb( teb )', 'teb').replace(
    'pLdrSystemDllInitBlock->pRtlUserThreadStart', 'start').replace('thread_data->fs', 'fs')
Path('prototype/out/wine-child-context.inc').write_text('''
void probe_init_child_context(I386_CONTEXT *wow_context, void *entry, void *arg,
                              TEB32 *teb, ULONG start, WORD cs32_sel, WORD ds64_sel, WORD fs)
{
    void *peb = arg, *wow_peb = arg;
    CONTEXT context = {0};
    context.FltSave.ControlWord = 0x27f;
    context.FltSave.MxCsr = 0x1f80;
''' + registers + '\n}\n')
