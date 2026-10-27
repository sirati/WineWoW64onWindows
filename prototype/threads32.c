#include "test_support.h"
static DWORD WINAPI worker(void *p) { Sleep(100);return (DWORD)(uintptr_t)p+8; }
void WINAPI app_entry(void)
{
    HANDLE h=CreateThread(0,0,worker,(void *)9,0,0);DWORD code=0;
    if(!h) ExitProcess(50);
    if(WaitForSingleObject(h,0)!=WAIT_TIMEOUT) ExitProcess(54);
    if(WaitForSingleObject((HANDLE)0x12345678,0)!=WAIT_FAILED) ExitProcess(55);
    if(WaitForSingleObject(h,1000)!=WAIT_OBJECT_0||!GetExitCodeThread(h,&code)||code!=17) ExitProcess(51);
    if(!CloseHandle(h)) ExitProcess(56);
    MESSAGE("threads32: parameter, normal return, timeout and invalid handle passed\r\n");ExitProcess(0);
}
