#include "test_support.h"
static HANDLE started,release_work,callback_done,finished;
static volatile LONG phase,calls;
static BOOL has_option(const char *wanted)
{
    const char *line=GetCommandLineA();
    for(;*line;line++){const char *a=line,*b=wanted;while(*a&&*b&&*a==*b){a++;b++;}if(!*b)return TRUE;}
    return FALSE;
}
static VOID CALLBACK work_callback(PTP_CALLBACK_INSTANCE instance,void *opaque,PTP_WORK work)
{
    (void)instance;(void)opaque;(void)work;
    InterlockedIncrement(&calls);SetEvent(started);
    if(WaitForSingleObject(release_work,3000)!=WAIT_OBJECT_0)ExitProcess(71);
    SetEvent(callback_done);
}
static DWORD WINAPI releaser(void *unused)
{ (void)unused;Sleep(100);return SetEvent(release_work)?0:72; }
static DWORD WINAPI watchdog(void *unused)
{
    (void)unused;
    if(WaitForSingleObject(finished,5000)==WAIT_OBJECT_0)return 0;
    MESSAGE("watchdog.phase=");message_hex(phase);
    MESSAGE("watchdog.callback-count=");message_hex(calls);
    MESSAGE("watchdog.callback-done=");message_hex(WaitForSingleObject(callback_done,0));
    ExitProcess(81);
}
void WINAPI app_entry(void)
{
    BOOL already_finished=has_option("--finished");
    if(already_finished){MESSAGE("mode=finished\r\n");}else{MESSAGE("mode=pending\r\n");}
    started=CreateEventA(NULL,TRUE,FALSE,NULL);release_work=CreateEventA(NULL,TRUE,FALSE,NULL);
    callback_done=CreateEventA(NULL,TRUE,FALSE,NULL);finished=CreateEventA(NULL,TRUE,FALSE,NULL);
    if(!started||!release_work||!callback_done||!finished)ExitProcess(73);
    HANDLE guard=CreateThread(NULL,0,watchdog,NULL,0,NULL);if(!guard)ExitProcess(74);
    phase=1;PTP_WORK work=CreateThreadpoolWork(work_callback,NULL,NULL);
    if(!work)ExitProcess(75);
    SubmitThreadpoolWork(work);
    if(WaitForSingleObject(started,3000)!=WAIT_OBJECT_0)ExitProcess(76);
    HANDLE release_thread=NULL;
    if(already_finished)
    {
        if(!SetEvent(release_work)||WaitForSingleObject(callback_done,3000)!=WAIT_OBJECT_0)ExitProcess(77);
        Sleep(100);
    }
    else
    {release_thread=CreateThread(NULL,0,releaser,NULL,0,NULL);if(!release_thread)ExitProcess(78);}
    phase=2;MESSAGE("phase=before-work-wait\r\n");
    WaitForThreadpoolWorkCallbacks(work,FALSE);
    phase=3;MESSAGE("phase=after-work-wait\r\n");
    CloseThreadpoolWork(work);
    PTP_POOL pool=CreateThreadpool(NULL);if(!pool||calls!=1)ExitProcess(79);
    CloseThreadpool(pool);
    phase=4;SetEvent(finished);
    if(WaitForSingleObject(guard,3000)!=WAIT_OBJECT_0)ExitProcess(80);
    if(release_thread){if(WaitForSingleObject(release_thread,3000)!=WAIT_OBJECT_0)ExitProcess(82);CloseHandle(release_thread);}
    CloseHandle(guard);CloseHandle(started);CloseHandle(release_work);CloseHandle(callback_done);CloseHandle(finished);
    MESSAGE("threadpool_wait32: pending/completed callback wait, work close and pool lifetime passed\r\n");
    ExitProcess(0);
}
