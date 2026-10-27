#include "test_support.h"
__declspec(dllimport) DWORD WINAPI LifecycleSet(DWORD);
__declspec(dllimport) DWORD WINAPI LifecycleGet(void);
__declspec(dllimport) DWORD WINAPI LifecycleCounts(DWORD);
static DWORD WINAPI worker(void *parameter)
{
    DWORD id=(DWORD)(uintptr_t)parameter;
    if(LifecycleSet(id))return 91;
    Sleep(10);
    if(LifecycleGet()!=id)return 92;
    if(id&1)ExitThread(id+100);
    return id+100;
}
void WINAPI app_entry(void)
{
    HANDLE threads[8];DWORD code;
    if(LifecycleSet(0xabcdef))ExitProcess(90);
    for(DWORD i=0;i<8;i++)
        if(!(threads[i]=CreateThread(NULL,0,worker,(void *)(uintptr_t)(i+1),0,NULL)))ExitProcess(93);
    for(DWORD i=0;i<8;i++)
    {
        if(WaitForSingleObject(threads[i],5000)!=WAIT_OBJECT_0||
           !GetExitCodeThread(threads[i],&code)||code!=i+101)ExitProcess(94);
        if(!CloseHandle(threads[i]))ExitProcess(95);
    }
    if(LifecycleGet()!=0xabcdef||LifecycleCounts(8))ExitProcess(96);
    MESSAGE("lifecycle32: independent static TLS, DLL/TLS notifications, FLS and ExitThread cleanup passed\r\n");
    ExitProcess(0);
}
