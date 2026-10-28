/*
 * The complete original Wine 11.0 wow64cpu/cpu.c is compiled below.
 * It remains LGPL-2.1-or-later under its original copyright notice.
 * Native-Windows changes: private guest contexts, per-context FS hooks,
 * a finite return to the x64 caller, and recovery after Windows clears FS.
 */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "cpu_host.h"
#include "native_threads.h"
#define PROBE_FS_HOST "movq 0x2d4(%r13),%r11\n\twrfsbase %r11\n\t"
#define PROBE_FS_GUEST "movq 0x2cc(%r13),%r11\n\twrfsbase %r11\n\t"
#define PROBE_SAVE_STATE \
    "pushq %rax\n\tpushq %rdx\n\tmovl probe_xsave_mask(%rip),%eax\n\t" \
    "movl probe_xsave_mask+4(%rip),%edx\n\ttestl %eax,%eax\n\tjz 8f\n\t" \
    "xsave64 (%r11)\n\tjmp 9f\n\t8:\n\tfxsave64 (%r11)\n\t9:\n\tpopq %rdx\n\tpopq %rax\n\t"
#define PROBE_RESTORE_STATE \
    "pushq %rax\n\tpushq %rdx\n\tmovl probe_xsave_mask(%rip),%eax\n\t" \
    "movl probe_xsave_mask+4(%rip),%edx\n\ttestl %eax,%eax\n\tjz 8f\n\t" \
    "xrstor64 (%r11)\n\tjmp 9f\n\t8:\n\tfxrstor64 (%r11)\n\t9:\n\tpopq %rdx\n\tpopq %rax\n\t"
#define PROBE_FP_SAVE "movq 0x2e4(%r13),%r11\n\t" PROBE_SAVE_STATE
#define PROBE_FP_RESTORE "movq 0x2e4(%r13),%r11\n\t" PROBE_RESTORE_STATE
#include "../wine-11.0-source/dlls/wow64cpu/cpu.c"
extern void probe_resume32(void);
extern DWORD WINAPI probe_enter32(I386_CONTEXT *);
extern void probe_check_process_exit(I386_CONTEXT *,DWORD);
static void *native_exception_dispatcher __attribute__((used));
extern void probe_exception_return(void);
/* Wine's signal_x86_64.c init_handler clears live AC before entering C.
 * On Windows, KiUserExceptionDispatcher can itself fault with AC set, before
 * any VEH runs. Reserve the host's process instrumentation callback to apply
 * that sequence before the native dispatcher, leaving its saved CONTEXT alone.
 * All registers, the stack and the remaining flags are preserved; syscall,
 * APC and native thread returns go directly to their original destination. */
__ASM_GLOBAL_FUNC(probe_exception_return,
    "pushfq\n\t"
    "cmpq native_exception_dispatcher(%rip),%r10\n\tjne 1f\n\t"
    "andl $~0x40000,(%rsp)\n\t"
    "1:\n\tpopfq\n\tjmp *%r10")
NTSTATUS probe_exception_boundary_init(void)
{
    PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION info={0,0,probe_exception_return};
    native_exception_dispatcher=(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"KiUserExceptionDispatcher");
    if(!native_exception_dispatcher)return STATUS_PROCEDURE_NOT_FOUND;
    return NtSetInformationProcess(GetCurrentProcess(),ProcessInstrumentationCallback,&info,sizeof(info));
}
static DWORD guest_teb_slot;
static DWORD guest_cpu_slot;
unsigned long probe_fs_repairs;
void probe_cpu_init(USHORT cs,USHORT ss,USHORT fs)
{ cs64_sel=cs;ds64_sel=ss;fs32_sel=fs;probe_xstate_init();guest_teb_slot=TlsAlloc();guest_cpu_slot=TlsAlloc();if(guest_teb_slot==TLS_OUT_OF_INDEXES||guest_cpu_slot==TLS_OUT_OF_INDEXES)ExitProcess(94);probe_threads_init(); }
TEB32 *probe_current_teb32(void)
{ return TlsGetValue(guest_teb_slot); }
struct guest_cpu *probe_current_cpu(void) { return TlsGetValue(guest_cpu_slot); }
void probe_capture_cpu_context(struct guest_cpu *cpu,const CONTEXT *context)
{
    copy_context_64to32(&cpu->ctx,CONTEXT_I386_ALL,(CONTEXT *)context);
    memcpy(&cpu->frame.xsave,&context->FltSave,sizeof(cpu->frame.xsave));
    /* Windows' FltSave supplies the legacy state separately from the XSAVE
     * header. The last guest syscall may have saved it in the init state;
     * XRSTOR must not discard the newly captured x87/SSE bytes as init. */
    cpu->frame.xstate.Mask|=3;
    CONTEXT_EX *extended;
    if(probe_context_xstate((CONTEXT *)context,&extended))
    {
        XSAVE_AREA_HEADER *xs=(void *)((BYTE *)extended+extended->XState.Offset);
        copy_xstate(&cpu->frame.xstate,xs,xstate_extended_features);
    }
    cpu->frame.xstate.Mask|=3;
    cpu->frame.restore_flags|=CONTEXT_FLOATING_POINT;
}
void probe_capture_guest_context(const CONTEXT *context)
{ probe_capture_cpu_context(probe_current_cpu(),context); }
void probe_resume_guest_context(CONTEXT *context)
{
    if(context->SegCs!=cs32_sel)return;
    struct guest_cpu *cpu=probe_current_cpu();
    if(!cpu||context->R13!=(uintptr_t)&cpu->ctx)return;
    probe_capture_cpu_context(cpu,context);
    context->Rip=(uintptr_t)probe_resume32;context->Rsp=context->R14;
    context->SegCs=cs64_sel;context->SegSs=ds64_sel;
}
DWORD WINAPI probe_run32(I386_CONTEXT *ctx)
{
    struct guest_cpu *cpu=(void *)((BYTE *)ctx-4);
    BYTE native_state[PROBE_XSAVE_CAPACITY] __attribute__((aligned(64)))={0};
    probe_cpu_prepare(cpu);
    void *previous=TlsGetValue(guest_teb_slot);
    void *previous_cpu=TlsGetValue(guest_cpu_slot);
    struct guest_cpu *previous_registered;
    probe_thread_enter(cpu,&previous_registered);
    TlsSetValue(guest_teb_slot,(void *)cpu->guest_fs_base);
    TlsSetValue(guest_cpu_slot,cpu);
    extern DWORD WINAPI probe_enter32_state(I386_CONTEXT *,void *);
    DWORD result=probe_enter32_state(ctx,native_state);
    TlsSetValue(guest_teb_slot,previous);
    TlsSetValue(guest_cpu_slot,previous_cpu);
    probe_thread_leave(previous_registered);
    probe_check_process_exit(ctx,result);
    return result;
}
BOOL probe_recover_fs(EXCEPTION_POINTERS *p)
{
    CONTEXT *ctx=p->ContextRecord;
    if(p->ExceptionRecord->ExceptionCode!=STATUS_ACCESS_VIOLATION||ctx->SegCs!=cs32_sel||
       p->ExceptionRecord->ExceptionInformation[0]==8||
       p->ExceptionRecord->ExceptionInformation[1]>=0x1000||*(BYTE *)(uintptr_t)ctx->Rip!=0x64) return FALSE;
    struct guest_cpu *cpu=(void *)(uintptr_t)(ctx->R13-4);
    if(cpu->magic!=PROBE_CPU_MAGIC||!cpu->guest_fs_base) return FALSE;
    /* A Windows context switch may reload FS from its zero-base descriptor.
     * Reuse Wine's conversion, then resume through its full restore path after
     * NtContinue has returned to x64. The faulting instruction is retried. */
    probe_capture_cpu_context(cpu,ctx);
    ctx->Rip=(uintptr_t)probe_resume32;ctx->Rsp=ctx->R14;
    ctx->SegCs=cs64_sel;ctx->SegSs=ds64_sel;
    InterlockedIncrement((LONG *)&probe_fs_repairs);
    return TRUE;
}
__ASM_GLOBAL_FUNC(probe_enter32_state,
    "pushq %rbp\n\tpushq %rbx\n\tpushq %rsi\n\tpushq %rdi\n\t"
    "pushq %r12\n\tpushq %r13\n\tpushq %r14\n\tpushq %r15\n\t"
    "subq $0x238,%rsp\n\tmovq %rdx,0x230(%rsp)\n\tmovq %rdx,%r11\n\t"
    PROBE_SAVE_STATE
    "movq %rcx,%r13\n\t"
    "rdfsbase %r11\n\tmovq %r11,0x2d4(%r13)\n\t"
    "movabsq $0x57494e4543505531,%r11\n\tmovq %r11,0x2dc(%r13)\n\t"
    "jmp probe_resume32")
__ASM_GLOBAL_FUNC(probe_resume32,
    "movl 0x9c(%r13),%edi\n\tmovl 0xa0(%r13),%esi\n\t"
    "movl 0xa4(%r13),%ebx\n\tmovl 0xb4(%r13),%ebp\n\t"
    "jmp .Lsyscall_32to64_return")
/* Windows enters a native VEH on the interrupted guest stack. Call C on
 * the saved native stack, so its probes use the native TEB's stack limits. */
__ASM_GLOBAL_FUNC(probe_vectored_entry,
    /* Wine's signal_x86_64.c init_handler clears live AC before calling C.
     * The saved exception context retains the interrupted guest flags. */
    "pushfq\n\tandl $~0x40000,(%rsp)\n\tpopfq\n\t"
    "pushq %r12\n\tmovq %rsp,%r12\n\tmovq 8(%rcx),%rax\n\t"
    "cmpw $0x23,0x38(%rax)\n\tjne 1f\n\t"
    "movq 0xe8(%rax),%r11\n\ttestq %r11,%r11\n\tje 1f\n\t"
    "movq %r11,%rsp\n\tandq $-16,%rsp\n\t"
    "1:\n\tsubq $0x20,%rsp\n\tcall probe_on_exception\n\t"
    "movq %r12,%rsp\n\tpopq %r12\n\tret")
__ASM_GLOBAL_FUNC(probe_exit64,
    "xchgq %r14,%rsp\n\t"
    PROBE_FP_SAVE
    PROBE_FS_HOST
    "movq 0x230(%rsp),%r11\n\t"
    PROBE_RESTORE_STATE
    "addq $0x238,%rsp\n\t"
    "popq %r15\n\tpopq %r14\n\tpopq %r13\n\tpopq %r12\n\t"
    "popq %rdi\n\tpopq %rsi\n\tpopq %rbx\n\tpopq %rbp\n\tret")
