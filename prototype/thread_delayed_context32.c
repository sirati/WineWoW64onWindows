/* Check context errors before and after asynchronous guest record removal. */
#include "test_support.h"
#include <winternl.h>
typedef LONG (WINAPI *get_fn)(HANDLE,CONTEXT *);
typedef LONG (WINAPI *set_fn)(HANDLE,const CONTEXT *);
static get_fn ntget;
static set_fn ntset;
static HANDLE finish;
static unsigned failures;
static DWORD WINAPI worker(void *unused)
{ (void)unused;return WaitForSingleObject(finish,5000)==WAIT_OBJECT_0?0:91; }
static void clear_context(CONTEXT *context)
{
    volatile BYTE *bytes=(volatile BYTE *)context;
    for(unsigned i=0;i<sizeof(*context);i++)bytes[i]=0;
}
static void sample(HANDLE thread,BOOL exited)
{
    CONTEXT context;clear_context(&context);
#ifdef _WIN64
    context.ContextFlags=CONTEXT_AMD64;
#else
    context.ContextFlags=CONTEXT_i386;
#endif
    LONG get_status=ntget(thread,&context),set_status=ntset(thread,&context);
    MESSAGE("empty.get.status=");message_hex(get_status);
    MESSAGE("empty.set.status=");message_hex(set_status);
    context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
    SetLastError(0xdeadbeef);
    BOOL get=GetThreadContext(thread,&context);DWORD get_error=GetLastError();
    SetLastError(0xdeadbeef);
    BOOL set=SetThreadContext(thread,&context);DWORD set_error=GetLastError();
    MESSAGE("win32.get.success=");message_hex(get);
    MESSAGE("win32.get.error=");message_hex(get_error);
    MESSAGE("win32.set.success=");message_hex(set);
    MESSAGE("win32.set.error=");message_hex(set_error);
    if(exited)
    {
        if(get_status!=(LONG)0xc0000001||
           (set_status!=(LONG)0xc0000001&&set_status!=(LONG)0xc000004b)||get||set||
           (get_error!=ERROR_GEN_FAILURE&&get_error!=ERROR_ACCESS_DENIED)||
           (set_error!=ERROR_GEN_FAILURE&&set_error!=ERROR_ACCESS_DENIED))++failures;
    }
    else if(get_status||set_status||!get||!set)++failures;
}
void WINAPI app_entry(void)
{
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    ntget=(get_fn)GetProcAddress(ntdll,"NtGetContextThread");
    ntset=(set_fn)GetProcAddress(ntdll,"NtSetContextThread");
    if(!ntget||!ntset)ExitProcess(79);
    MESSAGE("pointer.bits=");message_hex(8*sizeof(void *));
    for(unsigned forced=0;forced<2;forced++)
    {
        MESSAGE("mode.forced=");message_hex(forced);
        finish=CreateEventA(NULL,FALSE,FALSE,NULL);
        HANDLE thread=CreateThread(NULL,0,worker,NULL,CREATE_SUSPENDED,NULL),duplicate;
        if(!finish||!thread)ExitProcess(71);
        MESSAGE("phase=live\r\n");sample(thread,FALSE);
        if(!DuplicateHandle(GetCurrentProcess(),thread,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))ExitProcess(72);
        if(forced){if(!TerminateThread(thread,77))ExitProcess(73);}
        else if(ResumeThread(thread)!=1||!SetEvent(finish))ExitProcess(74);
        if(WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0)ExitProcess(75);
        DWORD started=GetTickCount();
        MESSAGE("phase=immediate\r\n");sample(thread,TRUE);
        if(!CloseHandle(thread))ExitProcess(76);
        Sleep(250);
        MESSAGE("phase=delayed250\r\n");MESSAGE("elapsed.ms=");message_hex(GetTickCount()-started);sample(duplicate,TRUE);
        Sleep(1750);
        MESSAGE("phase=delayed2000\r\n");MESSAGE("elapsed.ms=");message_hex(GetTickCount()-started);sample(duplicate,TRUE);
        CloseHandle(duplicate);CloseHandle(finish);
        CONTEXT context;clear_context(&context);context.ContextFlags=CONTEXT_CONTROL;
        SetLastError(0xdeadbeef);
        BOOL get=GetThreadContext(NULL,&context);DWORD get_error=GetLastError();
        SetLastError(0xdeadbeef);
        BOOL set=SetThreadContext(NULL,&context);DWORD set_error=GetLastError();
        MESSAGE("invalid.get.error=");message_hex(get_error);
        MESSAGE("invalid.set.error=");message_hex(set_error);
        if(get||set||get_error!=ERROR_INVALID_HANDLE||set_error!=ERROR_INVALID_HANDLE)++failures;
    }
    MESSAGE("diagnostic.failures=");message_hex(failures);
    if(failures)ExitProcess(78);
    MESSAGE("thread_delayed_context32: normal and forced exit retain valid context-handle errors after cleanup passed\r\n");
    ExitProcess(0);
}
