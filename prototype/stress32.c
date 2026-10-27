#include "test_support.h"
__declspec(dllimport) DWORD WINAPI ProbeGui(void);
void WINAPI app_entry(void)
{
    WORD cs; __asm__("movw %%cs,%0":"=r"(cs));
    if(cs!=0x23) ExitProcess(40);
    for(unsigned i=0;i<1000;i++)
    {
        volatile DWORD *p=VirtualAlloc(0,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!p) ExitProcess(41);
        p[0]=i^0x12345678;p[1023]=~i;
        if(p[0]!=(i^0x12345678)||p[1023]!=~i) ExitProcess(42);
        if(!VirtualFree((void *)p,0,MEM_RELEASE)) ExitProcess(43);
        if(!GetCurrentProcessId()||!GetTickCount64()) ExitProcess(44);
    }
    if(!ProbeGui()) ExitProcess(45);
    MESSAGE("stress32: 1000 allocation cycles, 2000 x64 DLL calls, and native x64 USER32 passed\r\n");
    ExitProcess(0);
}
