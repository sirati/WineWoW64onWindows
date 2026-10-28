#include "test_support.h"
static volatile LONG calls;
static DWORD owner;
static HANDLE ready,done;
static void require(BOOL value,DWORD stage)
{ if(!value){MESSAGE("apc32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage);} }
static void CALLBACK count_apc(ULONG_PTR value)
{
    require(GetCurrentThreadId()==owner,214);
    InterlockedExchangeAdd(&calls,value);
    if(value==10)
    {
        require(QueueUserAPC(count_apc,GetCurrentThread(),3),215);
        require(SleepEx(0,TRUE)==WAIT_IO_COMPLETION,216);
    }
}
static DWORD WINAPI worker(void *unused)
{
    (void)unused;owner=GetCurrentThreadId();SetEvent(ready);
    require(WaitForSingleObjectEx(done,3000,TRUE)==WAIT_IO_COMPLETION,217);
    return 0;
}
void WINAPI app_entry(void)
{
    owner=GetCurrentThreadId();
    require(QueueUserAPC(count_apc,GetCurrentThread(),7),218);
    require(SleepEx(0,FALSE)==0&&calls==0,219);
    require(SleepEx(0,TRUE)==WAIT_IO_COMPLETION&&calls==7,220);
    require(QueueUserAPC(count_apc,GetCurrentThread(),10),221);
    require(SleepEx(0,TRUE)==WAIT_IO_COMPLETION&&calls==20,222);
    for(unsigned i=0;i<64;i++)require(QueueUserAPC(count_apc,GetCurrentThread(),1),223);
    while(calls!=84)require(SleepEx(0,TRUE)==WAIT_IO_COMPLETION,224);
    ready=CreateEventA(NULL,TRUE,FALSE,NULL);done=CreateEventA(NULL,TRUE,FALSE,NULL);require(ready&&done,225);
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);require(thread!=NULL,226);
    require(WaitForSingleObject(ready,3000)==WAIT_OBJECT_0,227);
    require(QueueUserAPC(count_apc,thread,5),228);
    require(WaitForSingleObject(thread,3000)==WAIT_OBJECT_0&&calls==89,229);
    require(CloseHandle(thread)&&CloseHandle(ready)&&CloseHandle(done),230);
    owner=GetCurrentThreadId();
    typedef DWORD (WINAPI *queue2_t)(PAPCFUNC,HANDLE,ULONG_PTR,DWORD);
    queue2_t queue2=(queue2_t)GetProcAddress(GetModuleHandleA("kernel32.dll"),"QueueUserAPC2");
    require(queue2!=NULL,231);
    require(queue2(count_apc,GetCurrentThread(),2,0)&&calls==89,232);
    require(SleepEx(0,TRUE)==WAIT_IO_COMPLETION&&calls==91,233);
    SetLastError(0xdeadbeef);
    require(!queue2(count_apc,GetCurrentThread(),2,1)&&GetLastError()==ERROR_NOT_SUPPORTED&&calls==91,234);
    MESSAGE("apc32: Wine APC dispatcher, issuing thread, nonalertable/alertable waits, nested APCs, guest worker delivery and QueueUserAPC2 passed\r\n");
    ExitProcess(0);
}
