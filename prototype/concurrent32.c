#include "test_support.h"
static DWORD tls_index;
static DWORD WINAPI worker(void *param)
{
    DWORD self;__asm__ __volatile__("movl %%fs:0x18,%0":"=r"(self));
    if(TlsGetValue(tls_index)) return 10;
    if(!TlsSetValue(tls_index,param)) return 11;
    for(unsigned n=0;n<100;n++)
    {
        DWORD again;void *p=VirtualAlloc(0,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!p||!VirtualFree(p,0,MEM_RELEASE))return 12;
        Sleep(0);GetTickCount64();
        __asm__ __volatile__("movl %%fs:0x18,%0":"=r"(again));
        if(again!=self||TlsGetValue(tls_index)!=param) return 13;
        if(*(DWORD *)(uintptr_t)(self+0x24)!=GetCurrentThreadId()) return 14;
    }
    return (DWORD)(uintptr_t)param;
}
void WINAPI app_entry(void)
{
    HANDLE threads[8];DWORD code;
    tls_index=TlsAlloc();if(tls_index==TLS_OUT_OF_INDEXES) ExitProcess(75);
    TlsSetValue(tls_index,(void *)0x12345678);
    for(unsigned i=0;i<8;i++) if(!(threads[i]=CreateThread(0,0,worker,(void *)(uintptr_t)(100+i),0,0))) ExitProcess(76);
    for(unsigned i=0;i<8;i++)
    {
        if(WaitForSingleObject(threads[i],5000)!=WAIT_OBJECT_0||!GetExitCodeThread(threads[i],&code)||code!=100+i) ExitProcess(77);
        if(!CloseHandle(threads[i])) ExitProcess(78);
    }
    if(TlsGetValue(tls_index)!=(void *)0x12345678) ExitProcess(79);
    MESSAGE("concurrent32: 8 threads, distinct TEBs, TLS isolation, 800 allocation/scheduling cycles passed\r\n");ExitProcess(0);
}
