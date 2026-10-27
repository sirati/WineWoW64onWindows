#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include "test_support.h"
static void fail_at(DWORD stage)
{ MESSAGE("socket_extensions32 failure stage/socket-error: ");message_hex(stage);message_hex(WSAGetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static void *extension(SOCKET s,GUID id)
{
    void *fn=NULL;DWORD bytes;
    REQUIRE(!WSAIoctl(s,SIO_GET_EXTENSION_FUNCTION_POINTER,&id,sizeof(id),&fn,sizeof(fn),&bytes,NULL,NULL)&&fn,10);
    return fn;
}
static void bind_loopback(SOCKET socket,struct sockaddr_in *address)
{
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    REQUIRE(!bind(socket,(struct sockaddr *)&addr,sizeof(addr)),11);
    int size=sizeof(*address);REQUIRE(!getsockname(socket,(struct sockaddr *)address,&size),12);
}
static void complete(SOCKET s,OVERLAPPED *op,DWORD bytes)
{
    DWORD n=0,flags=0;
    REQUIRE(WSAGetOverlappedResult(s,op,&n,TRUE,&flags)&&n==bytes,13);
}
static void tcp_extensions(void)
{
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP),client=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP),
           accepted=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    REQUIRE(listener!=INVALID_SOCKET&&client!=INVALID_SOCKET&&accepted!=INVALID_SOCKET,20);
    struct sockaddr_in addr,local;bind_loopback(listener,&addr);bind_loopback(client,&local);REQUIRE(!listen(listener,4),21);
    LPFN_ACCEPTEX accept_ex=extension(listener,(GUID)WSAID_ACCEPTEX);
    LPFN_CONNECTEX connect_ex=extension(client,(GUID)WSAID_CONNECTEX);
    LPFN_GETACCEPTEXSOCKADDRS get_addresses=extension(listener,(GUID)WSAID_GETACCEPTEXSOCKADDRS);
    LPFN_TRANSMITFILE transmit=extension(client,(GUID)WSAID_TRANSMITFILE);
    OVERLAPPED accept_op={0},connect_op={0};
    accept_op.hEvent=CreateEventA(NULL,TRUE,FALSE,NULL);connect_op.hEvent=CreateEventA(NULL,TRUE,FALSE,NULL);
    REQUIRE(accept_op.hEvent&&connect_op.hEvent,22);
    char buffer[4+2*(sizeof(struct sockaddr_storage)+16)];DWORD n=0;
    REQUIRE(!accept_ex(listener,accepted,buffer,4,sizeof(struct sockaddr_storage)+16,sizeof(struct sockaddr_storage)+16,&n,&accept_op)&&WSAGetLastError()==ERROR_IO_PENDING,23);
    BOOL ok=connect_ex(client,(struct sockaddr *)&addr,sizeof(addr),"Wine",4,&n,&connect_op);
    REQUIRE(ok||WSAGetLastError()==ERROR_IO_PENDING,24);
    complete(client,&connect_op,4);complete(listener,&accept_op,4);
    REQUIRE(buffer[0]=='W'&&buffer[3]=='e',25);
    struct sockaddr *own,*peer;int own_size,peer_size;
    get_addresses(buffer,4,sizeof(struct sockaddr_storage)+16,sizeof(struct sockaddr_storage)+16,&own,&own_size,&peer,&peer_size);
    REQUIRE(own_size==sizeof(addr)&&peer_size==sizeof(addr)&&own->sa_family==AF_INET&&peer->sa_family==AF_INET,26);
    REQUIRE(((struct sockaddr_in *)own)->sin_port==addr.sin_port&&((struct sockaddr_in *)peer)->sin_port==local.sin_port,27);
    REQUIRE(!setsockopt(client,SOL_SOCKET,SO_UPDATE_CONNECT_CONTEXT,NULL,0),28);
    REQUIRE(!setsockopt(accepted,SOL_SOCKET,SO_UPDATE_ACCEPT_CONTEXT,(char *)&listener,sizeof(listener)),29);
    HANDLE file=CreateFileA("socket-transmit.tmp",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,NULL);
    REQUIRE(file!=INVALID_HANDLE_VALUE&&WriteFile(file,"Wine",4,&n,NULL)&&n==4,30);
    REQUIRE(SetFilePointer(file,0,NULL,FILE_BEGIN)==0,31);
    TRANSMIT_FILE_BUFFERS parts={"h",1,"t",1};OVERLAPPED send_op={0};send_op.hEvent=connect_op.hEvent;
    ok=transmit(client,file,4,0,&send_op,&parts,0);REQUIRE(ok||WSAGetLastError()==ERROR_IO_PENDING,32);
    unsigned total=0;char data[6];
    while(total<6){int got=recv(accepted,data+total,6-total,0);REQUIRE(got>0,33);total+=got;}
    complete(client,&send_op,6);REQUIRE(data[0]=='h'&&data[1]=='W'&&data[4]=='e'&&data[5]=='t',34);
    REQUIRE(CloseHandle(file)&&CloseHandle(accept_op.hEvent)&&CloseHandle(connect_op.hEvent),35);
    REQUIRE(!closesocket(accepted)&&!closesocket(client)&&!closesocket(listener),36);
}
static void recvmsg_test(void)
{
    SOCKET receiver=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP),sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    REQUIRE(receiver!=INVALID_SOCKET&&sender!=INVALID_SOCKET,40);
    struct sockaddr_in addr;bind_loopback(receiver,&addr);int enabled=1;
    REQUIRE(!setsockopt(receiver,IPPROTO_IP,IP_PKTINFO,(char *)&enabled,sizeof(enabled)),41);
    LPFN_WSARECVMSG receive=extension(receiver,(GUID)WSAID_WSARECVMSG);
    char data[4],control[128];struct sockaddr_storage source;WSABUF buffer={4,data};
    WSAMSG message={0};message.name=(struct sockaddr *)&source;message.namelen=sizeof(source);
    message.lpBuffers=&buffer;message.dwBufferCount=1;message.Control.buf=control;message.Control.len=sizeof(control);
    OVERLAPPED op={0};op.hEvent=CreateEventA(NULL,TRUE,FALSE,NULL);REQUIRE(op.hEvent,42);DWORD n;
    REQUIRE(receive(receiver,&message,&n,&op,NULL)==SOCKET_ERROR&&WSAGetLastError()==ERROR_IO_PENDING,43);
    REQUIRE(sendto(sender,"Wine",4,0,(struct sockaddr *)&addr,sizeof(addr))==4,44);complete(receiver,&op,4);
    REQUIRE(data[0]=='W'&&data[3]=='e'&&message.namelen==sizeof(addr)&&!message.dwFlags,45);
    BOOL found=FALSE;
    for(WSACMSGHDR *header=WSA_CMSG_FIRSTHDR(&message);header;header=WSA_CMSG_NXTHDR(&message,header))
        if(header->cmsg_level==IPPROTO_IP&&header->cmsg_type==IP_PKTINFO)
        {
            IN_PKTINFO *info=(void *)WSA_CMSG_DATA(header);
            REQUIRE(info->ipi_addr.s_addr==htonl(INADDR_LOOPBACK)&&info->ipi_ifindex,46);found=TRUE;
        }
    REQUIRE(found&&message.Control.len<=sizeof(control),47);
    REQUIRE(CloseHandle(op.hEvent)&&!closesocket(receiver)&&!closesocket(sender),48);
}
void WINAPI app_entry(void)
{
    WSADATA data;REQUIRE(!WSAStartup(MAKEWORD(2,2),&data),1);
    tcp_extensions();recvmsg_test();REQUIRE(!WSACleanup(),2);
    MESSAGE("socket_extensions32: AcceptEx, ConnectEx initial data, address conversion, TransmitFile buffers and WSARecvMsg IP packet-info control headers passed\r\n");ExitProcess(0);
}
