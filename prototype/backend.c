#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "../wine-11.0-source/dlls/wow64/wow64_private.h"
#include "wine/debug.h"
#include "shared.h"
#include "native_io.h"
#include "native_loader.h"
#include "../wine-11.0-source/dlls/ntdll/unixlib.h"
/* A guest thread can be terminated while entering a native wait. Diagnostic
 * output must not leave the native CRT's shared FILE lock owned by that
 * thread, blocking every surviving service that tries to log a result. */
static void diagnostic(const char *format,...)
{
    DWORD error=GetLastError(),written;char buffer[1024]={0};va_list arguments;
    va_start(arguments,format);
    int length=_vsnprintf(buffer,sizeof(buffer),format,arguments);
    va_end(arguments);
    if(length<0||length>=(int)sizeof(buffer))
    { length=0;while(length<(int)sizeof(buffer)&&buffer[length])++length; }
    WriteFile(GetStdHandle(STD_ERROR_HANDLE),buffer,length,&written,NULL);
    SetLastError(error);
}
USHORT native_machine=IMAGE_FILE_MACHINE_AMD64,current_machine=IMAGE_FILE_MACHINE_I386;
ULONG_PTR args_alignment=4,highest_user_address=0x7ffeffff,default_zero_bits=0x7fffffff;
void (WINAPI *pBTCpuUpdateProcessorInformation)(SYSTEM_CPU_INFORMATION *);
extern BOOLEAN WINAPI BTCpuIsProcessorFeaturePresent(UINT);
BOOLEAN (WINAPI *pBTCpuIsProcessorFeaturePresent)(UINT)=BTCpuIsProcessorFeaturePresent;
void (WINAPI *pBTCpuNotifyMemoryAlloc)(void *,SIZE_T,ULONG,ULONG,BOOL,NTSTATUS);
void (WINAPI *pBTCpuNotifyMemoryFree)(void *,SIZE_T,ULONG,BOOL,NTSTATUS);
void (WINAPI *pBTCpuNotifyReadFile)(HANDLE,void *,SIZE_T,BOOL,NTSTATUS);
void (WINAPI *pBTCpuFlushInstructionCache2)(const void *,SIZE_T);
NTSTATUS (WINAPI *pBTCpuNotifyMapViewOfSection)(void *,void *,void *,SIZE_T,ULONG,ULONG);
void (WINAPI *pBTCpuNotifyMemoryProtect)(void *,SIZE_T,ULONG,BOOL,NTSTATUS);
void (WINAPI *pBTCpuNotifyUnmapViewOfSection)(void *,BOOL,NTSTATUS);
void (WINAPI *pBTCpuThreadTerm)(HANDLE,LONG);
void (WINAPI *pBTCpuProcessTerm)(HANDLE,BOOL,NTSTATUS);
/* Windows TLS holds a temporary-allocation frame per active dispatch. This
 * replaces Wine's Unix host storage, while retaining Wine's actual thunks. */
struct temp_frame { void *blocks[256];unsigned count;UINT service; };
static DWORD temp_slot;
void probe_backend_init(void)
{ temp_slot=TlsAlloc();if(temp_slot==TLS_OUT_OF_INDEXES) ExitProcess(94); }
/* Wine's NLS pointer thunk is retained. Its host service must return tables
 * below 4 GiB, as Wine's Unix backend normally does for WoW64. */
NTSTATUS WINAPI probe_NtGetNlsSectionPtr(ULONG type,ULONG id,void *unknown,void **out,SIZE_T *out_size)
{
    static SRWLOCK lock=SRWLOCK_INIT;
    static struct { ULONG type,id;void *ptr;SIZE_T size; } cache[64];
    static unsigned count;
    NTSTATUS status;void *ptr=NULL;SIZE_T size=0;BOOL allocated=FALSE;
    AcquireSRWLockExclusive(&lock);
    for(unsigned i=0;i<count;i++) if(cache[i].type==type&&cache[i].id==id)
    { *out=cache[i].ptr;*out_size=cache[i].size;ReleaseSRWLockExclusive(&lock);return STATUS_SUCCESS; }
    const char *wine_path=type==10&&id==0?"stock-wine/l_intl.nls":type==9&&id==0?"stock-wine/sortdefault.nls":type==12&&id==1?"stock-wine/normnfc.nls":NULL;
    status=wine_path?STATUS_NOT_IMPLEMENTED:NtGetNlsSectionPtr(type,id,unknown,&ptr,&size);
    if(status<0&&wine_path)
    {
        WCHAR relative[MAX_PATH],path[32768];
        MultiByteToWideChar(CP_UTF8,0,wine_path,-1,relative,ARRAY_SIZE(relative));
        FILE *f=probe_runtime_path(path,ARRAY_SIZE(path),relative)?_wfopen(path,L"rb"):NULL;
        if(f) { fseek(f,0,SEEK_END);size=ftell(f);rewind(f);ptr=malloc(size);
                allocated=ptr!=NULL;if(ptr&&fread(ptr,1,size,f)==size) status=STATUS_SUCCESS;fclose(f); }
    }
    if(status>=0)
    {
        void *low=NULL;
        if(count==64||size>16*1024*1024) status=STATUS_NO_MEMORY;
        else
        {
            for(uintptr_t a=0x40000000;a<0x70000000;a+=0x100000)
                if((low=VirtualAlloc((void *)a,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE))) break;
            if(!low) status=STATUS_NO_MEMORY;
            else
            {
                memcpy(low,ptr,size);DWORD old;VirtualProtect(low,size,PAGE_READONLY,&old);
                cache[count].type=type;cache[count].id=id;cache[count].ptr=low;cache[count++].size=size;
                *out=low;*out_size=size;
            }
        }
    }
    if(allocated) free(ptr);
    ReleaseSRWLockExclusive(&lock);return status;
}
NTSTATUS (WINAPI *__imp_probe_NtGetNlsSectionPtr)(ULONG,ULONG,void *,void **,SIZE_T *)=probe_NtGetNlsSectionPtr;
NTSTATUS WINAPI probe_NtInitializeNlsFiles(void **out,LCID *lcid,LARGE_INTEGER *out_size)
{
    static SRWLOCK lock=SRWLOCK_INIT;static void *data;static SIZE_T size;
    NTSTATUS status=STATUS_SUCCESS;AcquireSRWLockExclusive(&lock);
    if(!data)
    {
        WCHAR path[32768];
        FILE *f=probe_runtime_path(path,ARRAY_SIZE(path),L"stock-wine/locale.nls")?_wfopen(path,L"rb"):NULL;
        if(!f) status=STATUS_NO_SUCH_FILE;
        else
        {
            fseek(f,0,SEEK_END);size=ftell(f);rewind(f);
            for(uintptr_t a=0x50000000;size<16*1024*1024&&a<0x70000000;a+=0x100000)
                if((data=VirtualAlloc((void *)a,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE)))break;
            if(!data) status=STATUS_NO_MEMORY;
            else if(fread(data,1,size,f)!=size) {VirtualFree(data,0,MEM_RELEASE);data=NULL;status=STATUS_UNSUCCESSFUL;}
            fclose(f);
        }
    }
    if(status>=0) { *out=data;*lcid=GetUserDefaultLCID();out_size->QuadPart=size; }
    ReleaseSRWLockExclusive(&lock);return status;
}
NTSTATUS (WINAPI *__imp_probe_NtInitializeNlsFiles)(void **,LCID *,LARGE_INTEGER *)=probe_NtInitializeNlsFiles;
/* Wine's synchronous LockFile path permits a NULL IO_STATUS_BLOCK. Windows
 * requires one even when failing immediately. Keep Wine's argument thunk
 * and supply storage only for this synchronous native-kernel difference. */
NTSTATUS WINAPI probe_NtLockFile(HANDLE file,HANDLE event,PIO_APC_ROUTINE apc,void *context,
                               IO_STATUS_BLOCK *io,LARGE_INTEGER *offset,LARGE_INTEGER *count,
                               ULONG *key,BOOLEAN dont_wait,BOOLEAN exclusive)
{
    IO_STATUS_BLOCK scratch={0};
    if(!io)
    {
        if(event||apc||!dont_wait)return STATUS_NOT_SUPPORTED;
        io=&scratch;
    }
    return NtLockFile(file,event,apc,context,io,offset,count,key,dont_wait,exclusive);
}
NTSTATUS (WINAPI *__imp_probe_NtLockFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,void *,IO_STATUS_BLOCK *,LARGE_INTEGER *,LARGE_INTEGER *,ULONG *,BOOLEAN,BOOLEAN)=probe_NtLockFile;
NTSTATUS WINAPI probe_NtOpenFile(HANDLE *file,ACCESS_MASK access,OBJECT_ATTRIBUTES *attributes,
                               IO_STATUS_BLOCK *io,ULONG share,ULONG options)
{
    if(!attributes)return NtOpenFile(file,access,attributes,io,share,options);
    OBJECT_ATTRIBUTES copy=*attributes;UNICODE_STRING name;
    if(copy.ObjectName)
    {
        name=*copy.ObjectName;
        WCHAR *buffer=Wow64AllocateTemp(name.Length+sizeof(WCHAR));
        memcpy(buffer,name.Buffer,name.Length);buffer[name.Length/sizeof(WCHAR)]=0;
        for(unsigned i=0;i<name.Length/sizeof(WCHAR);i++)if(buffer[i]==L'/')buffer[i]=L'\\';
        name.Buffer=buffer;copy.ObjectName=&name;
    }
    /* Wine API-set resolution uses its logical C:\windows\system32 path.
     * Route that namespace to Wine's DLL directory on a Windows host. */
    WCHAR mapped[2048],stock[1024];
    static const WCHAR *prefixes[]={L"\\??\\C:\\windows\\system32\\",L"\\??\\C:\\windows\\syswow64\\"};
    if(copy.ObjectName)
    for(unsigned i=0;i<2;i++)
    {
        size_t prefix=wcslen(prefixes[i]),length=name.Length/sizeof(WCHAR);
        if(length>prefix+4&&!wcsnicmp(name.Buffer,prefixes[i],prefix)&&!wcsnicmp(name.Buffer+length-4,L".dll",4))
        {
            if(length-prefix>900||!probe_runtime_path(stock,ARRAY_SIZE(stock),L"stock-wine"))return STATUS_NAME_TOO_LONG;
            swprintf(mapped,2048,L"\\??\\%s\\%s",stock,name.Buffer+prefix);
            name.Buffer=mapped;name.Length=wcslen(mapped)*sizeof(WCHAR);name.MaximumLength=name.Length+sizeof(WCHAR);
            break;
        }
    }
    /* Wine's Unix backend allows delete-on-close with SYNCHRONIZE alone. */
    if(options&FILE_DELETE_ON_CLOSE)access|=DELETE;
    return NtOpenFile(file,access,&copy,io,share,options);
}
NTSTATUS (WINAPI *__imp_probe_NtOpenFile)(HANDLE *,ACCESS_MASK,OBJECT_ATTRIBUTES *,IO_STATUS_BLOCK *,ULONG,ULONG)=probe_NtOpenFile;
NTSTATUS WINAPI probe_NtOpenDirectoryObject(HANDLE *directory,ACCESS_MASK access,OBJECT_ATTRIBUTES *attributes)
{
    NTSTATUS status=NtOpenDirectoryObject(directory,access,attributes);
    /* Wine's server puts session zero's named objects under Sessions too.
     * Windows uses the global BaseNamedObjects directory for session zero.
     * Preserve Wine's caller and relative Local/Global name resolution. */
    static const WCHAR wine_zero[]=L"\\Sessions\\0\\BaseNamedObjects";
    if((status==STATUS_OBJECT_NAME_NOT_FOUND||status==STATUS_OBJECT_PATH_NOT_FOUND)&&
       attributes&&!attributes->RootDirectory&&attributes->ObjectName&&
       attributes->ObjectName->Length==sizeof(wine_zero)-sizeof(WCHAR)&&
       !wcsnicmp(attributes->ObjectName->Buffer,wine_zero,ARRAY_SIZE(wine_zero)-1))
    {
        UNICODE_STRING name=RTL_CONSTANT_STRING(L"\\BaseNamedObjects");
        OBJECT_ATTRIBUTES copy=*attributes;copy.ObjectName=&name;
        status=NtOpenDirectoryObject(directory,access,&copy);
    }
    return status;
}
NTSTATUS (WINAPI *__imp_probe_NtOpenDirectoryObject)(HANDLE *,ACCESS_MASK,OBJECT_ATTRIBUTES *)=probe_NtOpenDirectoryObject;
NTSTATUS WINAPI probe_NtSetInformationFile(HANDLE file,IO_STATUS_BLOCK *io,void *info,ULONG size,FILE_INFORMATION_CLASS type)
{
    NTSTATUS adapted;
    if(probe_io_set_information(file,io,info,size,type,&adapted))return adapted;
    NTSTATUS status=NtSetInformationFile(file,io,info,size,type);
    /* Wine SetFileAttributes opens a metadata handle with SYNCHRONIZE only.
     * Reopen that same file with the access Windows requires for this write. */
    if(status==STATUS_ACCESS_DENIED&&type==FileBasicInformation)
    {
        HANDLE writable=ReOpenFile(file,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,FILE_FLAG_BACKUP_SEMANTICS);
        if(writable!=INVALID_HANDLE_VALUE)
        { status=NtSetInformationFile(writable,io,info,size,type);NtClose(writable); }
    }
    return status;
}
NTSTATUS (WINAPI *__imp_probe_NtSetInformationFile)(HANDLE,IO_STATUS_BLOCK *,void *,ULONG,FILE_INFORMATION_CLASS)=probe_NtSetInformationFile;
void *WINAPI Wow64AllocateTemp(SIZE_T size)
{
    struct temp_frame *frame=TlsGetValue(temp_slot);
    if(!frame||frame->count==256) { fputs("temporary allocation limit\n",stderr);ExitProcess(92); }
    void *p=malloc(size);
    if(!p) ExitProcess(93);
    frame->blocks[frame->count++]=p;
    return p;
}
static void *guest_bop_code;
void probe_set_bop_code(void *code) { guest_bop_code=code; }
static void *WINAPI probe_get_bop_code(void) { return guest_bop_code; }
void *(WINAPI *pBTCpuGetBopCode)(void)=probe_get_bop_code;
#include "out/wine-image-mapping.inc"
unsigned long probe_nt_calls,probe_library_calls;
int probe_trace;
extern NTSTATUS WINAPI wow64_NtClose(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateFile(UINT *);
extern NTSTATUS WINAPI wow64_NtDeleteFile(UINT *);
extern NTSTATUS WINAPI wow64_NtWaitForSingleObject(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryInformationThread(UINT *);
extern NTSTATUS WINAPI wow64_NtAlertThreadByThreadId(UINT *),wow64_NtWaitForAlertByThreadId(UINT *);
extern NTSTATUS WINAPI wow64_NtAlertMultipleThreadByThreadId(UINT *);
extern NTSTATUS WINAPI wow64_NtSignalAndWaitForSingleObject(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryTimerResolution(UINT *),wow64_NtSetTimerResolution(UINT *);
extern NTSTATUS WINAPI wow64_NtReadFile(UINT *),wow64_NtOpenFile(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryInformationFile(UINT *),wow64_NtSetInformationFile(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryVolumeInformationFile(UINT *);
extern NTSTATUS WINAPI wow64_NtGetNlsSectionPtr(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryInformationProcess(UINT *);
extern NTSTATUS WINAPI wow64_NtInitializeNlsFiles(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryDefaultLocale(UINT *),wow64_NtQueryDefaultUILanguage(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateKey(UINT *),wow64_NtOpenKey(UINT *),wow64_NtOpenKeyEx(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryKey(UINT *),wow64_NtQueryValueKey(UINT *),wow64_NtSetValueKey(UINT *);
extern NTSTATUS WINAPI wow64_NtEnumerateKey(UINT *),wow64_NtEnumerateValueKey(UINT *);
extern NTSTATUS WINAPI wow64_NtQuerySystemInformation(UINT *),wow64_NtQuerySystemTime(UINT *),wow64_NtQueryInformationToken(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryAttributesFile(UINT *);
extern NTSTATUS WINAPI wow64_NtWow64IsProcessorFeaturePresent(UINT *);
extern NTSTATUS WINAPI wow64_NtFsControlFile(UINT *),wow64_NtCreateSection(UINT *),wow64_NtQuerySection(UINT *);
extern NTSTATUS WINAPI wow64_NtMapViewOfSection(UINT *),wow64_NtUnmapViewOfSection(UINT *);
extern NTSTATUS WINAPI wow64_NtProtectVirtualMemory(UINT *),wow64_NtQueryVirtualMemory(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryDirectoryFile(UINT *),wow64_NtSetInformationProcess(UINT *);
extern NTSTATUS WINAPI wow64_NtLockFile(UINT *),wow64_NtUnlockFile(UINT *),wow64_NtFlushBuffersFile(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryFullAttributesFile(UINT *);
extern NTSTATUS WINAPI wow64_NtContinue(UINT *),wow64_NtContinueEx(UINT *),wow64_NtRaiseException(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateEvent(UINT *),wow64_NtSetEvent(UINT *),wow64_NtResetEvent(UINT *),wow64_NtCancelIoFile(UINT *);
extern NTSTATUS WINAPI wow64_NtCancelIoFileEx(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateNamedPipeFile(UINT *);
extern NTSTATUS WINAPI wow64_NtWaitForMultipleObjects(UINT *);
extern NTSTATUS WINAPI wow64_NtDuplicateObject(UINT *);
extern NTSTATUS WINAPI wow64_NtQueueApcThread(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateIoCompletion(UINT *),wow64_NtRemoveIoCompletion(UINT *),wow64_NtRemoveIoCompletionEx(UINT *);
extern NTSTATUS WINAPI wow64_NtSetIoCompletion(UINT *),wow64_NtSetIoCompletionEx(UINT *);
extern NTSTATUS WINAPI wow64_NtDeviceIoControlFile(UINT *);
extern NTSTATUS WINAPI wow64_NtGetNextThread(UINT *),wow64_NtSetInformationThread(UINT *);
extern NTSTATUS WINAPI wow64_NtGetContextThread(UINT *),wow64_NtSetContextThread(UINT *);
extern NTSTATUS WINAPI wow64_NtSuspendThread(UINT *),wow64_NtResumeThread(UINT *),wow64_NtOpenThread(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateThreadEx(UINT *),wow64_NtTerminateThread(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateUserProcess(UINT *);
extern NTSTATUS WINAPI wow64_NtQueryObject(UINT *),wow64_NtSetInformationObject(UINT *);
extern NTSTATUS WINAPI wow64_NtTestAlert(UINT *);
extern NTSTATUS WINAPI wow64_NtOpenDirectoryObject(UINT *),wow64_NtCreateDirectoryObject(UINT *),wow64_NtQueryDirectoryObject(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateMutant(UINT *),wow64_NtOpenMutant(UINT *),wow64_NtReleaseMutant(UINT *),wow64_NtQueryMutant(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateSemaphore(UINT *),wow64_NtOpenSemaphore(UINT *),wow64_NtReleaseSemaphore(UINT *),wow64_NtQuerySemaphore(UINT *);
extern NTSTATUS WINAPI wow64_NtOpenEvent(UINT *),wow64_NtQueryEvent(UINT *),wow64_NtPulseEvent(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateTimer(UINT *),wow64_NtOpenTimer(UINT *),wow64_NtSetTimer(UINT *),wow64_NtCancelTimer(UINT *),wow64_NtQueryTimer(UINT *);
extern NTSTATUS WINAPI wow64_NtOpenProcess(UINT *),wow64_NtTerminateProcess(UINT *);
extern NTSTATUS WINAPI wow64_NtReadVirtualMemory(UINT *),wow64_NtWriteVirtualMemory(UINT *),wow64_NtAllocateVirtualMemoryEx(UINT *);
extern NTSTATUS WINAPI wow64_NtDeleteKey(UINT *),wow64_NtDeleteValueKey(UINT *);
extern NTSTATUS WINAPI wow64_NtQuerySystemInformationEx(UINT *),wow64_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter(UINT *);
extern NTSTATUS WINAPI wow64_NtCreateKeyedEvent(UINT *);
extern NTSTATUS WINAPI wow64_NtOpenKeyedEvent(UINT *);
extern NTSTATUS WINAPI wow64_NtWaitForKeyedEvent(UINT *);
extern NTSTATUS WINAPI wow64_NtReleaseKeyedEvent(UINT *);
extern NTSTATUS WINAPI wow64_NtSetEventBoostPriority(UINT *);
extern NTSTATUS WINAPI wow64_NtAlertThread(UINT *);
static NTSTATUS WINAPI dispatch_nt(UINT *a)
{
    NTSTATUS status;
    UINT n=((struct temp_frame *)TlsGetValue(temp_slot))->service;
    if(probe_trace) diagnostic("NT_CALL %x\n",n);
    if(n==0x00d0)
    {
        diagnostic("HARD_ERROR status=%08x parameters=%u unicode_mask=%x option=%u\n",a[0],a[1],a[2],a[4]);
        if(a[1]<=4&&a[3])for(UINT i=0;i<a[1];i++)
        {
            ULONG value=((ULONG *)(ULONG_PTR)a[3])[i];
            if(a[2]&(1u<<i))
            {
                const UNICODE_STRING32 *str=(void *)(ULONG_PTR)value;
                diagnostic("HARD_ERROR parameter[%u]=%.*ls\n",i,str->Length/2,(WCHAR *)(ULONG_PTR)str->Buffer);
            }
            else diagnostic("HARD_ERROR parameter[%u]=%08lx\n",i,value);
        }
    }
    if(probe_trace&&(n==0x33||n==0x58||n==0x4a))
    {
        OBJECT_ATTRIBUTES32 *oa=(void *)(uintptr_t)a[2];UNICODE_STRING32 *name=oa?(void *)(uintptr_t)oa->ObjectName:NULL;
        if(name)diagnostic("OBJECT_NAME service=%x access=%08x root=%08lx path=%.*ls\n",n,a[1],oa->RootDirectory,name->Length/2,(WCHAR *)(uintptr_t)name->Buffer);
    }
    if(probe_trace&&n==0x27)diagnostic("SET_FILE handle=%x class=%u len=%u data=%08lx\n",a[0],a[4],a[3],*(DWORD *)(uintptr_t)a[2]);
    switch(n)
    {
    case 0x0089: status=wow64_NtCreateUserProcess(a);break;
    case 0x007b: status=wow64_NtCreateKeyedEvent(a);break;
    case 0x00b3: status=wow64_NtOpenKeyedEvent(a);break;
    case 0x0101: status=wow64_NtWaitForKeyedEvent(a);break;
    case 0x00d2: status=wow64_NtReleaseKeyedEvent(a);break;
    case 0x002d: status=wow64_NtSetEventBoostPriority(a);break;
    case 0x0066: status=wow64_NtAlertThread(a);break;
    case 0x0058: status=wow64_NtOpenDirectoryObject(a);break;
    case 0x0077: status=wow64_NtCreateDirectoryObject(a);break;
    case 0x00bc: status=wow64_NtQueryDirectoryObject(a);break;
    case 0x007e: status=wow64_NtCreateMutant(a);break;
    case 0x00b4: status=wow64_NtOpenMutant(a);break;
    case 0x0020: status=wow64_NtReleaseMutant(a);break;
    case 0x00c5: status=wow64_NtQueryMutant(a);break;
    case 0x0083: status=wow64_NtCreateSemaphore(a);break;
    case 0x00b6: status=wow64_NtOpenSemaphore(a);break;
    case 0x000a: status=wow64_NtReleaseSemaphore(a);break;
    case 0x00c7: status=wow64_NtQuerySemaphore(a);break;
    case 0x0040: status=wow64_NtOpenEvent(a);break;
    case 0x0056: status=wow64_NtQueryEvent(a);break;
    case 0x00bb: status=wow64_NtPulseEvent(a);break;
    case 0x0086: status=wow64_NtCreateTimer(a);break;
    case 0x00b9: status=wow64_NtOpenTimer(a);break;
    case 0x0062: status=wow64_NtSetTimer(a);break;
    case 0x0061: status=wow64_NtCancelTimer(a);break;
    case 0x0038: status=wow64_NtQueryTimer(a);break;
    case 0x0026: status=wow64_NtOpenProcess(a);break;
    case 0x002c: status=wow64_NtTerminateProcess(a);break;
    case 0x003f: status=wow64_NtReadVirtualMemory(a);break;
    case 0x003a: status=wow64_NtWriteVirtualMemory(a);break;
    case 0x006b: status=wow64_NtAllocateVirtualMemoryEx(a);break;
    case 0x008e: status=wow64_NtDeleteKey(a);break;
    case 0x008f: status=wow64_NtDeleteValueKey(a);break;
    case 0x00cb: status=wow64_NtQuerySystemInformationEx(a);break;
    case 0x0076: status=wow64_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter(a);break;
    case 0x00f8: status=wow64_NtTestAlert(a);break;
    case 0x0010: status=wow64_NtQueryObject(a);break;
    case 0x005c: status=wow64_NtSetInformationObject(a);break;
    case 0x0085: status=wow64_NtCreateThreadEx(a);break;
    case 0x0053: status=wow64_NtTerminateThread(a);break;
    case 0x0097: status=wow64_NtGetContextThread(a);break;
    case 0x00de: status=wow64_NtSetContextThread(a);break;
    case 0x00f5: status=wow64_NtSuspendThread(a);break;
    case 0x0052: status=wow64_NtResumeThread(a);break;
    case 0x00b8: status=wow64_NtOpenThread(a);break;
    case 0x009a: status=wow64_NtGetNextThread(a);break;
    case 0x000d: status=wow64_NtSetInformationThread(a);break;
    case 0x0007: status=wow64_NtDeviceIoControlFile(a);break;
    case 0x0078: status=wow64_NtCreateIoCompletion(a);break;
    case 0x0009: status=wow64_NtRemoveIoCompletion(a);break;
    case 0x00d3: status=wow64_NtRemoveIoCompletionEx(a);break;
    case 0x00e9: status=wow64_NtSetIoCompletion(a);break;
    case 0x00ea: status=wow64_NtSetIoCompletionEx(a);break;
    case 0x0048: status=wow64_NtCreateEvent(a);break;
    case 0x000e: status=wow64_NtSetEvent(a);break;
    case 0x00d7: status=wow64_NtResetEvent(a);break;
    case 0x005d: status=wow64_NtCancelIoFile(a);break;
    case 0x006e: status=wow64_NtCancelIoFileEx(a);break;
    case 0x007f: status=wow64_NtCreateNamedPipeFile(a);break;
    case 0x005b: status=wow64_NtWaitForMultipleObjects(a);break;
    case 0x003c: status=wow64_NtDuplicateObject(a);break;
    case 0x0045: status=wow64_NtQueueApcThread(a);break;
    case 0x0067: status=wow64_NtAlertThreadByThreadId(a);break;
    case 0x0064: status=wow64_NtAlertMultipleThreadByThreadId(a);break;
    case 0x00ff: status=wow64_NtWaitForAlertByThreadId(a);break;
    case 0x00f3: status=wow64_NtSignalAndWaitForSingleObject(a);break;
    case 0x00cc: status=wow64_NtQueryTimerResolution(a);break;
    case 0x00f0: status=wow64_NtSetTimerResolution(a);break;
    case 0x0043: status=wow64_NtContinue(a);break;
    case 0x0075: status=wow64_NtContinueEx(a);break;
    case 0x00cf: status=wow64_NtRaiseException(a);break;
    case 0x00a5: status=wow64_NtLockFile(a);break;
    case 0x00fc: status=wow64_NtUnlockFile(a);break;
    case 0x004b: status=wow64_NtFlushBuffersFile(a);break;
    case 0x00be: status=wow64_NtQueryFullAttributesFile(a);break;
    case 0x0035: status=wow64_NtQueryDirectoryFile(a);break;
    case 0x001c: status=wow64_NtSetInformationProcess(a);break;
    case 0x0039: status=wow64_NtFsControlFile(a);break;
    case 0x004a: status=wow64_NtCreateSection(a);break;
    case 0x0051: status=wow64_NtQuerySection(a);break;
    case 0x0028: status=wow64_NtMapViewOfSection(a);break;
    case 0x002a: status=wow64_NtUnmapViewOfSection(a);break;
    case 0x0050: status=wow64_NtProtectVirtualMemory(a);break;
    case 0x0023: status=wow64_NtQueryVirtualMemory(a);break;
    case 0x003d: status=wow64_NtQueryAttributesFile(a);break;
    case 0x0104: status=wow64_NtWow64IsProcessorFeaturePresent(a);break;
    case 0x0021: status=wow64_NtQueryInformationToken(a);break;
    case 0x0036: status=wow64_NtQuerySystemInformation(a);break;
    case 0x005a: status=wow64_NtQuerySystemTime(a);break;
    case 0x0012: status=wow64_NtOpenKey(a);break;
    case 0x0013: status=wow64_NtEnumerateValueKey(a);break;
    case 0x0015: status=wow64_NtQueryDefaultLocale(a);break;
    case 0x0016: status=wow64_NtQueryKey(a);break;
    case 0x0017: status=wow64_NtQueryValueKey(a);break;
    case 0x001d: status=wow64_NtCreateKey(a);break;
    case 0x0032: status=wow64_NtEnumerateKey(a);break;
    case 0x0044: status=wow64_NtQueryDefaultUILanguage(a);break;
    case 0x0060: status=wow64_NtSetValueKey(a);break;
    case 0x00b0: status=wow64_NtOpenKeyEx(a);break;
    case 0x009b: status=wow64_NtGetNlsSectionPtr(a);break;
    case 0x0031: status=wow64_NtQueryPerformanceCounter(a);break;
    case 0x0019: status=wow64_NtQueryInformationProcess(a);break;
    case 0x009e: status=wow64_NtInitializeNlsFiles(a);break;
    case 0x0006: status=wow64_NtReadFile(a);break;
    case 0x0011: status=wow64_NtQueryInformationFile(a);break;
    case 0x0027: status=wow64_NtSetInformationFile(a);break;
    case 0x0033: status=wow64_NtOpenFile(a);break;
    case 0x0049: status=wow64_NtQueryVolumeInformationFile(a);break;
    case NR_WAIT_FOR_SINGLE_OBJECT: status=wow64_NtWaitForSingleObject(a); break;
    case NR_WRITE_FILE: status=wow64_NtWriteFile(a); break;
    case NR_CLOSE: status=wow64_NtClose(a); break;
    case NR_ALLOCATE_VM: status=wow64_NtAllocateVirtualMemory(a); break;
    case NR_QUERY_INFORMATION_THREAD: status=wow64_NtQueryInformationThread(a); break;
    case NR_FREE_VM: status=wow64_NtFreeVirtualMemory(a); break;
    case NR_DELAY: status=wow64_NtDelayExecution(a); break;
    case NR_CREATE_FILE: status=wow64_NtCreateFile(a); break;
    case NR_DELETE_FILE: status=wow64_NtDeleteFile(a); break;
    default: diagnostic("UNIMPLEMENTED NT service %x\n",n);status=STATUS_NOT_IMPLEMENTED;break;
    }
    return status;
}
extern NTSTATUS wow64_syscall(UINT *,ULONG_PTR);
NTSTATUS WINAPI Wow64SystemServiceEx(UINT n,UINT *a)
{
    struct temp_frame frame={.service=n},*previous=TlsGetValue(temp_slot);
    TlsSetValue(temp_slot,&frame);
    InterlockedIncrement((LONG *)&probe_nt_calls);
    /* Wine's original SEH wrapper converts faults while marshalling guest
     * arguments into status returns before temporary storage is released. */
    NTSTATUS status=wow64_syscall(a,(ULONG_PTR)dispatch_nt);
    while(frame.count) free(frame.blocks[--frame.count]);
    TlsSetValue(temp_slot,previous);
    if(probe_trace) diagnostic("NT_RETURN %x status=%08lx\n",n,(unsigned long)status);
    if(status<0) diagnostic("NT service %x returned %08lx\n",n,(unsigned long)status);
    /* Reload SS before Wine jumps back to compatibility mode. This fixed
     * repeatable guest RET faults after native calls on the tested Windows VM. */
    __asm__ __volatile__("movw %%ss,%%ax; movw %%ax,%%ss":::"ax","memory");
    return status;
}
typedef NTSTATUS (WINAPI *entry_t)(void *);
extern BOOL probe_socket_unix_dispatch(ULONGLONG,ULONG,void *,NTSTATUS *);
static uint64_t private_bridge_table;
void probe_set_bridge_table(void *table) { private_bridge_table=(uintptr_t)table; }
NTSTATUS WINAPI probe_library_dispatch(uint64_t handle,unsigned int n,void *a)
{
    NTSTATUS status;
    InterlockedIncrement((LONG *)&probe_library_calls);
    if(handle==BRIDGE_NTDLL_UNIX_HANDLE)
    {
        status=STATUS_NOT_SUPPORTED;
        if(n==unix_wine_dbg_write)
        {
            /* Wine's x86 unix-call record contains a 32-bit string pointer. */
            const UINT *args=a;
            struct wine_dbg_write_params params={ULongToPtr(args[0]),args[1]};
            DWORD written=0;
            if(WriteFile(GetStdHandle(STD_ERROR_HANDLE),params.str,params.len,&written,NULL))status=written;
            else status=STATUS_UNSUCCESSFUL;
        }
        goto done;
    }
    if(probe_socket_unix_dispatch(handle,n,a,&status))goto done;
    if(handle==private_bridge_table&&n>=BRIDGE_COUNT)return STATUS_INVALID_PARAMETER;
    status=((entry_t *)(uintptr_t)handle)[n](a);
done:
    __asm__ __volatile__("movw %%ss,%%ax; movw %%ax,%%ss":::"ax","memory");
    return status;
}
unsigned char __cdecl __wine_dbg_get_channel_flags(struct __wine_debug_channel *ch) { (void)ch; return 0; }
int __cdecl __wine_dbg_header(enum __wine_debug_class c,struct __wine_debug_channel *ch,const char *f)
{ (void)c;(void)ch;(void)f; return -1; }
int __cdecl __wine_dbg_output(const char *s) { return fputs(s,stderr); }
