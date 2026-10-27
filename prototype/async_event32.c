#include "test_support.h"
static void fail_at(DWORD stage)
{ MESSAGE("async_event32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(condition,stage) do { if(!(condition))fail_at(stage); } while(0)
static unsigned char payload[65536];
struct cancel_job { HANDLE file;OVERLAPPED *target;BOOL result;DWORD error; };
static DWORD WINAPI cancel_worker(void *opaque)
{
    struct cancel_job *job=opaque;
    job->result=CancelIoEx(job->file,job->target);job->error=GetLastError();
    return 0;
}
static void send_command(HANDLE pipe,HANDLE event,char command)
{
    OVERLAPPED op={0};DWORD n;op.hEvent=event;
    BOOL ok=WriteFile(pipe,&command,1,&n,&op);
    REQUIRE(ok||GetLastError()==ERROR_IO_PENDING,101);
    REQUIRE(GetOverlappedResult(pipe,&op,&n,TRUE)&&n==1,102);
}
static void begin_read(HANDLE pipe,OVERLAPPED *op,char *buffer)
{
    DWORD n=99;op->Internal=0;op->InternalHigh=0;
    REQUIRE(SetEvent(op->hEvent),103);
    REQUIRE(!ReadFile(pipe,buffer,4,&n,op)&&GetLastError()==ERROR_IO_PENDING,104);
    REQUIRE(n==0&&op->Internal==0x103&&WaitForSingleObject(op->hEvent,0)==WAIT_TIMEOUT,105);
    REQUIRE(!GetOverlappedResult(pipe,op,&n,FALSE)&&GetLastError()==ERROR_IO_INCOMPLETE,106);
}
static void finish_read(HANDLE pipe,OVERLAPPED *op,char *buffer,BOOL wait)
{
    DWORD n=99;BOOL ok;
    for(unsigned i=0;;i++)
    {
        ok=GetOverlappedResult(pipe,op,&n,wait);
        if(ok||wait||GetLastError()!=ERROR_IO_INCOMPLETE||i==3000)break;
        Sleep(1);
    }
    REQUIRE(ok&&n==4&&op->Internal==0&&op->InternalHigh==4,107);
    REQUIRE(buffer[0]=='W'&&buffer[1]=='i'&&buffer[2]=='n'&&buffer[3]=='e',108);
}
static void require_cancelled(HANDLE pipe,OVERLAPPED *op)
{
    DWORD n=99;
    REQUIRE(WaitForSingleObject(op->hEvent,3000)==WAIT_OBJECT_0,135);
    REQUIRE(!GetOverlappedResult(pipe,op,&n,FALSE)&&GetLastError()==ERROR_OPERATION_ABORTED,136);
    REQUIRE(op->Internal==0xc0000120UL&&op->InternalHigh==0,137);
}
struct exit_job { HANDLE pipe;OVERLAPPED *op;char *buffer;BOOL explicit_exit; };
static DWORD WINAPI exit_with_pending_read(void *opaque)
{
    struct exit_job *job=opaque;
    begin_read(job->pipe,job->op,job->buffer);
    if(job->explicit_exit)ExitThread(0);
    return 0;
}
static void test_issuing_thread_exit(HANDLE pipe,OVERLAPPED *op,char *buffer)
{
    /* The worker's loader/TLS state disappears at exit. The application's
     * status block and buffer live on this still-running caller's stack. */
    for(unsigned i=0;i<32;i++)
    {
        struct exit_job job={pipe,op,buffer,i&1};DWORD code=99;
        HANDLE worker=CreateThread(NULL,0,exit_with_pending_read,&job,0,NULL);
        REQUIRE(worker,151);
        REQUIRE(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0&&GetExitCodeThread(worker,&code)&&!code,152);
        REQUIRE(CloseHandle(worker),153);
        require_cancelled(pipe,op);
    }
}
void WINAPI app_entry(void)
{
    HANDLE pipes[2],events[2],writes[2];OVERLAPPED reads[2]={{0},{0}};char data[2][4];DWORD n;
    for(unsigned i=0;i<2;i++)
    {
        const char *name=i?"\\\\.\\pipe\\wine-native-async-event-1":"\\\\.\\pipe\\wine-native-async-event-0";
        pipes[i]=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
        REQUIRE(pipes[i]!=INVALID_HANDLE_VALUE,109);
        events[i]=CreateEventA(NULL,TRUE,TRUE,NULL);writes[i]=CreateEventA(NULL,TRUE,FALSE,NULL);
        REQUIRE(events[i]&&writes[i],110);reads[i].hEvent=events[i];
    }
    begin_read(pipes[0],&reads[0],data[0]);send_command(pipes[0],writes[0],'R');
    finish_read(pipes[0],&reads[0],data[0],TRUE);
    /* Pipe quota is 4 KiB. The peer cannot drain this 64 KiB write until a
     * command on the other channel releases it, so pending is mandatory. */
    for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(unsigned char)i;
    send_command(pipes[0],writes[0],'W');
    OVERLAPPED bulk={0};bulk.hEvent=writes[0];
    REQUIRE(!WriteFile(pipes[0],payload,sizeof(payload),&n,&bulk)&&GetLastError()==ERROR_IO_PENDING,122);
    REQUIRE(bulk.Internal==0x103&&WaitForSingleObject(writes[0],0)==WAIT_TIMEOUT,123);
    send_command(pipes[1],writes[1],'S');
    REQUIRE(GetOverlappedResult(pipes[0],&bulk,&n,TRUE)&&n==sizeof(payload)&&bulk.InternalHigh==sizeof(payload),124);
    begin_read(pipes[0],&reads[0],data[0]);send_command(pipes[0],writes[0],'R');
    finish_read(pipes[0],&reads[0],data[0],FALSE);
    for(unsigned i=0;i<2;i++)begin_read(pipes[i],&reads[i],data[i]);
    REQUIRE(WaitForMultipleObjects(2,events,FALSE,0)==WAIT_TIMEOUT,154);
    for(unsigned i=0;i<2;i++)send_command(pipes[i],writes[i],'R');
    REQUIRE(WaitForMultipleObjects(2,events,TRUE,3000)==WAIT_OBJECT_0,155);
    finish_read(pipes[1],&reads[1],data[1],TRUE);finish_read(pipes[0],&reads[0],data[0],TRUE);
    REQUIRE(ResetEvent(events[0])&&WaitForMultipleObjects(2,events,FALSE,0)==WAIT_OBJECT_0+1,156);
    REQUIRE(WaitForMultipleObjects(2,events,TRUE,0)==WAIT_TIMEOUT,157);
    send_command(pipes[0],writes[0],'R');Sleep(200);
    REQUIRE(ReadFile(pipes[0],data[0],4,&n,&reads[0])&&n==4,111);
    finish_read(pipes[0],&reads[0],data[0],TRUE);
    OVERLAPPED second={0};char second_data[4];
    second.hEvent=CreateEventA(NULL,TRUE,FALSE,NULL);REQUIRE(second.hEvent,138);
    begin_read(pipes[0],&reads[0],data[0]);begin_read(pipes[0],&second,second_data);
    struct cancel_job job={pipes[0],&reads[0],FALSE,0};DWORD worker_code;
    HANDLE thread=CreateThread(NULL,0,cancel_worker,&job,0,NULL);REQUIRE(thread,139);
    REQUIRE(WaitForSingleObject(thread,3000)==WAIT_OBJECT_0&&GetExitCodeThread(thread,&worker_code)&&!worker_code,140);
    REQUIRE(CloseHandle(thread)&&job.result,141);
    require_cancelled(pipes[0],&reads[0]);
    REQUIRE(second.Internal==0x103&&WaitForSingleObject(second.hEvent,0)==WAIT_TIMEOUT,142);
    REQUIRE(!CancelIoEx(pipes[0],&reads[0])&&GetLastError()==ERROR_NOT_FOUND,143);
    send_command(pipes[0],writes[0],'R');finish_read(pipes[0],&second,second_data,TRUE);
    REQUIRE(CloseHandle(second.hEvent),144);
    HANDLE duplicate;
    REQUIRE(DuplicateHandle(GetCurrentProcess(),pipes[0],GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS),187);
    for(unsigned i=0;i<2;i++)
    {
        HANDLE issuing=i?duplicate:pipes[0],cancelling=i?pipes[0]:duplicate;
        begin_read(issuing,&reads[0],data[0]);
        REQUIRE(!CancelIoEx(pipes[1],&reads[0])&&GetLastError()==ERROR_NOT_FOUND,188);
        REQUIRE(reads[0].Internal==0x103&&WaitForSingleObject(events[0],0)==WAIT_TIMEOUT,189);
        REQUIRE(CancelIoEx(cancelling,&reads[0]),190);
        require_cancelled(issuing,&reads[0]);
    }
    REQUIRE(CloseHandle(duplicate),191);
    for(unsigned i=0;i<2;i++)begin_read(pipes[i],&reads[i],data[i]);
    REQUIRE(CancelIoEx(pipes[0],NULL),145);require_cancelled(pipes[0],&reads[0]);
    REQUIRE(reads[1].Internal==0x103&&WaitForSingleObject(events[1],0)==WAIT_TIMEOUT,146);
    REQUIRE(CancelIoEx(pipes[1],NULL),147);require_cancelled(pipes[1],&reads[1]);
    /* Completion runs on a native thread-pool worker while this thread may
     * still be translating/cancelling the guest request identifier. */
    for(unsigned i=0;i<64;i++)
    {
        begin_read(pipes[0],&reads[0],data[0]);send_command(pipes[0],writes[0],'r');
        BOOL requested=CancelIoEx(pipes[0],&reads[0]);
        REQUIRE(requested||GetLastError()==ERROR_NOT_FOUND,148);
        BOOL completed=GetOverlappedResult(pipes[0],&reads[0],&n,TRUE);
        if(completed)finish_read(pipes[0],&reads[0],data[0],FALSE);
        else
        {
            REQUIRE(GetLastError()==ERROR_OPERATION_ABORTED&&reads[0].InternalHigh==0,149);
            /* The peer's reply is still owed when cancellation wins. Drain
             * it before the next iteration, whether immediate or pending. */
            BOOL drain=ReadFile(pipes[0],data[0],4,&n,&reads[0]);
            REQUIRE(drain||GetLastError()==ERROR_IO_PENDING,150);
            finish_read(pipes[0],&reads[0],data[0],TRUE);
        }
    }
    begin_read(pipes[0],&reads[0],data[0]);REQUIRE(CancelIo(pipes[0]),112);
    REQUIRE(WaitForSingleObject(events[0],3000)==WAIT_OBJECT_0,113);
    REQUIRE(!GetOverlappedResult(pipes[0],&reads[0],&n,FALSE)&&GetLastError()==ERROR_OPERATION_ABORTED,114);
    REQUIRE(reads[0].Internal==0xc0000120UL&&reads[0].InternalHigh==0,115);
    begin_read(pipes[0],&reads[0],data[0]);
    REQUIRE(CloseHandle(events[0]),125);
    events[0]=CreateEventA(NULL,TRUE,FALSE,NULL);REQUIRE(events[0],126);
    REQUIRE(CancelIo(pipes[0]),127);
    BOOL closed_done=FALSE;
    for(unsigned i=0;i<3000;i++)
    {
        if(!GetOverlappedResult(pipes[0],&reads[0],&n,FALSE)&&GetLastError()==ERROR_OPERATION_ABORTED)
        { closed_done=TRUE;break; }
        Sleep(1);
    }
    REQUIRE(closed_done&&reads[0].InternalHigh==0&&WaitForSingleObject(events[0],0)==WAIT_TIMEOUT,128);
    reads[0].hEvent=events[0];
    test_issuing_thread_exit(pipes[0],&reads[0],data[0]);
    DWORD before_handles,after_handles;
    Sleep(20);REQUIRE(GetProcessHandleCount(GetCurrentProcess(),&before_handles),129);
    for(unsigned i=0;i<128;i++)
    {
        begin_read(pipes[0],&reads[0],data[0]);REQUIRE(CancelIo(pipes[0]),130);
        REQUIRE(WaitForSingleObject(events[0],3000)==WAIT_OBJECT_0,131);
        REQUIRE(reads[0].Internal==0xc0000120UL&&reads[0].InternalHigh==0,132);
    }
    Sleep(20);REQUIRE(GetProcessHandleCount(GetCurrentProcess(),&after_handles),133);
    REQUIRE(after_handles<=before_handles+4,134);
    begin_read(pipes[1],&reads[1],data[1]);REQUIRE(CloseHandle(pipes[1]),116);
    REQUIRE(WaitForSingleObject(events[1],3000)==WAIT_OBJECT_0,117);
    MESSAGE("close completion status/info: ");message_hex(reads[1].Internal);message_hex(reads[1].InternalHigh);
    /* Closing the peer races file cancellation. Native Windows reports
     * PIPE_BROKEN when the disconnect completes the pending read first. */
    REQUIRE((reads[1].Internal==0xc0000120UL||reads[1].Internal==0xc000014bUL)&&reads[1].InternalHigh==0,118);
    send_command(pipes[0],writes[0],'Q');REQUIRE(CloseHandle(pipes[0]),119);
    for(unsigned i=0;i<2;i++){REQUIRE(CloseHandle(events[i]),120);REQUIRE(CloseHandle(writes[i]),121);}
    MESSAGE("async_event32: pending reads/writes, polling, wait-any/all, event reuse, CancelIo, cross-thread and duplicate-handle targeted CancelIoEx, wrong-file rejection, cancel-all, completion races, issuing-thread return/ExitThread, closed events and handle cleanup passed\r\n");
    ExitProcess(0);
}
