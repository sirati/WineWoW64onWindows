/* Wine ntdll/tests/exception.c align_check_code instruction sequence. */
#include "test_support.h"
static LONG delivered;
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *exception)
{
    if(exception->ExceptionRecord->ExceptionCode!=(DWORD)0x80000002)return EXCEPTION_CONTINUE_SEARCH;
    ++delivered;
    exception->ContextRecord->EFlags&=~0x40000;
    return EXCEPTION_CONTINUE_EXECUTION;
}
void WINAPI app_entry(void)
{
    static const BYTE code[]={
        0x55,0x89,0xe5,0x9c,0x9c,0x58,0x0d,0,0,4,0,0x50,0x9d,
        0x89,0xe0,0x8b,0x40,1,0x9d,0x5d,0xc3
    };
    BYTE *memory=VirtualAlloc(NULL,sizeof(code),MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    void *handler=AddVectoredExceptionHandler(1,on_exception);
    if(!memory||!handler)ExitProcess(77);
    for(unsigned i=0;i<sizeof(code);i++)memory[i]=code[i];
    FlushInstructionCache(GetCurrentProcess(),memory,sizeof(code));
    MESSAGE("ALIGNMENT_BEGIN\r\n");
    ((void (__cdecl *)(void))memory)();
    MESSAGE("alignment.delivered=");message_hex(delivered);
    if(delivered){MESSAGE("ALIGNMENT_FAILED unexpected-handler\r\n");ExitProcess(78);}
    if(!GetCurrentProcessId()||!RemoveVectoredExceptionHandler(handler)||
       !VirtualFree(memory,0,MEM_RELEASE))ExitProcess(79);
    Sleep(0);
    MESSAGE("alignment32: alignment-check retry preserves guest execution without handler delivery passed\r\n");
    ExitProcess(0);
}
