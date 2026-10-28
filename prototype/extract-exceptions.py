#!/usr/bin/env python3
"""Keep Wine's exception algorithms verbatim; select its current-thread context path."""
from pathlib import Path
import re
root=Path('wine-11.0-source/dlls')
sys=(root/'wow64/syscall.c').read_text()
# Function starts can have multiline signatures. Bound each search to the
# preceding line that begins with its return type, rather than a comment.
def fn(source,name):
    pos=source.index(name+'(') if name+'(' in source else source.index(name+' (')
    start=source.rfind('\n',0,pos)+1
    while source[start].isspace(): start=source.rfind('\n',0,start-1)+1
    end=source.index('\n}',pos)+2
    return source[start:end]
loader=(root/'ntdll/loader.c').read_text()
relocator=re.search(r'^IMAGE_BASE_RELOCATION \* WINAPI LdrProcessRelocationBlock\(.*?^\}',loader,re.M|re.S).group()
Path('prototype/out/wine-bootstrap-relocations.inc').write_text(relocator+'\n\n'+fn(loader,'perform_relocations')+'\n')
Path('prototype/out/wine-main-relocations.inc').write_text(fn(loader,'perform_relocations')+'\n')
parts=[fn(sys,n) for n in ['exception_record_32to64','exception_record_64to32','get_context_return_value','call_user_exception_dispatcher','wow64_NtContinueEx','wow64_NtContinue','wow64_NtRaiseException','Wow64PassExceptionToGuest','Wow64ApcRoutine','wow64_NtCallbackReturn']]
start=sys.index('/* based on RtlRaiseException:')
end=sys.index('\n#endif',sys.index('\n#else',start))+len('\n#endif')
parts.insert(4,sys[start:end])
parts.insert(0,re.search(r'^struct user_apc_frame\n\{.*?^\};',sys,re.M|re.S).group())
parts.insert(0,re.search(r'^struct user_callback_frame\n\{.*?^\};',sys,re.M|re.S).group())
Path('prototype/out/wine-exception-routines.inc').write_text('\n\n'.join(parts)+'\n')
Path('prototype/out/wine-context-thunks.inc').write_text('\n\n'.join(fn(sys,n) for n in ['wow64_NtGetContextThread','wow64_NtSetContextThread'])+'\n')
start=sys.index('#ifdef __aarch64__',sys.index(' *           wow64_syscall\n'))
end=sys.index('\n#endif',start)+len('\n#endif')
Path('prototype/out/wine-protected-syscall.inc').write_text(fn(sys,'call_raise_user_exception_dispatcher')+'\n\n'+sys[start:end]+'\n')
signal=(root/'ntdll/unix/signal_x86_64.c').read_text()
privileged=fn(signal,'is_privileged_instr')
# Native exception records do not expose the CPU error code used by Wine to
# exclude IDT faults. Preserve their access-violation classification here.
assert privileged.count('    case 0xcd: /* int $xx */')==1, 'Wine interrupt decoder changed'
privileged=privileged.replace('    case 0xcd: /* int $xx */',
                              '    case 0xcd: /* native IDT fault */ return 0;\n')
Path('prototype/out/wine-privileged-instruction.inc').write_text(privileged+'\n')
system=(root/'ntdll/unix/system.c').read_text()
Path('prototype/out/wine-xstate.inc').write_text('\n\n'.join(fn(system,n) for n in ['next_xstate_offset','xstate_get_size','copy_xstate'])+'\n')
virtual=(root/'ntdll/unix/virtual.c').read_text()
Path('prototype/out/wine-stack-growth.inc').write_text(fn(virtual,'grow_thread_stack')+'\n')
exception=(root/'ntdll/exception.c').read_text()
Path('prototype/out/wine-extended-context.inc').write_text(exception[exception.index('struct context_copy_range\n'):])
private=(root/'ntdll/unix/unix_private.h').read_text()
result=[fn(private,'set_context_exception_reporting_flags')]
for name,flags in [('set_thread_wow64_context','flags'),('get_thread_wow64_context','needed_flags')]:
    source=fn(signal,name)
    start=source.index('    if (!(wow_frame = get_cpu_area')
    end=source.index('    if ('+('flags & CONTEXT_I386_XSTATE' if flags=='flags' else '(needed_flags & CONTEXT_I386_XSTATE)'))
    core=source[start:source.index('\n    return STATUS_SUCCESS;',end)]
    core+='\n'
    # Internal XSAVE storage is standard layout. Guest/native CONTEXT_EX may
    # be compacted; Wine copy_xstate handles differing source/destination layouts.
    core=core.replace('        if (user_shared_data->XState.CompactionEnabled)\n            frame->xstate.CompactionMask |= xstate_extended_features;\n','')
    core=core.replace('            frame->xstate.CompactionMask |= xstate_extended_features;\n','')
    declarations='    const I386_CONTEXT *context=ctx;\n    DWORD flags=context->ContextFlags & ~CONTEXT_i386;\n' if flags=='flags' else '    I386_CONTEXT *context=ctx;\n    DWORD needed_flags=context->ContextFlags & ~CONTEXT_i386;\n'
    validation='''    if ((FLAGS & CONTEXT_I386_XSTATE) && xstate_extended_features)
    {
        CONTEXT_EX *context_ex = (CONTEXT_EX *)(context + 1);
        XSAVE_AREA_HEADER *xs = (XSAVE_AREA_HEADER *)((char *)context_ex + context_ex->XState.Offset);
        if (context_ex->XState.Length < sizeof(XSAVE_AREA_HEADER) || context_ex->XState.Length > xstate_size)
            return STATUS_INVALID_PARAMETER;
        if (xs->Mask & ~user_shared_data->XState.EnabledFeatures) return STATUS_INVALID_PARAMETER;
        if ((xs->Mask & xstate_extended_features) && context_ex->XState.Length < xstate_get_size(xs->CompactionMask,xs->Mask))
            return STATUS_BUFFER_OVERFLOW;
    }
    else FLAGS &= ~CONTEXT_I386_XSTATE;
'''.replace('FLAGS',flags)
    result.append('/* Original Wine context copy core, selected target storage. */\n'+source[:source.index('\n{')]+'\n{\n    struct syscall_frame *frame=get_syscall_frame();\n    I386_CONTEXT *wow_frame;\n'+declarations+'    if(size!=sizeof(I386_CONTEXT))return STATUS_INFO_LENGTH_MISMATCH;\n    if(handle!=GetCurrentThread())return STATUS_NOT_SUPPORTED;\n'+validation+core+'    return STATUS_SUCCESS;\n}')
thread=(root/'ntdll/unix/thread.c').read_text()
Path('prototype/out/wine-remote-xstate.inc').write_text('\n\n'.join(
    fn(thread,n) for n in ['xstate_to_server','xstate_from_server'])+'\n')
result.insert(0,fn(thread,'fpu_to_fpux'))
result.insert(0,fn(thread,'fpux_to_fpu'))
result.insert(0,fn(thread,'get_cpu_area'))
Path('prototype/out/wine-self-context.inc').write_text('\n\n'.join(result)+'\n')
