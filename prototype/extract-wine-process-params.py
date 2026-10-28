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
