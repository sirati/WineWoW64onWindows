#include "test_support.h"
static void fail_at(DWORD stage)
{ MESSAGE("io_notify32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static void pair(HANDLE *server,HANDLE *client)
{
    *server=CreateNamedPipeA("\\\\.\\pipe\\wine-io-notify32",PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE|PIPE_WAIT,1,4096,4096,0,NULL);
    REQUIRE(*server!=INVALID_HANDLE_VALUE,10);
    *client=CreateFileA("\\\\.\\pipe\\wine-io-notify32",GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    REQUIRE(*client!=INVALID_HANDLE_VALUE,11);
    OVERLAPPED op={0};op.hEvent=CreateEventA(NULL,TRUE,FALSE,NULL);REQUIRE(op.hEvent,12);
    REQUIRE(!ConnectNamedPipe(*server,&op)&&GetLastError()==ERROR_PIPE_CONNECTED,13);
    REQUIRE(CloseHandle(op.hEvent),14);
}
static void send_payload(HANDLE client)
{ DWORD n;REQUIRE(WriteFile(client,"Wine",4,&n,NULL)&&n==4,15); }
static void result(HANDLE file,OVERLAPPED *op,char *data)
{
    DWORD n=99;
    REQUIRE(GetOverlappedResult(file,op,&n,FALSE)&&n==4,16);
    REQUIRE(op->Internal==0&&op->InternalHigh==4&&data[0]=='W'&&data[3]=='e',17);
}
static void pending(HANDLE file,OVERLAPPED *op,char *data)
{ DWORD n;REQUIRE(!ReadFile(file,data,4,&n,op)&&GetLastError()==ERROR_IO_PENDING&&op->Internal==0x103,18); }
static volatile DWORD callback_count;
static DWORD issuer;
static OVERLAPPED *expected;
static DWORD expected_error,expected_bytes;
static void CALLBACK completion(DWORD error,DWORD bytes,OVERLAPPED *op)
{
    REQUIRE(GetCurrentThreadId()==issuer&&op==expected,20);
    REQUIRE(error==expected_error&&bytes==expected_bytes&&op->InternalHigh==bytes,21);
    InterlockedIncrement((LONG *)&callback_count);
}
static void wait_callback(void)
{
    for(unsigned i=0;i<100&&!callback_count;i++)SleepEx(30,TRUE);
    REQUIRE(callback_count==1,22);
}
static unsigned char bulk[65536];
static DWORD WINAPI drain(void *opaque)
{
    HANDLE file=opaque;unsigned char buffer[2048];DWORD n,total=0;
    while(total<sizeof(bulk))
    { REQUIRE(ReadFile(file,buffer,sizeof(buffer),&n,NULL)&&n,23);total+=n; }
    REQUIRE(total==sizeof(bulk),24);return 0;
}
static void apc_test(void)
{
    HANDLE file,client;pair(&file,&client);char data[4];OVERLAPPED op={0};
    issuer=GetCurrentThreadId();expected=&op;expected_error=0;expected_bytes=4;callback_count=0;
    REQUIRE(ReadFileEx(file,data,4,&op,completion),25);
    send_payload(client);Sleep(20);REQUIRE(!callback_count,26);
    wait_callback();result(file,&op,data);
    op.Internal=0;callback_count=0;expected_error=ERROR_OPERATION_ABORTED;expected_bytes=0;
    REQUIRE(ReadFileEx(file,data,4,&op,completion)&&CancelIoEx(file,&op),27);
    wait_callback();REQUIRE(op.Internal==0xc0000120UL,28);
    op.Internal=0;callback_count=0;expected_error=0;expected_bytes=sizeof(bulk);
    REQUIRE(WriteFileEx(file,bulk,sizeof(bulk),&op,completion),29);
    REQUIRE(!callback_count,30);
    HANDLE worker=CreateThread(NULL,0,drain,client,0,NULL);REQUIRE(worker,31);
    wait_callback();REQUIRE(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0,32);
    REQUIRE(CloseHandle(worker)&&CloseHandle(file)&&CloseHandle(client),33);
}
static void eventless_test(void)
{
    HANDLE file,client,duplicate,event;pair(&file,&client);
    REQUIRE(DuplicateHandle(GetCurrentProcess(),file,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS),40);
    event=CreateEventA(NULL,TRUE,FALSE,NULL);REQUIRE(event,41);
    char data[4];OVERLAPPED op={0};HANDLE handles[2]={event,duplicate};
    for(unsigned i=0;i<128;i++)
    {
        pending(file,&op,data);
        REQUIRE(WaitForSingleObject(duplicate,0)==WAIT_TIMEOUT,42);
        REQUIRE(WaitForMultipleObjects(2,handles,FALSE,0)==WAIT_TIMEOUT,43);
        send_payload(client);
        REQUIRE(WaitForMultipleObjects(2,handles,FALSE,3000)==WAIT_OBJECT_0+1,44);
        result(file,&op,data);
        REQUIRE(SetEvent(event)&&WaitForMultipleObjects(2,handles,TRUE,0)==WAIT_OBJECT_0,45);
        REQUIRE(ResetEvent(event),46);
    }
    REQUIRE(CloseHandle(file),47);pending(duplicate,&op,data);
    REQUIRE(CancelIoEx(duplicate,&op)&&WaitForSingleObject(duplicate,3000)==WAIT_OBJECT_0,48);
    DWORD n;REQUIRE(!GetOverlappedResult(duplicate,&op,&n,FALSE)&&GetLastError()==ERROR_OPERATION_ABORTED,49);
    REQUIRE(CloseHandle(duplicate)&&CloseHandle(event)&&CloseHandle(client),50);
}
static void packet(HANDLE port,OVERLAPPED *expected_op,DWORD error)
{
    ULONG_PTR key=0;OVERLAPPED *op=NULL;DWORD n=99;
    BOOL ok=GetQueuedCompletionStatus(port,&n,&key,&op,3000);
    REQUIRE((error?!ok&&GetLastError()==error:ok)&&key==0x13572468&&op==expected_op,60);
    REQUIRE(n==(error?0:4)&&op->InternalHigh==n&&op->Internal==(error?0xc0000120UL:0),61);
}
static void empty(HANDLE port)
{
    ULONG_PTR key;OVERLAPPED *op=(void *)1;DWORD n;
    REQUIRE(!GetQueuedCompletionStatus(port,&n,&key,&op,0)&&GetLastError()==WAIT_TIMEOUT&&!op,62);
}
static void port_test(void)
{
    HANDLE file,client;pair(&file,&client);OVERLAPPED op={0};char data[4];DWORD n;
    HANDLE port=CreateIoCompletionPort(file,NULL,0x13572468,1);REQUIRE(port,63);
    for(unsigned i=0;i<64;i++)
    {
        pending(file,&op,data);send_payload(client);packet(port,&op,0);result(file,&op,data);
        pending(file,&op,data);REQUIRE(CancelIoEx(file,&op),64);packet(port,&op,ERROR_OPERATION_ABORTED);
    }
    op.hEvent=CreateEventA(NULL,TRUE,FALSE,NULL);REQUIRE(op.hEvent,65);
    HANDLE event=op.hEvent;op.hEvent=(HANDLE)((ULONG_PTR)event|1);
    pending(file,&op,data);send_payload(client);
    REQUIRE(WaitForSingleObject(event,3000)==WAIT_OBJECT_0,66);result(file,&op,data);empty(port);
    op.hEvent=NULL;REQUIRE(CloseHandle(event),67);
    send_payload(client);REQUIRE(ReadFile(file,data,4,&n,&op)&&n==4,68);packet(port,&op,0);
    REQUIRE(SetFileCompletionNotificationModes(file,FILE_SKIP_COMPLETION_PORT_ON_SUCCESS),69);
    send_payload(client);REQUIRE(ReadFile(file,data,4,&n,&op)&&n==4,70);result(file,&op,data);empty(port);
    pending(file,&op,data);send_payload(client);packet(port,&op,0);
    REQUIRE(PostQueuedCompletionStatus(port,7,0x1111,(OVERLAPPED *)0x1234),71);
    REQUIRE(PostQueuedCompletionStatus(port,8,0x2222,(OVERLAPPED *)0x5678),72);
    OVERLAPPED_ENTRY entries[2];ULONG count=0;
    REQUIRE(GetQueuedCompletionStatusEx(port,entries,2,&count,3000,FALSE)&&count==2,73);
    REQUIRE(entries[0].lpCompletionKey==0x1111&&entries[0].dwNumberOfBytesTransferred==7&&
            entries[0].lpOverlapped==(OVERLAPPED *)0x1234&&entries[1].lpCompletionKey==0x2222&&
            entries[1].dwNumberOfBytesTransferred==8&&entries[1].lpOverlapped==(OVERLAPPED *)0x5678,74);
    REQUIRE(CloseHandle(file)&&CloseHandle(client)&&CloseHandle(port),75);
}
void WINAPI app_entry(void)
{
    eventless_test();apc_test();port_test();
    MESSAGE("io_notify32: eventless waits, file aliases, completion APCs, pending WriteFileEx, IOCP cancellation, low-bit suppression, skip-success and batched dequeue passed\r\n");
    ExitProcess(0);
}
