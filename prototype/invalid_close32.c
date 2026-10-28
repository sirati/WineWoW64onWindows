/* Wine ntdll/tests/exception.c invalid-handle control, followed by another
 * close to check that native debugger exceptions leave the I/O adapter usable. */
#include "test_support.h"
static volatile LONG delivered;
static LONG CALLBACK on_invalid_handle(EXCEPTION_POINTERS *exception)
{
    if(exception->ExceptionRecord->ExceptionCode!=(DWORD)0xc0000008)return EXCEPTION_CONTINUE_SEARCH;
    ++delivered;return EXCEPTION_CONTINUE_EXECUTION;
}
void WINAPI app_entry(void)
{
    typedef LONG (WINAPI *close_t)(HANDLE);
    close_t close=(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtClose");
    const char *line=GetCommandLineA();BOOL debugged=FALSE;
    for(;*line;line++)if(line[0]=='-'&&line[1]=='-'&&line[2]=='d')debugged=TRUE;
    if(!close)ExitProcess(71);
    void *handler=AddVectoredExceptionHandler(1,on_invalid_handle);
    if(!handler)ExitProcess(72);
    MESSAGE("INVALID_CLOSE_BEGIN\r\n");
    LONG status=close((HANDLE)0xdeadbeef);
    MESSAGE("invalid.close.status=");message_hex(status);
    MESSAGE("invalid.close.delivered=");message_hex(delivered);
    if(status!=(LONG)0xc0000008||delivered!=(debugged?1:0))ExitProcess(73);
    MESSAGE("phase=before-valid-close\r\n");
    for(unsigned i=0;i<8;i++)
    {
        HANDLE event=CreateEventA(NULL,TRUE,FALSE,NULL);
        if(!event||close(event))ExitProcess(74);
    }
    if(!RemoveVectoredExceptionHandler(handler))ExitProcess(75);
    MESSAGE("invalid_close32: invalid handle status and exception delivery preserve later handle closes passed\r\n");
    ExitProcess(0);
}
