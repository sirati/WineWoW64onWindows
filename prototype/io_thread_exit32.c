#include "test_support.h"
static void fail_at(DWORD stage)
{ MESSAGE("io_thread_exit32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static volatile LONG callbacks;
struct job
{
    HANDLE file,event,ready,hold;
    BOOL stack,apc;
    OVERLAPPED *issued;
    OVERLAPPED shared;
    char buffer[8];
};
static void CALLBACK completion(DWORD error,DWORD bytes,OVERLAPPED *operation)
{ (void)error;(void)bytes;(void)operation;InterlockedIncrement(&callbacks); }
static DWORD WINAPI issue(void *argument)
{
    struct job *job=argument;OVERLAPPED local={0};char buffer[8];DWORD count;
    OVERLAPPED *op=job->stack?&local:&job->shared;
    op->hEvent=job->event;job->issued=op;
    if(job->apc)REQUIRE(ReadFileEx(job->file,job->stack?buffer:job->buffer,8,op,completion),10);
    else REQUIRE(!ReadFile(job->file,job->stack?buffer:job->buffer,8,&count,op)&&GetLastError()==ERROR_IO_PENDING,11);
    REQUIRE(SetEvent(job->ready),12);
    /* Terminate only after submission has returned and the worker is waiting,
     * outside Wine's loader and native request-list critical sections. */
    WaitForSingleObject(job->hold,INFINITE);return 99;
}
static void one_case(BOOL stack,BOOL apc)
{
    struct job job={0};job.stack=stack;job.apc=apc;
    job.file=CreateNamedPipeA("\\\\.\\pipe\\wine-io-thread-exit32",PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE|PIPE_WAIT,1,4096,4096,0,NULL);
    REQUIRE(job.file!=INVALID_HANDLE_VALUE,20);
    HANDLE client=CreateFileA("\\\\.\\pipe\\wine-io-thread-exit32",GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    REQUIRE(client!=INVALID_HANDLE_VALUE,21);
    OVERLAPPED connect={0};
    REQUIRE(!ConnectNamedPipe(job.file,&connect)&&GetLastError()==ERROR_PIPE_CONNECTED,22);
    job.event=CreateEventA(NULL,TRUE,FALSE,NULL);job.ready=CreateEventA(NULL,TRUE,FALSE,NULL);
    job.hold=CreateEventA(NULL,TRUE,FALSE,NULL);REQUIRE(job.event&&job.ready&&job.hold,23);
    HANDLE port=NULL;
    if(!apc){port=CreateIoCompletionPort(job.file,NULL,0x13572468,1);REQUIRE(port,24);}
    callbacks=0;HANDLE thread=CreateThread(NULL,0,issue,&job,0,NULL);REQUIRE(thread,25);
    REQUIRE(WaitForSingleObject(job.ready,3000)==WAIT_OBJECT_0,26);
    REQUIRE(TerminateThread(thread,77)&&WaitForSingleObject(thread,3000)==WAIT_OBJECT_0,27);
    DWORD code;REQUIRE(GetExitCodeThread(thread,&code)&&code==77,28);
    REQUIRE(WaitForSingleObject(apc?job.file:job.event,3000)==WAIT_OBJECT_0,29);
    if(port)
    {
        DWORD bytes=99;ULONG_PTR key=0;OVERLAPPED *op=NULL;
        REQUIRE(!GetQueuedCompletionStatus(port,&bytes,&key,&op,3000)&&GetLastError()==ERROR_OPERATION_ABORTED,30);
        REQUIRE(!bytes&&key==0x13572468&&op==job.issued,31);
    }
    if(!stack)REQUIRE(job.shared.Internal==0xc0000120UL&&!job.shared.InternalHigh,32);
    /* A completion APC belonging to the terminated issuer must not be
     * redirected to this surviving alertable thread. */
    SleepEx(5,TRUE);REQUIRE(!callbacks,33);
    REQUIRE(CloseHandle(thread)&&CloseHandle(job.ready)&&CloseHandle(job.hold)&&CloseHandle(job.event),34);
    if(port)REQUIRE(CloseHandle(port),35);
    REQUIRE(CloseHandle(job.file)&&CloseHandle(client),36);
}
void WINAPI app_entry(void)
{
    for(unsigned i=0;i<8;i++)
    {
        one_case(FALSE,FALSE);one_case(TRUE,FALSE);
        one_case(FALSE,TRUE);one_case(TRUE,TRUE);
    }
    MESSAGE("io_thread_exit32: forced issuer termination, shared/stack I/O buffers, events, IOCP cancellation and no callback migration passed\r\n");
    ExitProcess(0);
}
