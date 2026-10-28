/* Native Winsock provider below Wine's unchanged socket API and AFD layouts. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include "wine/afd.h"
#include "wine/condrv.h"
#include "native_io.h"
extern BOOL probe_socket_ready(void);
static NTSTATUS socket_status(int error)
{
    static const struct { NTSTATUS status;DWORD error; } errors[]={
#include "out/wine-socket-errors.inc"
    };
    if(!error)return STATUS_SUCCESS;
    if(error==WSAEALREADY)return STATUS_ADDRESS_ALREADY_ASSOCIATED;
    for(unsigned i=0;i<ARRAY_SIZE(errors);i++)if(errors[i].error==error)return errors[i].status;
    return STATUS_INVALID_PARAMETER;
}
#include "out/wine-socket-events.inc"
#define FIXME(...) do {} while(0)
#include "out/wine-socket-control.inc"
static void *socket_extension(SOCKET socket,GUID guid)
{
    void *function=NULL;DWORD bytes;
    if(WSAIoctl(socket,SIO_GET_EXTENSION_FUNCTION_POINTER,&guid,sizeof(guid),&function,sizeof(function),&bytes,NULL,NULL))return NULL;
    return function;
}
struct socket_io {
    SOCKET socket;
    BOOL overlapped,receive;
    DWORD flags,*guest_flags;
    WSAMSG message;
    WSABUF *buffers;
    void *guest_control;
    int *guest_addr_size;
    BOOL message_mode,connect,accept;
    SOCKET accepted;
    DWORD receive_size,local_size,remote_size;
    void *output,*payload,*control;
    LPFN_GETACCEPTEXSOCKADDRS get_addresses;
    TRANSMIT_FILE_BUFFERS transmit;
};
static void socket_complete(void *opaque,IO_STATUS_BLOCK *io,void *guest)
{
    struct socket_io *op=opaque;
    if(io&&io->Pointer!=guest&&(io->Status>=0||io->Status==STATUS_BUFFER_OVERFLOW))
    {
        if(op->connect&&setsockopt(op->socket,SOL_SOCKET,SO_UPDATE_CONNECT_CONTEXT,NULL,0))io->Status=socket_status(WSAGetLastError());
        if(op->accept)
        {
            SOCKET listener=op->socket;
            if(setsockopt(op->accepted,SOL_SOCKET,SO_UPDATE_ACCEPT_CONTEXT,(char *)&listener,sizeof(listener)))io->Status=socket_status(WSAGetLastError());
            struct sockaddr *local,*remote;int local_size,remote_size;
            struct sockaddr_storage a,b;
            op->get_addresses(op->output,op->receive_size,op->local_size,op->remote_size,&local,&local_size,&remote,&remote_size);
            if(local_size>sizeof(a)||remote_size>sizeof(b)||local_size+sizeof(int)>op->local_size||remote_size+sizeof(int)>op->remote_size)
                io->Status=STATUS_BUFFER_TOO_SMALL;
            else
            {
                memcpy(&a,local,local_size);memcpy(&b,remote,remote_size);
                char *out=(char *)op->output+op->receive_size;
                *(int *)out=local_size;memcpy(out+sizeof(int),&a,local_size);out+=op->local_size;
                *(int *)out=remote_size;memcpy(out+sizeof(int),&b,remote_size);
            }
        }
        if(op->receive)
        {
            DWORD bytes,flags=op->message_mode?op->message.dwFlags:op->flags;
            if(op->overlapped&&!op->message_mode)WSAGetOverlappedResult(op->socket,(OVERLAPPED *)io,&bytes,FALSE,&flags);
            if(op->message_mode)
            {
                if(op->guest_addr_size)*op->guest_addr_size=op->message.namelen;
                if(op->guest_control&&!wow64_translate_control(&op->message.Control,op->guest_control))
                { flags|=MSG_CTRUNC;io->Status=STATUS_BUFFER_OVERFLOW; }
            }
            if(op->guest_flags)*op->guest_flags=flags;
            if(op->overlapped&&guest)((DWORD *)guest)[2]=flags;
        }
    }
    HeapFree(GetProcessHeap(),0,op->payload);HeapFree(GetProcessHeap(),0,op->control);
    HeapFree(GetProcessHeap(),0,op->buffers);HeapFree(GetProcessHeap(),0,op);
}
static NTSTATUS socket_buffers(struct socket_io *op,ULONG_PTR address,unsigned count)
{
    if(!count||count>0x100000)return STATUS_INVALID_PARAMETER;
    const struct afd_wsabuf_32 *guest=(void *)address;
    op->buffers=HeapAlloc(GetProcessHeap(),0,count*sizeof(*op->buffers));
    if(!op->buffers)return STATUS_NO_MEMORY;
    for(unsigned i=0;i<count;i++)
    { op->buffers[i].len=guest[i].len;op->buffers[i].buf=(void *)(ULONG_PTR)guest[i].buf; }
    return STATUS_SUCCESS;
}
static int poll_timeout(LONGLONG timeout)
{
    if(timeout==0x7fffffffffffffffLL)return -1;
    if(timeout>0)
    { LARGE_INTEGER now;NtQuerySystemTime(&now);timeout=now.QuadPart-timeout; }
    if(timeout>=0)return 0;
    ULONGLONG ms=(-timeout+9999)/10000;
    return ms>INT_MAX?INT_MAX:ms;
}
static NTSTATUS socket_poll(void *input,ULONG input_size,void *output,ULONG output_size,ULONG_PTR *information)
{
    struct afd_poll_params_32 *in=input,*out=output;
    if(input_size<offsetof(struct afd_poll_params_32,sockets)||!in->count||in->count>0x100000||
       in->count>(input_size-16)/sizeof(in->sockets[0])||output_size<input_size)return STATUS_INVALID_PARAMETER;
    WSAPOLLFD *polls=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,in->count*sizeof(*polls));
    if(!polls)return STATUS_NO_MEMORY;
    unsigned count=in->count;
    for(unsigned i=0;i<count;i++)
    {
        polls[i].fd=in->sockets[i].socket;
        if(in->sockets[i].flags&(AFD_POLL_READ|AFD_POLL_ACCEPT|AFD_POLL_HUP))polls[i].events|=POLLRDNORM;
        if(in->sockets[i].flags&AFD_POLL_WRITE)polls[i].events|=POLLWRNORM;
        if(in->sockets[i].flags&AFD_POLL_OOB)polls[i].events|=POLLRDBAND;
    }
    int ret=WSAPoll(polls,count,poll_timeout(in->timeout));
    NTSTATUS status=ret==SOCKET_ERROR?socket_status(WSAGetLastError()):ret?STATUS_SUCCESS:STATUS_TIMEOUT;
    if(ret>=0)
    {
        if(out!=in)memcpy(out,in,input_size);
        for(unsigned i=0;i<count;i++)
        {
            int requested=out->sockets[i].flags,flags=0;
            if(polls[i].revents&POLLRDNORM)flags|=requested&(AFD_POLL_READ|AFD_POLL_ACCEPT);
            if(polls[i].revents&POLLWRNORM)flags|=AFD_POLL_WRITE|AFD_POLL_CONNECT;
            if(polls[i].revents&POLLRDBAND)flags|=AFD_POLL_OOB;
            if(polls[i].revents&POLLHUP)flags|=AFD_POLL_HUP;
            if(polls[i].revents&POLLERR)flags|=AFD_POLL_CONNECT_ERR|AFD_POLL_RESET;
            out->sockets[i].flags=flags&requested;
            out->sockets[i].status=polls[i].revents&POLLNVAL?STATUS_INVALID_HANDLE:STATUS_SUCCESS;
        }
        *information=input_size;
    }
    HeapFree(GetProcessHeap(),0,polls);return status;
}
static NTSTATUS socket_call(HANDLE file,HANDLE event,void *context,IO_STATUS_BLOCK *io,const struct probe_io_args *args)
{
    struct socket_io *op=args->opaque;
    SOCKET s=(SOCKET)file;NTSTATUS status=STATUS_SUCCESS;ULONG_PTR information=0;int ret=0;
    (void)context;
    OVERLAPPED *overlapped=(OVERLAPPED *)io;
    /* Persistent union storage belongs to the common Wine IOSB adapter. */
    overlapped->hEvent=event;overlapped->Offset=0;overlapped->OffsetHigh=0;
    switch(args->code)
    {
    case IOCTL_AFD_BIND:
    {
        const struct afd_bind_params *p=args->buffer;
        if(args->size<sizeof(int)+sizeof(ADDRESS_FAMILY))return STATUS_INVALID_PARAMETER;
        ret=bind(s,&p->addr,args->size-sizeof(int));
        if(!ret&&args->output)
        { int size=args->output_size;ret=getsockname(s,args->output,&size);information=size; }
        break;
    }
    case IOCTL_AFD_LISTEN:
        if(args->size<sizeof(struct afd_listen_params))return STATUS_INVALID_PARAMETER;
        ret=listen(s,((struct afd_listen_params *)args->buffer)->backlog);break;
    case IOCTL_AFD_WINE_ACCEPT:
    {
        if(args->output_size<sizeof(ULONG))return STATUS_BUFFER_TOO_SMALL;
        SOCKET accepted=accept(s,NULL,NULL);
        if(accepted==INVALID_SOCKET)ret=SOCKET_ERROR;
        else { *(ULONG *)args->output=accepted;information=sizeof(ULONG); }
        break;
    }
    case IOCTL_AFD_WINE_CONNECT:
    {
        const struct afd_connect_params *p=args->buffer;
        if(args->size<sizeof(*p)||p->addr_len<0||(ULONG)p->addr_len>args->size-sizeof(*p))return STATUS_INVALID_PARAMETER;
        if(p->synchronous)ret=connect(s,(const struct sockaddr *)(p+1),p->addr_len);
        else
        {
            LPFN_CONNECTEX function=socket_extension(s,(GUID)WSAID_CONNECTEX);DWORD bytes;
            if(!function)return socket_status(WSAGetLastError());
            SIZE_T size=args->size-sizeof(*p);
            op->payload=HeapAlloc(GetProcessHeap(),0,size);if(!op->payload)return STATUS_NO_MEMORY;
            memcpy(op->payload,p+1,size);op->connect=TRUE;op->overlapped=TRUE;
            ret=function(s,op->payload,p->addr_len,(char *)op->payload+p->addr_len,size-p->addr_len,&bytes,overlapped)?0:SOCKET_ERROR;
            if(ret==SOCKET_ERROR&&WSAGetLastError()==ERROR_IO_PENDING)return STATUS_PENDING;
            information=bytes;
        }
        break;
    }
    case IOCTL_AFD_WINE_ACCEPT_INTO:
    {
        const struct afd_accept_into_params *p=args->buffer;DWORD bytes=0;
        if(args->size<sizeof(*p)||p->recv_len>args->output_size||p->local_len>args->output_size-p->recv_len)return STATUS_INVALID_PARAMETER;
        LPFN_ACCEPTEX function=socket_extension(s,(GUID)WSAID_ACCEPTEX);
        op->get_addresses=socket_extension(s,(GUID)WSAID_GETACCEPTEXSOCKADDRS);
        if(!function||!op->get_addresses)return socket_status(WSAGetLastError());
        op->accept=TRUE;op->overlapped=TRUE;op->accepted=p->accept_handle;op->output=args->output;
        op->receive_size=p->recv_len;op->local_size=p->local_len;op->remote_size=args->output_size-p->recv_len-p->local_len;
        ret=function(s,op->accepted,args->output,op->receive_size,op->local_size,op->remote_size,&bytes,overlapped)?0:SOCKET_ERROR;
        if(ret==SOCKET_ERROR&&WSAGetLastError()==ERROR_IO_PENDING)return STATUS_PENDING;
        information=bytes;break;
    }
    case IOCTL_AFD_WINE_TRANSMIT:
    {
        const struct afd_transmit_params *p=args->buffer;
        if(args->size<sizeof(*p))return STATUS_INVALID_PARAMETER;
        LPFN_TRANSMITFILE function=socket_extension(s,(GUID)WSAID_TRANSMITFILE);
        if(!function)return socket_status(WSAGetLastError());
        op->transmit.Head=(void *)(ULONG_PTR)p->head_ptr;op->transmit.HeadLength=p->head_len;
        op->transmit.Tail=(void *)(ULONG_PTR)p->tail_ptr;op->transmit.TailLength=p->tail_len;
        overlapped->Offset=p->offset.LowPart;overlapped->OffsetHigh=p->offset.HighPart;op->overlapped=TRUE;
        ret=function(s,(HANDLE)(ULONG_PTR)p->file,p->file_len,p->buffer_size,overlapped,&op->transmit,p->flags)?0:SOCKET_ERROR;
        if(ret==SOCKET_ERROR&&WSAGetLastError()==ERROR_IO_PENDING)return STATUS_PENDING;
        information=io->Information;break;
    }
    case IOCTL_AFD_WINE_SHUTDOWN:
        if(args->size<sizeof(int))return STATUS_INVALID_PARAMETER;
        ret=shutdown(s,*(int *)args->buffer);break;
    case IOCTL_AFD_GETSOCKNAME:
    case IOCTL_AFD_WINE_GETPEERNAME:
    {
        int size=args->output_size;
        ret=args->code==IOCTL_AFD_GETSOCKNAME?getsockname(s,args->output,&size):getpeername(s,args->output,&size);
        information=size;break;
    }
    case IOCTL_AFD_WINE_GET_INFO:
    {
        WSAPROTOCOL_INFOW info;int size=sizeof(info);
        if(args->output_size<sizeof(struct afd_get_info_params))return STATUS_BUFFER_TOO_SMALL;
        ret=getsockopt(s,SOL_SOCKET,SO_PROTOCOL_INFOW,(char *)&info,&size);
        if(!ret)
        { struct afd_get_info_params *out=args->output;out->family=info.iAddressFamily;out->type=info.iSocketType;out->protocol=info.iProtocol;information=sizeof(*out); }
        break;
    }
    case IOCTL_AFD_WINE_FIONBIO:
    case IOCTL_AFD_WINE_FIONREAD:
    case IOCTL_AFD_WINE_SIOCATMARK:
    {
        u_long value=args->size>=sizeof(ULONG)?*(ULONG *)args->buffer:0;
        LONG command=args->code==IOCTL_AFD_WINE_FIONBIO?FIONBIO:args->code==IOCTL_AFD_WINE_FIONREAD?FIONREAD:SIOCATMARK;
        ret=ioctlsocket(s,command,&value);
        if(!ret&&args->output_size>=sizeof(value)){*(ULONG *)args->output=value;information=sizeof(value);}
        break;
    }
    case IOCTL_AFD_POLL:
        status=socket_poll(args->buffer,args->size,args->output,args->output_size,&information);break;
    case IOCTL_AFD_EVENT_SELECT:
    {
        const struct afd_event_select_params_32 *p=args->buffer;
        if(args->size<sizeof(*p))return STATUS_INVALID_PARAMETER;
        ret=WSAEventSelect(s,(HANDLE)(ULONG_PTR)p->event,afd_poll_flag_to_win32(p->mask));break;
    }
    case IOCTL_AFD_GET_EVENTS:
    {
        WSANETWORKEVENTS events;struct afd_get_events_params *out=args->output;
        static const unsigned bits[]={FD_READ_BIT,FD_OOB_BIT,FD_WRITE_BIT,FD_CLOSE_BIT,FD_CLOSE_BIT,FD_CLOSE_BIT,FD_CONNECT_BIT,FD_ACCEPT_BIT,FD_CONNECT_BIT};
        if(args->output_size<sizeof(*out))return STATUS_BUFFER_TOO_SMALL;
        ret=WSAEnumNetworkEvents(s,(HANDLE)args->buffer,&events);
        if(!ret)
        {
            memset(out,0,sizeof(*out));out->flags=afd_poll_flag_from_win32(events.lNetworkEvents);
            for(unsigned i=0;i<ARRAY_SIZE(bits);i++)out->status[i]=socket_status(events.iErrorCode[bits[i]]);
            if((events.lNetworkEvents&FD_CONNECT)&&!events.iErrorCode[FD_CONNECT_BIT])out->flags&=~AFD_POLL_CONNECT_ERR;
            information=sizeof(*out);
        }
        break;
    }
    case IOCTL_AFD_RECV:
    case IOCTL_AFD_WINE_RECVMSG:
    case IOCTL_AFD_WINE_SENDMSG:
    {
        DWORD count,bytes=0;ULONG_PTR buffers;struct sockaddr *address=NULL;int *address_size=NULL;int send_address_size=0;
        if(args->code==IOCTL_AFD_WINE_SENDMSG)
        {
            const struct afd_sendmsg_params *p=args->buffer;
            if(args->size<sizeof(*p))return STATUS_INVALID_PARAMETER;
            op->overlapped=p->force_async;count=p->count;buffers=p->buffers_ptr;op->flags=p->ws_flags;
            address=(void *)(ULONG_PTR)p->addr_ptr;send_address_size=p->addr_len;
        }
        else if(args->code==IOCTL_AFD_WINE_RECVMSG)
        {
            const struct afd_recvmsg_params *p=args->buffer;
            if(args->size<sizeof(*p))return STATUS_INVALID_PARAMETER;
            op->receive=TRUE;op->overlapped=p->force_async;count=p->count;buffers=p->buffers_ptr;
            op->guest_flags=(void *)(ULONG_PTR)p->ws_flags_ptr;
            op->flags=op->guest_flags?*op->guest_flags:0;
            address=(void *)(ULONG_PTR)p->addr_ptr;address_size=(void *)(ULONG_PTR)p->addr_len_ptr;
            if(p->control_ptr)
            {
                struct afd_wsabuf_32 *control=(void *)(ULONG_PTR)p->control_ptr;
                if(control->len>0x1000000)return STATUS_INVALID_PARAMETER;
                op->message_mode=TRUE;op->guest_control=control;op->guest_addr_size=address_size;
                op->message.name=address;op->message.namelen=address_size?*address_size:0;op->message.dwFlags=op->flags;
                op->message.Control.len=2*control->len;
                op->control=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,op->message.Control.len);
                if(!op->control&&control->len)return STATUS_NO_MEMORY;
                op->message.Control.buf=op->control;
            }
        }
        else
        {
            const struct afd_recv_params_32 *p=args->buffer;
            if(args->size<sizeof(*p))return STATUS_INVALID_PARAMETER;
            op->receive=TRUE;op->overlapped=(p->recv_flags&AFD_RECV_FORCE_ASYNC)!=0;count=p->count;buffers=p->buffers;
            op->flags=(p->msg_flags&AFD_MSG_OOB?MSG_OOB:0)|(p->msg_flags&AFD_MSG_PEEK?MSG_PEEK:0)|(p->msg_flags&AFD_MSG_WAITALL?MSG_WAITALL:0);
        }
        if((status=socket_buffers(op,buffers,count)))return status;
        OVERLAPPED *ov=op->overlapped?overlapped:NULL;
        if(op->message_mode)
        {
            LPFN_WSARECVMSG function=socket_extension(s,(GUID)WSAID_WSARECVMSG);
            if(!function)return socket_status(WSAGetLastError());
            op->message.lpBuffers=op->buffers;op->message.dwBufferCount=count;
            ret=function(s,&op->message,&bytes,ov,NULL);
        }
        else if(op->receive)
            ret=address?WSARecvFrom(s,op->buffers,count,&bytes,&op->flags,address,address_size,ov,NULL):
                        WSARecv(s,op->buffers,count,&bytes,&op->flags,ov,NULL);
        else
            ret=address?WSASendTo(s,op->buffers,count,&bytes,op->flags,address,send_address_size,ov,NULL):
                        WSASend(s,op->buffers,count,&bytes,op->flags,ov,NULL);
        if(ret==SOCKET_ERROR&&WSAGetLastError()==WSA_IO_PENDING)return STATUS_PENDING;
        information=bytes;break;
    }
    case IOCTL_AFD_WINE_COMPLETE_ASYNC:
        if(args->size<sizeof(NTSTATUS))return STATUS_INVALID_PARAMETER;
        status=*(NTSTATUS *)args->buffer;break;
    case IOCTL_AFD_WINE_KEEPALIVE_VALS:
    case IOCTL_AFD_WINE_GET_INTERFACE_LIST:
    case IOCTL_AFD_WINE_ADDRESS_LIST_CHANGE:
    {
        DWORD bytes=0;
        DWORD code=args->code==IOCTL_AFD_WINE_KEEPALIVE_VALS?SIO_KEEPALIVE_VALS:
                   args->code==IOCTL_AFD_WINE_GET_INTERFACE_LIST?SIO_GET_INTERFACE_LIST:SIO_ADDRESS_LIST_CHANGE;
        op->overlapped=args->code==IOCTL_AFD_WINE_ADDRESS_LIST_CHANGE;
        ret=WSAIoctl(s,code,args->code==IOCTL_AFD_WINE_ADDRESS_LIST_CHANGE?NULL:args->buffer,
                     args->code==IOCTL_AFD_WINE_ADDRESS_LIST_CHANGE?0:args->size,args->output,args->output_size,&bytes,
                     op->overlapped?overlapped:NULL,NULL);
        if(ret==SOCKET_ERROR&&WSAGetLastError()==ERROR_IO_PENDING)return STATUS_PENDING;
        information=bytes;break;
    }
    default:
    {
        static const struct { ULONG code;int level,option;BOOL set; } options[]={
#include "out/wine-socket-options.inc"
        };
        BOOL found=FALSE;
        for(unsigned i=0;i<ARRAY_SIZE(options);i++)if(options[i].code==args->code)
        {
            int size=args->output_size;
            ret=options[i].set?setsockopt(s,options[i].level,options[i].option,args->buffer,args->size):
                               getsockopt(s,options[i].level,options[i].option,args->output,&size);
            if(!options[i].set)information=size;
            found=TRUE;break;
        }
        if(!found){fprintf(stderr,"UNSUPPORTED_WINE_DEVICE_IOCTL %08lx\n",args->code);status=STATUS_NOT_SUPPORTED;}
        break;
    }
    }
    if(ret==SOCKET_ERROR)status=socket_status(WSAGetLastError());
    io->Status=status;io->Information=status>=0?information:0;
    if(status>=0)SetEvent((HANDLE)((ULONG_PTR)event&~(ULONG_PTR)1));
    return status;
}
NTSTATUS WINAPI probe_NtDeviceIoControlFile(HANDLE file,HANDLE event,PIO_APC_ROUTINE apc,void *context,
                                            IO_STATUS_BLOCK *io,ULONG code,void *input,ULONG input_size,void *output,ULONG output_size)
{
    extern int probe_trace;
    if(probe_trace)fprintf(stderr,"DEVICE_IOCTL code=%08lx file=%p in=%lu out=%lu\n",code,file,input_size,output_size);
    if(probe_trace&&code==IOCTL_CONDRV_WRITE_CONSOLE&&input&&!(input_size&1))
    {
        char text[2048];
        int length=WideCharToMultiByte(CP_UTF8,0,input,min(input_size/sizeof(WCHAR),512),text,sizeof(text)-1,NULL,NULL);
        if(length){text[length]=0;fprintf(stderr,"CONSOLE_WRITE_TEXT=%s\n",text);}
    }
    if(!probe_socket_ready())return STATUS_DEVICE_NOT_READY;
    struct socket_io *op=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*op));
    if(!op)return STATUS_NO_MEMORY;
    op->socket=(SOCKET)file;
    const struct probe_io_args args={.buffer=input,.size=input_size,.code=code,.output=output,.output_size=output_size,
                                    .opaque=op,.complete=socket_complete};
    return probe_submit_io(socket_call,file,event,apc,context,io,&args,FALSE);
}
BOOL probe_socket_close(HANDLE file,NTSTATUS *status)
{
    int type,size=sizeof(type);
    if(getsockopt((SOCKET)file,SOL_SOCKET,SO_TYPE,(char *)&type,&size))return FALSE;
    *status=closesocket((SOCKET)file)?socket_status(WSAGetLastError()):STATUS_SUCCESS;return TRUE;
}
NTSTATUS (WINAPI *__imp_probe_NtDeviceIoControlFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,void *,IO_STATUS_BLOCK *,ULONG,void *,ULONG,void *,ULONG)=probe_NtDeviceIoControlFile;
