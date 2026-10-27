/* Small Win32 facade. NT argument conversion and CPU transitions use Wine. */
#define WIN32_NO_STATUS
#include <windef.h>
#include <winnt.h>
#include <winternl.h>
#include "wine/asm.h"
#include "shared.h"
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INFINITE 0xffffffffu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xffffffffu
typedef NTSTATUS (__stdcall *unix_call)(uint64_t,unsigned int,void *);
uint64_t bridge_handle;
unix_call bridge_gate;
void *host_exit_gate;
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,void *reserved) { (void)h;(void)reason;(void)reserved;return TRUE; }
static uint32_t call_host(unsigned int op, uint32_t value, uint32_t *high)
{
    struct bridge_args args = {value,0,0,0};
    bridge_gate(bridge_handle,op,&args);
    if(high) *high=args.high;
    return args.result;
}
static uint32_t call_host_args(unsigned int op, uint32_t value, uint32_t high, uint32_t extra,
                               uint32_t *out_high)
{
    struct bridge_args args = {value,0,high,extra};
    bridge_gate(bridge_handle,op,&args);
    if(out_high) *out_high=args.high;
    return args.result;
}
HANDLE WINAPI GetStdHandle(DWORD n) { return (HANDLE)(uintptr_t)call_host(BRIDGE_GET_STD_HANDLE,n,0); }
DWORD WINAPI GetCurrentProcessId(void) { return call_host(BRIDGE_GET_PID,0,0); }
ULONGLONG WINAPI GetTickCount64(void)
{
    uint32_t hi,lo=call_host(BRIDGE_GET_TICKS,0,&hi);
    return ((uint64_t)hi<<32)|lo;
}
BOOL WINAPI WriteFile(HANDLE f,const void *buf,DWORD size,DWORD *written,void *overlapped)
{
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    if(overlapped) return FALSE;
    status=NtWriteFile(f,0,0,0,&io,buf,size,0,0);
    if(written) *written=status>=0 ? io.Information : 0;
    return status>=0;
}
void *WINAPI VirtualAlloc(void *base,SIZE_T size,DWORD type,DWORD protect)
{
    return NtAllocateVirtualMemory((HANDLE)-1,&base,0,&size,type,protect)>=0 ? base : 0;
}
BOOL WINAPI VirtualFree(void *base,SIZE_T size,DWORD type)
{
    return NtFreeVirtualMemory((HANDLE)-1,&base,&size,type)>=0;
}
void WINAPI Sleep(DWORD ms)
{
    LARGE_INTEGER timeout; timeout.QuadPart=-(int64_t)ms*10000;
    NtDelayExecution(FALSE,&timeout);
}
DWORD WINAPI ProbeGui(void) { return call_host(BRIDGE_GUI_CHECK,0,0); }

DWORD WINAPI GetCurrentDirectoryA(DWORD size,char *buffer)
{
    return call_host_args(BRIDGE_GET_CWD,(uint32_t)(uintptr_t)buffer,0,size,0);
}
DWORD WINAPI GetCurrentThreadId(void) { return call_host(BRIDGE_GET_TID,0,0); }
HANDLE WINAPI GetProcessHeap(void) { return (HANDLE)(uintptr_t)call_host(BRIDGE_GET_HEAP,0,0); }
DWORD WINAPI TlsAlloc(void) { return call_host(BRIDGE_TLS_ALLOC,0,0); }
BOOL WINAPI TlsFree(DWORD idx) { return call_host(BRIDGE_TLS_FREE,idx,0)!=0; }
LPVOID WINAPI TlsGetValue(DWORD idx) { return (LPVOID)(uintptr_t)call_host(BRIDGE_TLS_GET,idx,0); }
BOOL WINAPI TlsSetValue(DWORD idx,LPVOID value) { return call_host_args(BRIDGE_TLS_SET,idx,(uint32_t)(uintptr_t)value,0,0)!=0; }

typedef struct { USHORT Length, MaximumLength; ULONG Buffer; } UNICODE_STRING32_PROBE;
typedef struct {
    ULONG Length, RootDirectory, ObjectName, Attributes, SecurityDescriptor, SecurityQualityOfService;
} OBJECT_ATTRIBUTES32_PROBE;
typedef struct { union { NTSTATUS Status; ULONG Pointer; }; ULONG Information; } IO_STATUS_BLOCK32_PROBE;
typedef struct {
    NTSTATUS ExitStatus; ULONG TebBaseAddress; ULONG ClientProcess; ULONG ClientThread;
    ULONG AffinityMask; LONG Priority; LONG BasePriority;
} THREAD_BASIC_INFORMATION32_PROBE;

static unsigned make_nt_path(const char *name, WCHAR *out, unsigned count)
{
    char cwd[512]; DWORD n=GetCurrentDirectoryA(sizeof(cwd),cwd); unsigned pos=0,i;
    if(!n || n>=sizeof(cwd)) return 0;
    if(count<8) return 0;
    out[pos++]=L'\\';out[pos++]=L'?';out[pos++]=L'?';out[pos++]=L'\\';
    for(i=0;i<n && pos+1<count;i++) out[pos++]=(WCHAR)(unsigned char)cwd[i];
    if(pos && out[pos-1]!=L'\\') out[pos++]=L'\\';
    for(i=0;name[i] && pos+1<count;i++) out[pos++]=(WCHAR)(unsigned char)name[i];
    out[pos]=0; return pos;
}

HANDLE WINAPI CreateFileA(const char *name,DWORD access,DWORD share,void *sa,DWORD disposition,
                          DWORD flags,HANDLE template)
{
    WCHAR path[1024]; UNICODE_STRING32_PROBE str; OBJECT_ATTRIBUTES32_PROBE oa;
    IO_STATUS_BLOCK32_PROBE io; HANDLE h=INVALID_HANDLE_VALUE; unsigned n;
    (void)sa;(void)flags;(void)template;
    n=make_nt_path(name,path,1024); if(!n) return h;
    str.Length=(USHORT)(n*sizeof(WCHAR));str.MaximumLength=str.Length+sizeof(WCHAR);
    str.Buffer=(ULONG)(uintptr_t)(path);
    oa.Length=sizeof(oa);oa.RootDirectory=0;oa.ObjectName=(ULONG)(uintptr_t)&str;
    oa.Attributes=0x40;oa.SecurityDescriptor=0;oa.SecurityQualityOfService=0;
    memset(&io,0,sizeof(io));
    if(NtCreateFile(&h,access,(POBJECT_ATTRIBUTES)&oa,(PIO_STATUS_BLOCK)&io,0,0,share,disposition,0,0,0)<0) return INVALID_HANDLE_VALUE;
    return h;
}
BOOL WINAPI CloseHandle(HANDLE h) { return NtClose(h)>=0; }
BOOL WINAPI DeleteFileA(const char *name)
{
    WCHAR path[1024]; UNICODE_STRING32_PROBE str; OBJECT_ATTRIBUTES32_PROBE oa; unsigned n;
    n=make_nt_path(name,path,1024);if(!n)return FALSE;
    str.Length=(USHORT)(n*sizeof(WCHAR));str.MaximumLength=str.Length+sizeof(WCHAR);str.Buffer=(ULONG)(uintptr_t)path;
    oa.Length=sizeof(oa);oa.RootDirectory=0;oa.ObjectName=(ULONG)(uintptr_t)&str;oa.Attributes=0x40;oa.SecurityDescriptor=0;oa.SecurityQualityOfService=0;
    return NtDeleteFile((POBJECT_ATTRIBUTES)&oa)>=0;
}
DWORD WINAPI WaitForSingleObject(HANDLE h,DWORD ms)
{
    /* Wine kernelbase/sync.c preserves informational statuses such as timeout
     * and maps NT errors to WAIT_FAILED. The facade does not set LastError. */
    LARGE_INTEGER timeout; NTSTATUS status;
    timeout.QuadPart=-(int64_t)ms*10000;
    status=NtWaitForSingleObject(h,FALSE,ms==INFINITE?0:&timeout);
    return ((ULONG)status>>30)==3?WAIT_FAILED:(DWORD)status;
}
BOOL WINAPI GetExitCodeThread(HANDLE h,DWORD *code)
{
    THREAD_BASIC_INFORMATION32_PROBE info; memset(&info,0,sizeof(info));
    if(NtQueryInformationThread(h,0,&info,sizeof(info),0)<0)return FALSE;
    if(code)*code=(DWORD)info.ExitStatus;return TRUE;
}
typedef DWORD (WINAPI *guest_start_t)(void *);
HANDLE WINAPI CreateThread(void *sa,SIZE_T stack,guest_start_t start,void *param,DWORD flags,DWORD *tid)
{
    uint32_t h=call_host_args(BRIDGE_CREATE_THREAD,(uint32_t)(uintptr_t)start,
                              (uint32_t)(uintptr_t)param,(uint32_t)stack,0);
    if(tid)*tid=0; (void)sa;(void)flags; return (HANDLE)(uintptr_t)h;
}
__ASM_STDCALL_FUNC(ExitThread,4,"movl 4(%esp),%eax\n\tjmp *" __ASM_NAME("host_exit_gate"))
__ASM_STDCALL_FUNC(ExitProcess,4,"movl 4(%esp),%eax\n\tjmp *" __ASM_NAME("host_exit_gate"))
