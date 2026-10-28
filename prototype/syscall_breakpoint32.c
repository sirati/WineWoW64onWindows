/* Wine ntdll/tests/exception.c test_hwbpt_in_syscall, followed by thread
 * creation to check that a trapped TLS clear did not abandon the registry. */
#include "test_support.h"
static volatile LONG delivered;
static LONG CALLBACK on_trap(EXCEPTION_POINTERS *exception)
{
    if(exception->ExceptionRecord->ExceptionCode!=(DWORD)0x80000004)return EXCEPTION_CONTINUE_SEARCH;
    ++delivered;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static DWORD WINAPI worker(void *unused) { (void)unused;return 29; }
void WINAPI app_entry(void)
{
    typedef LONG (WINAPI *set_thread_t)(HANDLE,ULONG,void *,ULONG);
    set_thread_t set_thread=(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtSetInformationThread");
    DWORD index=TlsAlloc();BYTE *teb;
    __asm__ volatile("movl %%fs:0x18,%0":"=r"(teb));
    if(!set_thread||index>=64)ExitProcess(71);
    void *handler=AddVectoredExceptionHandler(1,on_trap);
    if(!handler)ExitProcess(72);
    void *volatile *slot=(void *volatile *)(teb+0xe10+index*sizeof(void *));
    CONTEXT context={.ContextFlags=CONTEXT_DEBUG_REGISTERS};
    context.Dr0=(DWORD)(ULONG_PTR)slot;context.Dr7=3|(3<<16)|(3<<18);
    if(!SetThreadContext(GetCurrentThread(),&context))ExitProcess(73);
    *slot=(void *)0xdeadbeef;
    LONG guest_traps=delivered;delivered=0;
    LONG status=set_thread(GetCurrentThread(),10,&index,sizeof(index)); /* ThreadZeroTlsCell */
    LONG syscall_traps=delivered;
    context.Dr7=0;
    if(!SetThreadContext(GetCurrentThread(),&context))ExitProcess(74);
    MESSAGE("guest.traps=");message_hex(guest_traps);
    MESSAGE("syscall.traps=");message_hex(syscall_traps);
    MESSAGE("syscall.status=");message_hex(status);
    if(guest_traps!=1||syscall_traps||status||*slot)ExitProcess(75);
    if(!RemoveVectoredExceptionHandler(handler)||!TlsFree(index))ExitProcess(76);
    MESSAGE("phase=before-thread-create\r\n");
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);DWORD code;
    if(!thread||WaitForSingleObject(thread,3000)!=WAIT_OBJECT_0||
       !GetExitCodeThread(thread,&code)||code!=29||!CloseHandle(thread))ExitProcess(77);
    MESSAGE("syscall_breakpoint32: guest watchpoint delivered, syscall watchpoint ignored and later thread creation passed\r\n");
    ExitProcess(0);
}
