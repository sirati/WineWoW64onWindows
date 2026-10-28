/* Wine's relative-priority interface must survive the native NT boundary. */
#include "test_support.h"
static DWORD WINAPI worker(void *unused)
{ (void)unused;return GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_IDLE?0:81; }
void WINAPI app_entry(void)
{
    HANDLE self=GetCurrentThread();
    int original=GetThreadPriority(self);
    const int priorities[]={0,-2,-1,1,2,THREAD_PRIORITY_TIME_CRITICAL,THREAD_PRIORITY_IDLE,0};
    if(original==THREAD_PRIORITY_ERROR_RETURN)ExitProcess(71);
    for(unsigned i=0;i<sizeof(priorities)/sizeof(priorities[0]);i++)
    {
        if(!SetThreadPriority(self,priorities[i])||GetThreadPriority(self)!=priorities[i])
        {
            message_hex(priorities[i]);message_hex(GetLastError());
            SetThreadPriority(self,original);ExitProcess(72);
        }
    }
    if(!SetThreadPriority(self,original))ExitProcess(73);
    HANDLE thread=CreateThread(NULL,0,worker,NULL,CREATE_SUSPENDED,NULL);
    if(!thread||GetThreadPriority(thread)!=THREAD_PRIORITY_NORMAL||
       !SetThreadPriority(thread,THREAD_PRIORITY_TIME_CRITICAL)||GetThreadPriority(thread)!=THREAD_PRIORITY_TIME_CRITICAL||
       !SetThreadPriority(thread,THREAD_PRIORITY_IDLE)||GetThreadPriority(thread)!=THREAD_PRIORITY_IDLE)ExitProcess(74);
    DWORD code;
    if(ResumeThread(thread)!=1||WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0||
       !GetExitCodeThread(thread,&code)||code||!CloseHandle(thread))ExitProcess(75);
    MESSAGE("thread_priority32: relative and extreme priorities round-trip on current and worker threads passed\r\n");
    ExitProcess(0);
}
