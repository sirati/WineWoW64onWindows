#include "test_support.h"
void WINAPI app_entry(void)
{
    DWORD self,peb;
    __asm__ __volatile__("movl %%fs:0x18,%0; movl %%fs:0x30,%1":"=r"(self),"=r"(peb));
    if(!self||!peb) { MESSAGE("teb32: guest TEB or PEB is missing\r\n");ExitProcess(31); }
    if(*(DWORD *)(uintptr_t)(self+0x18)!=self) ExitProcess(33);
    /* Force the same descriptor reload that exposed FS loss under Windows
     * scheduling, so recovery is exercised on every host run. */
    __asm__ __volatile__("movw %%fs,%%ax;movw %%ax,%%fs":::"ax","memory");
    for(unsigned i=0;i<1000;i++)
    {
        DWORD again;Sleep(0);GetTickCount64();
        __asm__ __volatile__("movl %%fs:0x18,%0":"=r"(again));
        if(again!=self) ExitProcess(35);
    }
    MESSAGE("teb32: FS/TEB survived 1000 Windows call and scheduling cycles\r\n");ExitProcess(0);
}
