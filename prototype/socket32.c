#include <winsock2.h>
#include <ws2tcpip.h>
#include "test_support.h"
static void fail_at(DWORD stage)
{ MESSAGE("socket32 failure stage/socket-error: ");message_hex(stage);message_hex(WSAGetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static int (WINAPI *dll_send)(SOCKET);
static void address(SOCKET socket,struct sockaddr_in *addr)
{ int size=sizeof(*addr);REQUIRE(!getsockname(socket,(struct sockaddr *)addr,&size)&&size==sizeof(*addr),10); }
static void bind_loopback(SOCKET socket)
{
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    REQUIRE(!bind(socket,(struct sockaddr *)&addr,sizeof(addr)),11);
}
static void tcp_pair(SOCKET *client,SOCKET *server)
{
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);REQUIRE(listener!=INVALID_SOCKET,12);
    bind_loopback(listener);REQUIRE(!listen(listener,4),13);
    struct sockaddr_in addr;address(listener,&addr);
    *client=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);REQUIRE(*client!=INVALID_SOCKET,14);
    REQUIRE(!connect(*client,(struct sockaddr *)&addr,sizeof(addr)),15);
    *server=accept(listener,NULL,NULL);REQUIRE(*server!=INVALID_SOCKET,16);REQUIRE(!closesocket(listener),17);
}
static void received(SOCKET socket,OVERLAPPED *op,char *data,BOOL wait)
{
    DWORD n=99,flags=99;
    REQUIRE(WSAGetOverlappedResult(socket,op,&n,wait,&flags)&&n==4&&flags==0,18);
    REQUIRE(data[0]=='W'&&data[3]=='e'&&op->Internal==0&&op->InternalHigh==4,19);
}
static void receive_pending(SOCKET socket,OVERLAPPED *op,char *data)
{
    DWORD n,flags=0;WSABUF buffer={4,data};
    REQUIRE(WSARecv(socket,&buffer,1,&n,&flags,op,NULL)==SOCKET_ERROR&&WSAGetLastError()==WSA_IO_PENDING,20);
    REQUIRE(op->Internal==0x103,21);
}
static volatile DWORD callbacks;
static DWORD issuer;
static void CALLBACK socket_completion(DWORD error,DWORD bytes,OVERLAPPED *op,DWORD flags)
{
    REQUIRE(GetCurrentThreadId()==issuer&&error==ERROR_OPERATION_ABORTED&&bytes==0&&flags==0&&op->Internal==0xc0000120UL,22);
    InterlockedIncrement((LONG *)&callbacks);
}
static void tcp_test(void)
{
    SOCKET client,server;tcp_pair(&client,&server);char data[4];int size,type;DWORD n,flags=0;
    size=sizeof(type);REQUIRE(!getsockopt(server,SOL_SOCKET,SO_TYPE,(char *)&type,&size)&&type==SOCK_STREAM,30);
    type=1;REQUIRE(!setsockopt(server,IPPROTO_TCP,TCP_NODELAY,(char *)&type,sizeof(type)),31);
    type=0;size=sizeof(type);REQUIRE(!getsockopt(server,IPPROTO_TCP,TCP_NODELAY,(char *)&type,&size)&&type==1,32);
    u_long nonblock=1;REQUIRE(!ioctlsocket(server,FIONBIO,&nonblock),33);
    REQUIRE(recv(server,data,4,0)==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK,34);
    fd_set read;FD_ZERO(&read);FD_SET(server,&read);struct timeval timeout={0};
    REQUIRE(select(0,&read,NULL,NULL,&timeout)==0&&read.fd_count==0,35);
    REQUIRE(dll_send(client)==4,36);
    FD_ZERO(&read);FD_SET(server,&read);timeout.tv_sec=3;
    REQUIRE(select(0,&read,NULL,NULL,&timeout)==1&&FD_ISSET(server,&read),37);
    u_long available=0;REQUIRE(!ioctlsocket(server,FIONREAD,&available)&&available==4,38);
    WSAPOLLFD poll={server,POLLRDNORM,0};REQUIRE(WSAPoll(&poll,1,0)==1&&(poll.revents&POLLRDNORM),39);
    REQUIRE(recv(server,data,4,MSG_PEEK)==4&&data[0]=='W',40);
    REQUIRE(recv(server,data,4,0)==4&&data[3]=='e',41);
    nonblock=0;REQUIRE(!ioctlsocket(server,FIONBIO,&nonblock),42);
    WSAEVENT event=WSACreateEvent();REQUIRE(event!=WSA_INVALID_EVENT,43);
    REQUIRE(!WSAEventSelect(server,event,FD_READ|FD_CLOSE)&&dll_send(client)==4,44);
    REQUIRE(WSAWaitForMultipleEvents(1,&event,FALSE,3000,FALSE)==WSA_WAIT_EVENT_0,45);
    WSANETWORKEVENTS events;REQUIRE(!WSAEnumNetworkEvents(server,event,&events)&&(events.lNetworkEvents&FD_READ),46);
    REQUIRE(!WSAEventSelect(server,NULL,0),47);nonblock=0;REQUIRE(!ioctlsocket(server,FIONBIO,&nonblock),48);
    REQUIRE(recv(server,data,4,0)==4,49);
    OVERLAPPED op={0};op.hEvent=event;
    for(unsigned i=0;i<32;i++)
    { receive_pending(server,&op,data);REQUIRE(dll_send(client)==4,50);received(server,&op,data,TRUE); }
    receive_pending(server,&op,data);REQUIRE(CancelIoEx((HANDLE)server,&op),51);
    REQUIRE(WaitForSingleObject(event,3000)==WAIT_OBJECT_0,52);
    REQUIRE(!WSAGetOverlappedResult(server,&op,&n,FALSE,&flags)&&WSAGetLastError()==ERROR_OPERATION_ABORTED,53);
    op.hEvent=NULL;
    receive_pending(server,&op,data);REQUIRE(dll_send(client)==4,54);received(server,&op,data,TRUE);
    issuer=GetCurrentThreadId();WSABUF buffer={4,data};flags=0;
    REQUIRE(WSARecv(server,&buffer,1,&n,&flags,&op,socket_completion)==SOCKET_ERROR&&WSAGetLastError()==WSA_IO_PENDING,55);
    REQUIRE(CancelIoEx((HANDLE)server,&op),56);
    for(unsigned i=0;i<100&&!callbacks;i++)SleepEx(30,TRUE);
    REQUIRE(callbacks==1,57);
    HANDLE port=CreateIoCompletionPort((HANDLE)server,NULL,0xabc123,1);REQUIRE(port,58);
    receive_pending(server,&op,data);REQUIRE(dll_send(client)==4,59);
    OVERLAPPED *done=NULL;ULONG_PTR key=0;
    BOOL ok=GetQueuedCompletionStatus(port,&n,&key,&done,3000);
    if(!ok||n!=4||key!=0xabc123||done!=&op)
    { MESSAGE("socket completion ok/error/bytes/key/request/expected/status: ");message_hex(ok);message_hex(GetLastError());message_hex(n);message_hex(key);message_hex((ULONG_PTR)done);message_hex((ULONG_PTR)&op);message_hex(op.Internal); }
    REQUIRE(ok&&n==4&&key==0xabc123&&done==&op,60);
    received(server,&op,data,FALSE);
    receive_pending(server,&op,data);REQUIRE(CancelIoEx((HANDLE)server,&op),61);done=NULL;
    REQUIRE(!GetQueuedCompletionStatus(port,&n,&key,&done,3000)&&GetLastError()==ERROR_OPERATION_ABORTED&&done==&op&&key==0xabc123,62);
    REQUIRE(!shutdown(client,SD_SEND)&&recv(server,data,4,0)==0,63);
    REQUIRE(!closesocket(client)&&!closesocket(server)&&WSACloseEvent(event)&&CloseHandle(port),64);
}
static void udp_test(void)
{
    SOCKET client=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP),server=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    REQUIRE(client!=INVALID_SOCKET&&server!=INVALID_SOCKET,70);bind_loopback(server);
    struct sockaddr_in addr,source;address(server,&addr);int source_size=sizeof(source);char data[4];
    REQUIRE(sendto(client,"Wine",4,0,(struct sockaddr *)&addr,sizeof(addr))==4,71);
    REQUIRE(recvfrom(server,data,4,0,(struct sockaddr *)&source,&source_size)==4&&data[0]=='W'&&source.sin_addr.s_addr==htonl(INADDR_LOOPBACK),72);
    REQUIRE(!closesocket(client)&&!closesocket(server),73);
}
void WINAPI app_entry(void)
{
    WSADATA data;REQUIRE(!WSAStartup(MAKEWORD(2,2),&data),1);
    HMODULE library=LoadLibraryA("socket_library32.dll");REQUIRE(library,2);
    dll_send=(void *)GetProcAddress(library,"SocketSend32");REQUIRE(dll_send,3);
    tcp_test();udp_test();REQUIRE(FreeLibrary(library)&&!WSACleanup(),4);
    MESSAGE("socket32: Wine socket DLL, TCP/UDP, nonblocking, select/poll, options, events, overlapped/eventless receive, APC and IOCP cancellation passed\r\n");ExitProcess(0);
}
