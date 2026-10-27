#include "test_support.h"
__declspec(dllimport) DWORD WINAPI LegacyWorkerCallbacks(void *,void *,DWORD);
struct state { DWORD issuer,worker,calls,index; };
static DWORD (WINAPI *lifecycle_set)(DWORD), (WINAPI *lifecycle_get)(void), (WINAPI *lifecycle_counts)(DWORD);
static DWORD WINAPI guest_callback(struct state *state,DWORD iteration)
{
    DWORD id=GetCurrentThreadId();
    if(id==state->issuer||state->worker&&state->worker!=id)ExitProcess(1);
    if(!iteration)
    { state->worker=id;if(lifecycle_set(0x12345))ExitProcess(2); }
    if((DWORD)(ULONG_PTR)TlsGetValue(state->index)!=iteration||lifecycle_get()!=0x12345)ExitProcess(3);
    if(!TlsSetValue(state->index,(void *)(ULONG_PTR)(iteration+1)))ExitProcess(4);
    state->calls++;return iteration+1;
}
void WINAPI app_entry(void)
{
    HMODULE library=LoadLibraryA("lifecycle_dll32.dll");if(!library)ExitProcess(5);
    lifecycle_set=(void *)GetProcAddress(library,"LifecycleSet@4");
    lifecycle_get=(void *)GetProcAddress(library,"LifecycleGet@0");
    lifecycle_counts=(void *)GetProcAddress(library,"LifecycleCounts@4");
    if(!lifecycle_set||!lifecycle_get||!lifecycle_counts)ExitProcess(6);
    struct state state={GetCurrentThreadId(),0,0,TlsAlloc()};if(state.index==TLS_OUT_OF_INDEXES)ExitProcess(7);
    if(LegacyWorkerCallbacks(guest_callback,&state,100)!=5050||state.calls!=100||!state.worker)ExitProcess(8);
    if(lifecycle_counts(1)||TlsGetValue(state.index)||!TlsFree(state.index)||!FreeLibrary(library))ExitProcess(9);
    MESSAGE("callback_worker32: native worker attaches Wine state once, preserves guest TLS across callbacks and runs Wine DLL/TLS/FLS detach on native exit passed\r\n");ExitProcess(0);
}
