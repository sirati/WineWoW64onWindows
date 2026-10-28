#include "test_support.h"
static CONDITION_VARIABLE condition=CONDITION_VARIABLE_INIT;
static CRITICAL_SECTION section;
static HANDLE ready;
static unsigned waiting;
static BOOL released;
static BOOL single_mode(void)
{
    const char *line=GetCommandLineA(),*option="--single";
    for(;*line;line++)
    {
        const char *a=line,*b=option;while(*a&&*b&&*a==*b){a++;b++;}
        if(!*b)return TRUE;
    }
    return FALSE;
}
static DWORD WINAPI waiter(void *unused)
{
    (void)unused;EnterCriticalSection(&section);
    if(++waiting==8)SetEvent(ready);
    while(!released)
        if(!SleepConditionVariableCS(&condition,&section,3000))
        {DWORD error=GetLastError();LeaveCriticalSection(&section);return error;}
    LeaveCriticalSection(&section);return 0;
}
void WINAPI app_entry(void)
{
    BOOL single=single_mode();unsigned completed=0;
    if(single){MESSAGE("mode=single\r\n");}else{MESSAGE("mode=all\r\n");}
    InitializeCriticalSection(&section);
    ready=CreateEventA(NULL,TRUE,FALSE,NULL);if(!ready)ExitProcess(70);
    for(unsigned round=0;round<8;round++)
    {
        HANDLE threads[8];waiting=0;released=FALSE;ResetEvent(ready);
        for(unsigned i=0;i<8;i++)
        {threads[i]=CreateThread(NULL,0,waiter,NULL,0,NULL);if(!threads[i])ExitProcess(71);}
        if(WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0)ExitProcess(72);
        EnterCriticalSection(&section);released=TRUE;
        if(single){for(unsigned i=0;i<8;i++)WakeConditionVariable(&condition);}
        else WakeAllConditionVariable(&condition);
        LeaveCriticalSection(&section);
        if(WaitForMultipleObjects(8,threads,TRUE,5000)!=WAIT_OBJECT_0)ExitProcess(73);
        for(unsigned i=0;i<8;i++)
        {
            DWORD code;if(!GetExitCodeThread(threads[i],&code)||code)ExitProcess(74);
            CloseHandle(threads[i]);completed++;
        }
    }
    CloseHandle(ready);DeleteCriticalSection(&section);
    MESSAGE("rounds=");message_hex(8);
    MESSAGE("completed.waiters=");message_hex(completed);
    MESSAGE("condition_wake32: eight waiters wake and thread lifetimes finish passed\r\n");
    ExitProcess(0);
}
