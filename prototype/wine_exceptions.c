/* Wine LGPL exception/context code, with native Windows storage adapters.
 * The guest dispatcher and C++ handlers remain in the original Wine DLLs. */
#include <stdio.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "cpu_host.h"
#include "native_threads.h"
#include "../wine-11.0-source/dlls/wow64/wow64_private.h"
#include "wine/debug.h"
#include "wine/asm.h"
extern struct guest_cpu *probe_current_cpu(void);
extern TEB32 *probe_current_teb32(void);
extern void probe_capture_guest_context(const CONTEXT *);
extern void probe_capture_cpu_context(struct guest_cpu *,const CONTEXT *);
extern void probe_resume32(void);
extern DWORD WINAPI probe_run32(I386_CONTEXT *);
extern void WINAPI RtlCaptureContext(CONTEXT *);
static void DECLSPEC_NORETURN cpu_simulate(void)
{
    probe_run32(&probe_current_cpu()->ctx);
    ExitProcess(95);
}
struct wine_thread_state { TEB teb; WOW64INFO info; struct { ULONG fs; } arch; };
static DWORD state_slot=TLS_OUT_OF_INDEXES;
static DWORD target_slot=TLS_OUT_OF_INDEXES;
static WORD ss32_sel,ds64_sel;
static struct guest_cpu *context_cpu(void)
{
    struct guest_cpu *cpu=target_slot!=TLS_OUT_OF_INDEXES?TlsGetValue(target_slot):NULL;
    return cpu?cpu:probe_current_cpu();
}
static TEB *wine_teb(void)
{
    struct wine_thread_state *state=TlsGetValue(state_slot);
    struct guest_cpu *cpu=context_cpu();
    if(!state)
    {
        state=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*state));
        if(!state)ExitProcess(94);
        state->teb.Peb=NtCurrentTeb()->Peb;
        state->teb.Tib.Self=&state->teb.Tib;
        state->teb.TlsSlots[WOW64_TLS_WOW64INFO]=&state->info;
        state->info.NativeSystemPageSize=4096;state->info.CpuFlags=WOW64_CPUFLAGS_MSFT64;
        state->info.NativeMachineType=IMAGE_FILE_MACHINE_AMD64;state->info.EmulatedMachineType=IMAGE_FILE_MACHINE_I386;
        TlsSetValue(state_slot,state);
        probe_thread_exception_state(state);
    }
    state->teb.TlsSlots[WOW64_TLS_CPURESERVED]=cpu;
    state->arch.fs=cpu?cpu->ctx.SegFs:0;
    return &state->teb;
}
static struct syscall_frame *get_syscall_frame(void) { return &context_cpu()->frame; }
static BOOL is_wow64(void) { return probe_current_cpu()!=NULL; }
static typeof(((struct wine_thread_state *)0)->arch) *amd64_thread_data(void)
{ return &CONTAINING_RECORD(wine_teb(),struct wine_thread_state,teb)->arch; }
#undef NtCurrentTeb
#define NtCurrentTeb() wine_teb()
#include "out/wine-self-context.inc"
static NTSTATUS thread_context(HANDLE handle,I386_CONTEXT *context,BOOL set)
{
    extern int probe_trace;
    struct guest_thread *thread=probe_thread_acquire(handle);
    if(!thread){if(probe_trace)fprintf(stderr,"THREAD_CONTEXT no-record handle=%p set=%d flags=%08lx\n",handle,set,context->ContextFlags);return STATUS_INVALID_HANDLE;}
    struct guest_cpu *cpu=probe_thread_cpu(thread);
    if(!cpu){if(probe_trace)fprintf(stderr,"THREAD_CONTEXT no-cpu handle=%p set=%d\n",handle,set);probe_thread_release(thread);return STATUS_THREAD_NOT_IN_PROCESS;}
    BOOL self=(handle==GetCurrentThread()||GetThreadId(handle)==GetCurrentThreadId());
    probe_cpu_prepare(cpu);
    BYTE native_storage[PROBE_XSAVE_CAPACITY+2048] __attribute__((aligned(64)))={0};
    CONTEXT *native=(CONTEXT *)native_storage;CONTEXT_EX *native_ex=NULL;
    DWORD native_flags=CONTEXT_ALL|(xstate_extended_features?CONTEXT_XSTATE:0);
    NTSTATUS status=RtlInitializeExtendedContext(native,native_flags,&native_ex);
    if(status){probe_thread_release(thread);return status;}
    native->ContextFlags=native_flags;
    if(xstate_extended_features)RtlSetExtendedFeaturesMask(native_ex,probe_xsave_mask);

    DWORD flags=context->ContextFlags&~CONTEXT_i386;
    if(!self||(flags&CONTEXT_I386_DEBUG_REGISTERS))status=NtGetContextThread(handle,native);
    if(status){if(probe_trace)fprintf(stderr,"THREAD_CONTEXT native status=%08lx\n",status);probe_thread_release(thread);return status;}
    if(!self&&native->SegCs==0x23)probe_capture_cpu_context(cpu,native);
    struct guest_cpu *previous=TlsGetValue(target_slot);TlsSetValue(target_slot,cpu);
    status=set?set_thread_wow64_context(GetCurrentThread(),context,sizeof(*context)):
               get_thread_wow64_context(GetCurrentThread(),context,sizeof(*context));
    TlsSetValue(target_slot,previous);
    if(!status&&set)
    {
        /* Wine's legacy context copy is also consumed by our full XRSTOR. */
        if(flags&0x20)cpu->frame.xstate.Mask|=3;
        else if(flags&0x8)cpu->frame.xstate.Mask|=1;
    }
    if(probe_trace&&(flags&0x40))
    {
        DWORD *high6=(DWORD *)((BYTE *)&cpu->frame.xstate+64+6*16);
        fprintf(stderr,"THREAD_XSTATE set=%d status=%08lx cpu=%p mask=%llx high6=%lx,%lx,%lx,%lx\n",
            set,status,cpu,cpu->frame.xstate.Mask,high6[0],high6[1],high6[2],high6[3]);
    }
    if(!status&&(flags&CONTEXT_I386_DEBUG_REGISTERS))
    {
        if(set)
        { native->Dr0=context->Dr0;native->Dr1=context->Dr1;native->Dr2=context->Dr2;
          native->Dr3=context->Dr3;native->Dr6=context->Dr6;native->Dr7=context->Dr7; }
        else
        { context->Dr0=native->Dr0;context->Dr1=native->Dr1;context->Dr2=native->Dr2;
          context->Dr3=native->Dr3;context->Dr6=native->Dr6;context->Dr7=native->Dr7; }
    }
    if(!status&&set)
    {
        cpu->flags|=WOW64_CPURESERVED_FLAG_RESET_STATE;
        if(!self)
        {
            /* A suspended thread can be in compatibility code or in its
             * native adapter. Keep the native continuation in the latter
             * case; Wine restores the edited guest context on return. */
            if(native->SegCs==0x23)
            {
                /* Native NtSetContextThread normalizes CS to x64 for this
                 * native process. Resume Wine's transition on the saved
                 * native stack; its IRET restores the requested x86 state. */
                WORD cs;__asm__("movw %%cs,%0":"=r"(cs));
                native->Rip=(ULONG_PTR)probe_resume32;native->Rsp=native->R14;
                native->SegCs=cs;native->SegSs=ss32_sel;
            }
            status=NtSetContextThread(handle,native);
        }
        else if(flags&CONTEXT_I386_DEBUG_REGISTERS)
        { native->ContextFlags=CONTEXT_DEBUG_REGISTERS;status=NtSetContextThread(handle,native); }
    }
    if(probe_trace)fprintf(stderr,"THREAD_CONTEXT done status=%08lx set=%d flags=%08lx self=%d nativecs=%x eip=%08lx esp=%08lx ebp=%08lx\n",status,set,flags,self,native->SegCs,context->Eip,context->Esp,context->Ebp);
    probe_thread_release(thread);return status;
}
NTSTATUS WINAPI probe_RtlWow64GetThreadContext(HANDLE thread,WOW64_CONTEXT *context)
{ return thread_context(thread,(I386_CONTEXT *)context,FALSE); }
NTSTATUS WINAPI probe_RtlWow64SetThreadContext(HANDLE thread,const WOW64_CONTEXT *context)
{ return thread_context(thread,(I386_CONTEXT *)context,TRUE); }
static NTSTATUS WINAPI get_context(HANDLE thread,HANDLE process,void *unused,void *context)
{ (void)process;(void)unused;return probe_RtlWow64GetThreadContext(thread,context); }
static NTSTATUS WINAPI set_context(HANDLE thread,HANDLE process,void *unused,void *context)
{ (void)process;(void)unused;return probe_RtlWow64SetThreadContext(thread,context); }
static NTSTATUS (WINAPI *pBTCpuGetContext)(HANDLE,HANDLE,void *,void *)=get_context;
static NTSTATUS (WINAPI *pBTCpuSetContext)(HANDLE,HANDLE,void *,void *)=set_context;
static void *(WINAPI *pKiRaiseUserExceptionDispatcher)(void);
#include "out/wine-protected-syscall.inc"
static SYSTEM_DLL_INIT_BLOCK init_block;
SYSTEM_DLL_INIT_BLOCK *pLdrSystemDllInitBlock=&init_block;
static WOW64INFO info={.CpuFlags=WOW64_CPUFLAGS_MSFT64};
static WOW64INFO *wow64info=&info;
#undef TRACE
#define TRACE(...) do {} while(0)
#define Wow64ApcRoutine wine_original_Wow64ApcRoutine
NTSTATUS WINAPI wine_RtlGetExtendedContextLength(ULONG,ULONG *);
NTSTATUS WINAPI wine_RtlInitializeExtendedContext(void *,ULONG,CONTEXT_EX **);
NTSTATUS WINAPI wine_RtlCopyExtendedContext(CONTEXT_EX *,ULONG,CONTEXT_EX *);
static NTSTATUS WINAPI trace_copy_extended_context(CONTEXT_EX *dst,ULONG flags,CONTEXT_EX *src)
{
    extern int probe_trace;
    NTSTATUS status=wine_RtlCopyExtendedContext(dst,flags,src);
    if(probe_trace)
    {
        XSAVE_AREA_HEADER *xs=(void *)((BYTE *)dst+dst->XState.Offset);
        fprintf(stderr,"GUEST_CONTEXT_COPY status=%08lx flags=%08lx legacy=%ld/%lu all=%ld/%lu xstate=%ld/%lu mask=%llx compact=%llx enabled=%llx\n",
            status,flags,dst->Legacy.Offset,dst->Legacy.Length,dst->All.Offset,dst->All.Length,
            dst->XState.Offset,dst->XState.Length,xs->Mask,xs->CompactionMask,user_shared_data->XState.EnabledFeatures);
    }
    return status;
}
#define RtlGetExtendedContextLength wine_RtlGetExtendedContextLength
#define RtlInitializeExtendedContext wine_RtlInitializeExtendedContext
#define RtlCopyExtendedContext trace_copy_extended_context
#include "out/wine-exception-routines.inc"
#undef RtlGetExtendedContextLength
#undef RtlInitializeExtendedContext
#undef RtlCopyExtendedContext
#undef Wow64ApcRoutine
#undef NtCurrentTeb
#define RtlWow64GetThreadContext probe_RtlWow64GetThreadContext
#define RtlWow64SetThreadContext probe_RtlWow64SetThreadContext
#include "out/wine-context-thunks.inc"
#undef RtlWow64GetThreadContext
#undef RtlWow64SetThreadContext
void WINAPI Wow64ApcRoutine(ULONG_PTR a,ULONG_PTR b,ULONG_PTR c,CONTEXT *unused)
{
    CONTEXT resume;
    volatile LONG resumed=0;
    (void)unused;
    /* The Windows APC dispatcher owns its native continuation. Capture a
     * local native continuation so Wine's original NtContinueEx path can
     * return after guest dispatch without depending on undocumented args. */
    RtlCaptureContext(&resume);
    if(!InterlockedExchange(&resumed,1))wine_original_Wow64ApcRoutine(a,b,c,&resume);
}
void probe_apc_init(void *dispatcher)
{ init_block.pKiUserApcDispatcher=(ULONG_PTR)dispatcher; }
void probe_exception_thread_cleanup(void)
{
    if(state_slot==TLS_OUT_OF_INDEXES)return;
    struct wine_thread_state *state=TlsGetValue(state_slot);
    TlsSetValue(state_slot,NULL);
    probe_thread_exception_state(NULL);
    HeapFree(GetProcessHeap(),0,state);
}
void probe_exception_init(void *dispatcher,void *raise_dispatcher,WORD ss)
{
    state_slot=TlsAlloc();if(state_slot==TLS_OUT_OF_INDEXES)ExitProcess(94);
    target_slot=TlsAlloc();if(target_slot==TLS_OUT_OF_INDEXES)ExitProcess(94);
    ss32_sel=ds64_sel=ss;init_block.pKiUserExceptionDispatcher=(ULONG_PTR)dispatcher;
    pKiRaiseUserExceptionDispatcher=raise_dispatcher;
}
BOOL probe_dispatch_guest_exception(EXCEPTION_POINTERS *p)
{
    extern int probe_trace;
    struct guest_cpu *cpu=probe_current_cpu();CONTEXT *context=p->ContextRecord;
    if(!cpu||context->SegCs!=0x23||context->R13!=(uintptr_t)&cpu->ctx)return FALSE;
    WORD native_cs;__asm__("movw %%cs,%0":"=r"(native_cs));
    probe_capture_guest_context(context);
    /* Windows places its exception CONTEXT_EX on the interrupted guest
     * stack. Wine builds the x86 dispatcher frame below the same guest ESP,
     * so initialization of that frame can overwrite the source XSAVE area.
     * Retain a native-stack copy before Wine allocates the guest frame. */
    BYTE saved_storage[PROBE_XSAVE_CAPACITY+2048] __attribute__((aligned(64)));
    CONTEXT *saved=(CONTEXT *)saved_storage;
    memcpy(saved,context,sizeof(*saved));
    if((context->ContextFlags&CONTEXT_XSTATE)==CONTEXT_XSTATE)
    {
        CONTEXT_EX *saved_ex;
        NTSTATUS status=wine_RtlInitializeExtendedContext(saved,context->ContextFlags,&saved_ex);
        if(!status)status=wine_RtlCopyExtendedContext(saved_ex,CONTEXT_XSTATE,(CONTEXT_EX *)(context+1));
        if(status)return FALSE;
    }
    if(probe_trace)
    {
        fprintf(stderr,"EXCEPTION_CONTEXT native_flags=%08lx guest_mask=%llx code=%08lx\n",context->ContextFlags,cpu->frame.xstate.Mask,p->ExceptionRecord->ExceptionCode);
        if(context->ContextFlags&0x40)
        { CONTEXT_EX *ex=(CONTEXT_EX *)(context+1);XSAVE_AREA_HEADER *xs=(void *)((BYTE *)ex+ex->XState.Offset);
          fprintf(stderr,"EXCEPTION_XSTATE length=%lu offset=%ld mask=%llx compact=%llx\n",ex->XState.Length,ex->XState.Offset,xs->Mask,xs->CompactionMask); }
    }
    /* Native Windows reports compatibility-mode debug traps with WX86
     * status codes. Wine's guest dispatcher expects the x86 exception codes
     * its Unix signal backend normally supplies. Keep the native record. */
    EXCEPTION_RECORD record=*p->ExceptionRecord;
    if(record.ExceptionCode==STATUS_WX86_SINGLE_STEP)record.ExceptionCode=STATUS_SINGLE_STEP;
    else if(record.ExceptionCode==STATUS_WX86_BREAKPOINT)record.ExceptionCode=STATUS_BREAKPOINT;
    EXCEPTION_POINTERS stable={&record,saved};
    Wow64PassExceptionToGuest(&stable);
    /* The Windows VEH context may point at a native CONTEXT_EX whose compact
     * area belongs to the interrupted x64 dispatcher. Wine has already copied
     * it into the guest frame, so do not ask Windows to consume that native
     * extension when the VEH resumes through probe_resume32. */
    context->ContextFlags&=~0x40u;
    context->Rip=(uintptr_t)probe_resume32;context->Rsp=context->R14;
    context->SegCs=native_cs;context->SegSs=ss32_sel;
    return TRUE;
}
