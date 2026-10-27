#include <winsock2.h>
#include <ws2tcpip.h>
#include "test_support.h"
static void require(BOOL value,DWORD stage)
{ if(!value){MESSAGE("socket_dns32 failure stage/error: ");message_hex(stage);message_hex(WSAGetLastError());ExitProcess(stage);} }
void WINAPI app_entry(void)
{
    WSADATA startup;char name[256],host[128],service[32];struct addrinfo hints={0},*info=NULL;
    require(!WSAStartup(MAKEWORD(2,2),&startup),201);
    require(!gethostname(name,sizeof(name))&&name[0],202);
    hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;hints.ai_flags=AI_NUMERICHOST|AI_NUMERICSERV;
    require(!getaddrinfo("127.0.0.1","4567",&hints,&info)&&info,203);
    require(info->ai_family==AF_INET&&info->ai_addrlen==sizeof(struct sockaddr_in),204);
    struct sockaddr_in *address=(struct sockaddr_in *)info->ai_addr;
    require(address->sin_addr.s_addr==htonl(0x7f000001)&&address->sin_port==htons(4567),205);
    require(!getnameinfo(info->ai_addr,info->ai_addrlen,host,sizeof(host),service,sizeof(service),NI_NUMERICHOST|NI_NUMERICSERV),206);
    require(host[0]=='1'&&service[0]=='4'&&service[1]=='5'&&service[2]=='6'&&service[3]=='7'&&!service[4],207);
    freeaddrinfo(info);
    struct hostent *entry=gethostbyname("localhost");require(entry&&entry->h_addrtype==AF_INET&&entry->h_length==4,208);
    require(entry->h_addr_list&&entry->h_addr_list[0]&&entry->h_name,209);
    require(!WSACleanup(),210);
    MESSAGE("socket_dns32: Wine Winsock startup, hostname, address/host marshalling and numeric reverse lookup passed\r\n");
    ExitProcess(0);
}
