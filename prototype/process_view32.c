/* Observe the emulated process through public Win32 and NT queries. */
#include "test_support.h"
typedef LONG (WINAPI *query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
typedef LONG (WINAPI *system_fn)(ULONG,void *,ULONG,ULONG *);
struct basic_process { LONG status;void *peb;ULONG affinity;LONG priority;ULONG pid,parent; };
struct basic_system {
    ULONG reserved,timer,page,pages,lowest_page,highest_page,granularity;
    void *lowest,*highest;ULONG affinity;BYTE processors,padding[3];
};
void WINAPI app_entry(void)
{
    query_fn query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");
    system_fn system=(system_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQuerySystemInformation");
    void *peb;BOOL wow;ULONG length;
    __asm__ __volatile__("movl %%fs:0x30,%0":"=r"(peb));
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,GetCurrentProcessId());
    if(!query||!system||!process)ExitProcess(71);
    HANDLE handles[]={GetCurrentProcess(),process};
    for(unsigned i=0;i<2;i++)
    {
        struct basic_process info={0};ULONG wow_peb=0;
        if(!IsWow64Process(handles[i],&wow)||!wow)ExitProcess(72);
        if(query(handles[i],0,&info,sizeof(info),&length)||length!=sizeof(info)||info.peb!=peb||
           info.pid!=GetCurrentProcessId())ExitProcess(73);
        if(query(handles[i],26,&wow_peb,sizeof(wow_peb),&length)||length!=sizeof(wow_peb)||wow_peb!=(ULONG)(uintptr_t)peb)
            ExitProcess(74);
    }
    struct basic_system basic={0};SYSTEM_INFO info;MEMORYSTATUSEX memory={.dwLength=sizeof(memory)};
    if(system(0,&basic,sizeof(basic),&length)||length!=sizeof(basic))ExitProcess(75);
    GetSystemInfo(&info);
    ULONG highest=(ULONG)(uintptr_t)basic.highest;
    if(info.lpMaximumApplicationAddress!=basic.highest||info.lpMinimumApplicationAddress!=basic.lowest||
       info.dwPageSize!=basic.page||info.dwAllocationGranularity!=basic.granularity)ExitProcess(76);
#ifdef TEST_LARGE_ADDRESS_AWARE
    if(highest!=0xfffeffff)ExitProcess(77);
#else
    if(highest!=0x7ffeffff)ExitProcess(77);
#endif
    if(!GlobalMemoryStatusEx(&memory)||memory.ullTotalVirtual!=(ULONGLONG)highest-(ULONG)(uintptr_t)basic.lowest+1)
        ExitProcess(78);
    void *allocation=VirtualAlloc(0,65536,MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN,PAGE_READWRITE);
    if(!allocation||(uintptr_t)allocation+65535>highest)ExitProcess(79);
    if(!VirtualFree(allocation,0,MEM_RELEASE)||!CloseHandle(process))ExitProcess(80);
#ifdef TEST_LARGE_ADDRESS_AWARE
    MESSAGE("process_view_laa32: guest PEB, WoW64 identity and 4 GiB address range passed\r\n");
#else
    MESSAGE("process_view32: guest PEB, WoW64 identity and 2 GiB address range passed\r\n");
#endif
    ExitProcess(0);
}
