/* A converted fiber retains its stack after its original thread exits. */
#include "test_support.h"
static void *main_fiber;
static void *volatile preserved;
static void *volatile preserved_allocation;
static volatile LONG resumed;
static DWORD main_tid;
static void WINAPI exit_on_other_stack(void *unused)
{
    (void)unused;
    if(!ConvertFiberToThread())ExitProcess(81);
    ExitThread(16);
}
static DWORD WINAPI worker(void *unused)
{
    MEMORY_BASIC_INFORMATION memory;
    DWORD local=0;
    (void)unused;
    preserved=ConvertThreadToFiber(NULL);
    if(!preserved||!VirtualQuery(&local,&memory,sizeof(memory)))ExitProcess(82);
    preserved_allocation=memory.AllocationBase;
    void *other=CreateFiber(0,exit_on_other_stack,NULL);
    if(!other)ExitProcess(83);
    SwitchToFiber(other);
    if(GetCurrentThreadId()!=main_tid)ExitProcess(84);
    InterlockedIncrement(&resumed);
    SwitchToFiber(main_fiber);
    ExitProcess(85);
    return 0;
}
void WINAPI app_entry(void)
{
    main_tid=GetCurrentThreadId();
    main_fiber=ConvertThreadToFiber(NULL);
    if(!main_fiber)ExitProcess(86);
    for(unsigned i=0;i<8;i++)
    {
        HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);
        DWORD code;
        if(!thread||WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0||
           !GetExitCodeThread(thread,&code)||code!=16||!CloseHandle(thread))ExitProcess(87);
        SwitchToFiber(preserved);
        if(resumed!=(LONG)i+1)ExitProcess(88);
        DeleteFiber(preserved);
        MEMORY_BASIC_INFORMATION memory;
        if(!VirtualQuery(preserved_allocation,&memory,sizeof(memory))||memory.State!=MEM_FREE)ExitProcess(89);
    }
    if(!ConvertFiberToThread())ExitProcess(90);
    MESSAGE("fiber_thread_exit32: retained fibers resume on another thread and release their stacks passed\r\n");
    ExitProcess(0);
}
