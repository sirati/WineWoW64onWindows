#include "test_support.h"
static void fail_at(DWORD stage)
{ MESSAGE("pipe_control32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(condition,stage) do { if(!(condition))fail_at(stage); } while(0)
static void command(HANDLE pipe,char value)
{
    DWORD n;char ack=0;
    REQUIRE(WriteFile(pipe,&value,1,&n,NULL)&&n==1,160);
    REQUIRE(ReadFile(pipe,&ack,1,&n,NULL)&&n==1&&ack=='A',161);
}
static void begin_connect(HANDLE pipe,OVERLAPPED *op)
{
    REQUIRE(SetEvent(op->hEvent),162);
    REQUIRE(!ConnectNamedPipe(pipe,op)&&GetLastError()==ERROR_IO_PENDING,163);
    REQUIRE(op->Internal==0x103&&WaitForSingleObject(op->hEvent,0)==WAIT_TIMEOUT,164);
}
static void completed(HANDLE pipe,OVERLAPPED *op,DWORD count)
{
    DWORD n=99;
    REQUIRE(WaitForSingleObject(op->hEvent,3000)==WAIT_OBJECT_0,165);
    REQUIRE(GetOverlappedResult(pipe,op,&n,FALSE)&&n==count&&op->Internal==0&&op->InternalHigh==count,166);
}
static void cancelled(HANDLE pipe,OVERLAPPED *op)
{
    DWORD n=99;
    REQUIRE(CancelIoEx(pipe,op),167);
    REQUIRE(WaitForSingleObject(op->hEvent,3000)==WAIT_OBJECT_0,168);
    REQUIRE(!GetOverlappedResult(pipe,op,&n,FALSE)&&GetLastError()==ERROR_OPERATION_ABORTED,169);
    REQUIRE(op->Internal==0xc0000120UL&&op->InternalHigh==0,170);
}
static void begin_transaction(HANDLE pipe,OVERLAPPED *op,char *reply)
{
    DWORD n=99;
    op->Internal=0;op->InternalHigh=0;
    REQUIRE(SetEvent(op->hEvent),171);
    REQUIRE(!TransactNamedPipe(pipe,"ping",4,reply,4,&n,op)&&GetLastError()==ERROR_IO_PENDING,172);
    DWORD event_state=WaitForSingleObject(op->hEvent,0);
    if(op->Internal!=0x103||event_state!=WAIT_TIMEOUT)
    {
        MESSAGE("pending transaction bytes/status/info/event: ");
        message_hex(n);message_hex(op->Internal);message_hex(op->InternalHigh);message_hex(event_state);
        fail_at(173);
    }
    REQUIRE(!GetOverlappedResult(pipe,op,&n,FALSE)&&GetLastError()==ERROR_IO_INCOMPLETE,174);
}
void WINAPI app_entry(void)
{
    HANDLE control=CreateFileA("\\\\.\\pipe\\wine-fs-control-command",GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    REQUIRE(control!=INVALID_HANDLE_VALUE,175);
    HANDLE pipe=CreateNamedPipeA("\\\\.\\pipe\\wine-fs-control-target",PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
                                 PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,1,4096,4096,0,NULL);
    REQUIRE(pipe!=INVALID_HANDLE_VALUE,176);
    OVERLAPPED op={0};op.hEvent=CreateEventA(NULL,TRUE,TRUE,NULL);REQUIRE(op.hEvent,177);
    MESSAGE("pipe phase: initial connect\r\n");
    begin_connect(pipe,&op);command(control,'C');completed(pipe,&op,0);
    char reply[4];DWORD n,available,left;
    for(unsigned i=0;i<32;i++)
    {
        begin_transaction(pipe,&op,reply);command(control,'T');completed(pipe,&op,4);
        REQUIRE(reply[0]=='W'&&reply[1]=='i'&&reply[2]=='n'&&reply[3]=='e',178);
    }
    MESSAGE("pipe phase: peek\r\n");command(control,'P');
    REQUIRE(PeekNamedPipe(pipe,reply,2,&n,&available,&left)&&n==2&&available==4&&left==2&&reply[0]=='p'&&reply[1]=='e',179);
    REQUIRE(ReadFile(pipe,reply,4,&n,&op)&&n==4,180);completed(pipe,&op,4);
    REQUIRE(reply[0]=='p'&&reply[1]=='e'&&reply[2]=='e'&&reply[3]=='k',181);
    REQUIRE(PeekNamedPipe(pipe,NULL,0,NULL,&available,NULL)&&available==0,182);
    MESSAGE("pipe phase: transaction cancel\r\n");
    begin_transaction(pipe,&op,reply);cancelled(pipe,&op);command(control,'D');
    begin_transaction(pipe,&op,reply);command(control,'T');completed(pipe,&op,4);
    MESSAGE("pipe phase: listen cancel/reconnect\r\n");
    REQUIRE(DisconnectNamedPipe(pipe),183);command(control,'X');
    begin_connect(pipe,&op);cancelled(pipe,&op);
    begin_connect(pipe,&op);command(control,'C');completed(pipe,&op,0);
    begin_transaction(pipe,&op,reply);command(control,'T');completed(pipe,&op,4);
    MESSAGE("pipe phase: client before listen\r\n");
    REQUIRE(DisconnectNamedPipe(pipe),184);command(control,'X');
    /* A client may arrive before the first ConnectNamedPipe on a newly
     * created instance. A disconnected reused instance must listen first. */
    REQUIRE(CloseHandle(pipe),192);
    pipe=CreateNamedPipeA("\\\\.\\pipe\\wine-fs-control-target",PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
                          PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,1,4096,4096,0,NULL);
    REQUIRE(pipe!=INVALID_HANDLE_VALUE,193);
    command(control,'C');
    REQUIRE(!ConnectNamedPipe(pipe,&op)&&GetLastError()==ERROR_PIPE_CONNECTED,185);
    begin_transaction(pipe,&op,reply);command(control,'T');completed(pipe,&op,4);
    command(control,'Q');
    REQUIRE(CloseHandle(pipe)&&CloseHandle(op.hEvent)&&CloseHandle(control),186);
    MESSAGE("pipe_control32: Wine pipe creation, pending connect/transact, pending publication, event reuse, peek, disconnect, targeted cancellation and reconnect passed\r\n");
    ExitProcess(0);
}
