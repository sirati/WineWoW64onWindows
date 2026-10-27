/* Wine-only control for its original debug buffer and output routines. */
#include "test_support.h"
typedef const char *(__cdecl *duplicate_t)(const char *);
typedef int (__cdecl *output_t)(const char *);
static duplicate_t duplicate;
static output_t output;
static DWORD check(void)
{
    const char *copy=duplicate("wine_debug32 string copy");
    if(!copy)return 1;
    const char *expected="wine_debug32 string copy";
    for(unsigned i=0;expected[i]||copy[i];i++)if(expected[i]!=copy[i])return 2;
    const char line[]="wine_debug32 original Wine output passed\n";
    if(output(line)!=sizeof(line)-1)return 3;
    return 0;
}
static DWORD WINAPI worker(void *unused) { (void)unused;return check(); }
void WINAPI app_entry(void)
{
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    duplicate=(duplicate_t)GetProcAddress(ntdll,"__wine_dbg_strdup");
    output=(output_t)GetProcAddress(ntdll,"__wine_dbg_output");
    if(!duplicate||!output)ExitProcess(77); /* Native Windows lacks Wine exports. */
    DWORD result=check();if(result)ExitProcess(result);
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);if(!thread)ExitProcess(4);
    if(WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0||
       !GetExitCodeThread(thread,&result)||result||!CloseHandle(thread))ExitProcess(5);
    MESSAGE("wine_debug32: original Wine main/worker debug buffers and output passed\r\n");
    ExitProcess(0);
}
