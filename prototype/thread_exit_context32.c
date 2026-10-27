#include "test_support.h"
static void fail_at(DWORD stage)
{ MESSAGE("thread_exit_context32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
struct job { HANDLE target;volatile LONG spinning,querying,exited; };
static volatile LONG successful_queries,ended_queries;
static DWORD WINAPI target(void *argument)
{
    struct job *job=argument;job->spinning=1;
    for(;;)__asm__ volatile("pause":::"memory");
}
static DWORD WINAPI query(void *argument)
{
    struct job *job=argument;CONTEXT context={0};context.ContextFlags=CONTEXT_FULL;
    REQUIRE(GetThreadContext(job->target,&context)&&context.SegCs==0x23&&context.Eip&&context.Esp,10);
    InterlockedIncrement(&successful_queries);job->querying=1;
    /* The target stays suspended until it is terminated. A valid handle
     * remains open throughout, including after its native thread signals. */
    while(!job->exited)
    {
        context.ContextFlags=CONTEXT_FULL;
        if(GetThreadContext(job->target,&context))InterlockedIncrement(&successful_queries);
        else InterlockedIncrement(&ended_queries);
    }
    for(unsigned i=0;i<64;i++)
    {
        context.ContextFlags=CONTEXT_FULL;
        if(GetThreadContext(job->target,&context))InterlockedIncrement(&successful_queries);
        else InterlockedIncrement(&ended_queries);
    }
    return 0;
}
void WINAPI app_entry(void)
{
    for(unsigned iteration=0;iteration<64;iteration++)
    {
        struct job job={0};job.target=CreateThread(NULL,0,target,&job,0,NULL);REQUIRE(job.target,20);
        for(unsigned i=0;i<3000&&!job.spinning;i++)Sleep(1);REQUIRE(job.spinning,21);
        REQUIRE(SuspendThread(job.target)==0,22);
        HANDLE inspector=CreateThread(NULL,0,query,&job,0,NULL);REQUIRE(inspector,23);
        for(unsigned i=0;i<3000&&!job.querying;i++)Sleep(1);REQUIRE(job.querying,24);
        REQUIRE(TerminateThread(job.target,77)&&WaitForSingleObject(job.target,3000)==WAIT_OBJECT_0,25);
        job.exited=1;
        REQUIRE(WaitForSingleObject(inspector,3000)==WAIT_OBJECT_0,26);
        DWORD code;REQUIRE(GetExitCodeThread(inspector,&code)&&!code,27);
        REQUIRE(CloseHandle(inspector)&&CloseHandle(job.target),28);
    }
    REQUIRE(successful_queries>=64,29);
    MESSAGE("thread_exit_context32 query success/ended counts:\r\n");
    message_hex(successful_queries);message_hex(ended_queries);
    MESSAGE("thread_exit_context32: concurrent context queries and suspended-thread termination across 64 thread lifetimes passed\r\n");
    ExitProcess(0);
}
