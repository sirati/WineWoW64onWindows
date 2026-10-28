/* Native Windows backend for the original Wine ws2_32 PE and WoW64 thunks. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define WS(x) x
#define WS_addrinfo addrinfo
#define WS_hostent hostent
#define WS_sockaddr sockaddr
typedef NTSTATUS (*unixlib_entry_t)(void *);
struct getaddrinfo_params;
struct gethostbyaddr_params;
struct gethostbyname_params;
struct gethostname_params;
struct getnameinfo_params;
static NTSTATUS unix_getaddrinfo(void *);
static NTSTATUS unix_gethostbyaddr(void *);
static NTSTATUS unix_gethostbyname(void *);
static NTSTATUS unix_gethostname(void *);
static NTSTATUS unix_getnameinfo(void *);
static int family_from_unix(int family) { return family; }
static int errno_from_unix(int unused) { (void)unused;return WSAGetLastError(); }
#include "out/wine-socket-dns.inc"
static INIT_ONCE winsock_once=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK initialize_winsock(INIT_ONCE *once,void *param,void **context)
{
    WSADATA data;(void)once;(void)param;(void)context;
    int status=WSAStartup(MAKEWORD(2,2),&data);
    if(status)fprintf(stderr,"NATIVE_WSA_STARTUP_FAILED %d\n",status);
    return !status;
}
static BOOL ensure_winsock(void)
{ return InitOnceExecuteOnce(&winsock_once,initialize_winsock,NULL,NULL); }
BOOL probe_socket_ready(void) { return ensure_winsock(); }
static NTSTATUS unix_getaddrinfo(void *opaque)
{
    struct getaddrinfo_params *p=opaque;
    struct addrinfo hints={0},*info,*src,*previous=NULL,*dst=p->info;
    if(!ensure_winsock())return WSASYSNOTREADY;
    if(p->hints)
    { hints.ai_flags=p->hints->ai_flags;hints.ai_family=p->hints->ai_family;
      hints.ai_socktype=p->hints->ai_socktype;hints.ai_protocol=p->hints->ai_protocol; }
    int status=getaddrinfo(p->node,p->service,p->hints?&hints:NULL,&info);
    if(status)return status;
    SIZE_T needed=0;
    for(src=info;src;src=src->ai_next)
        needed+=sizeof(*dst)+src->ai_addrlen+(src->ai_canonname?strlen(src->ai_canonname)+1:0);
    if(needed>*p->size) { *p->size=needed;freeaddrinfo(info);return ERROR_INSUFFICIENT_BUFFER; }
    for(src=info;src;src=src->ai_next)
    {
        char *next=(char *)(dst+1);*dst=*src;dst->ai_next=NULL;
        if(src->ai_canonname)
        { dst->ai_canonname=next;strcpy(next,src->ai_canonname);next+=strlen(next)+1; }
        dst->ai_addr=(struct sockaddr *)next;memcpy(next,src->ai_addr,src->ai_addrlen);next+=src->ai_addrlen;
        if(previous)previous->ai_next=dst;previous=dst;dst=(struct addrinfo *)next;
    }
    freeaddrinfo(info);return STATUS_SUCCESS;
}
static NTSTATUS unix_gethostbyaddr(void *opaque)
{
    struct gethostbyaddr_params *p=opaque;
    if(!ensure_winsock())return WSASYSNOTREADY;
    struct hostent *host=gethostbyaddr(p->addr,p->len,p->family);
    return host?hostent_from_unix(host,p->host,p->size):WSAGetLastError();
}
static NTSTATUS unix_gethostbyname(void *opaque)
{
    struct gethostbyname_params *p=opaque;
    if(!ensure_winsock())return WSASYSNOTREADY;
    struct hostent *host=gethostbyname(p->name);
    return host?hostent_from_unix(host,p->host,p->size):WSAGetLastError();
}
static NTSTATUS unix_gethostname(void *opaque)
{
    struct gethostname_params *p=opaque;
    if(!ensure_winsock())return SOCKET_ERROR;
    return gethostname(p->name,p->size);
}
static NTSTATUS unix_getnameinfo(void *opaque)
{
    struct getnameinfo_params *p=opaque;
    if(!ensure_winsock())return WSASYSNOTREADY;
    return getnameinfo(p->addr,p->addr_len,p->host,p->host_len,p->serv,p->serv_len,p->flags);
}
BOOL probe_socket_unix_dispatch(ULONGLONG handle,ULONG code,void *args,NTSTATUS *result)
{
    if(handle!=(ULONG_PTR)__wine_unix_call_wow64_funcs)return FALSE;
    if(code==ws_unix_funcs_count)
    {
        struct { int family,type,protocol;DWORD flags,handle,error; } *p=args;
        SOCKET socket=ensure_winsock()?WSASocketW(p->family,p->type,p->protocol,NULL,0,p->flags):INVALID_SOCKET;
        p->error=socket==INVALID_SOCKET?WSAGetLastError():0;p->handle=(ULONG)socket;
        *result=p->error?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;return TRUE;
    }
    *result=code<ws_unix_funcs_count?__wine_unix_call_wow64_funcs[code](args):STATUS_INVALID_PARAMETER;
    return TRUE;
}
BOOL probe_socket_builtin_unixlib(const char *name,ULONGLONG *handle)
{
    if(_stricmp(name,"ws2_32.dll"))return FALSE;
    *handle=(ULONG_PTR)__wine_unix_call_wow64_funcs;return TRUE;
}
