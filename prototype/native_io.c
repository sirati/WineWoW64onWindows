/* Windows request lifetime/notification adapter around Wine's original I/O thunks.
 * Status conversion and release publication come from Wine, not a second
 * implementation. Native kernel completions are held until that publication. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>
#include <stdio.h>
#include "../wine-11.0-source/dlls/wow64/wow64_private.h"
#include "native_io.h"
typedef ULONGLONG client_ptr_t;
static BOOL in_wow64_call(void) { return TRUE; }
static void *wine_server_get_ptr(client_ptr_t p) { return (void *)(ULONG_PTR)p; }
#include "out/wine-async-iosb.inc"

/* Do not retain a duplicate file handle here. Windows must see last-close
 * and issuing-thread exit so that it cancels outstanding native requests.
 * Aliases track the same file object; requests keep only this metadata alive. */
struct file_state {
    LONG refs;
    HANDLE event,port;
    ULONG_PTR key;
    ULONG flags;
};
struct file_alias { LIST_ENTRY entry;HANDLE handle;struct file_state *file; };
static SRWLOCK files_lock=SRWLOCK_INIT;
static LIST_ENTRY aliases={&aliases,&aliases};
static BOOL same_object(HANDLE a,HANDLE b)
{
    typedef NTSTATUS (WINAPI *compare_fn)(HANDLE,HANDLE);
    compare_fn compare=(compare_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCompareObjects");
    return a==b||(compare&&compare(a,b)==STATUS_SUCCESS);
}
static void release_file(struct file_state *file)
{
    if(!InterlockedDecrement(&file->refs))
    { CloseHandle(file->event);if(file->port)CloseHandle(file->port);HeapFree(GetProcessHeap(),0,file); }
}
static struct file_state *find_file_locked(HANDLE handle)
{
    for(LIST_ENTRY *e=aliases.Flink;e!=&aliases;e=e->Flink)
    {
        struct file_alias *alias=CONTAINING_RECORD(e,struct file_alias,entry);
        if(same_object(alias->handle,handle))return alias->file;
    }
    return NULL;
}
static BOOL add_alias_locked(HANDLE handle,struct file_state *file)
{
    for(LIST_ENTRY *e=aliases.Flink;e!=&aliases;e=e->Flink)
        if(CONTAINING_RECORD(e,struct file_alias,entry)->handle==handle)return TRUE;
    struct file_alias *alias=HeapAlloc(GetProcessHeap(),0,sizeof(*alias));
    if(!alias)return FALSE;
    alias->handle=handle;alias->file=file;InterlockedIncrement(&file->refs);
    InsertTailList(&aliases,&alias->entry);return TRUE;
}
static void remove_alias_locked(HANDLE handle)
{
    for(LIST_ENTRY *e=aliases.Flink;e!=&aliases;e=e->Flink)
    {
        struct file_alias *alias=CONTAINING_RECORD(e,struct file_alias,entry);
        if(alias->handle!=handle)continue;
        RemoveEntryList(e);release_file(alias->file);HeapFree(GetProcessHeap(),0,alias);break;
    }
}
static struct file_state *get_file(HANDLE handle,BOOL create)
{
    struct file_state *file;
    AcquireSRWLockExclusive(&files_lock);
    file=find_file_locked(handle);
    if(!file&&create)
    {
        IO_STATUS_BLOCK io;FILE_MODE_INFORMATION mode;
        if(!NtQueryInformationFile(handle,&io,&mode,sizeof(mode),FileModeInformation))
        {
            file=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*file));
            if(file)
            {
                file->refs=1;file->event=CreateEventW(NULL,TRUE,TRUE,NULL);
                if(!file->event){HeapFree(GetProcessHeap(),0,file);file=NULL;}
            }
        }
    }
    else if(file)InterlockedIncrement(&file->refs);
    if(file&&!add_alias_locked(handle,file)){release_file(file);file=NULL;}
    ReleaseSRWLockExclusive(&files_lock);
    return file;
}
BOOL probe_io_set_information(HANDLE handle,IO_STATUS_BLOCK *io,void *info,ULONG size,
                              FILE_INFORMATION_CLASS type,NTSTATUS *status)
{
    if(type!=FileCompletionInformation&&type!=FileReplaceCompletionInformation&&
       type!=FileIoCompletionNotificationInformation)return FALSE;
    struct file_state *file=get_file(handle,TRUE);
    if(!file){*status=STATUS_INVALID_HANDLE;return TRUE;}
    *status=STATUS_SUCCESS;
    HANDLE port=NULL,old=NULL;
    if(type==FileIoCompletionNotificationInformation)
    {
        if(size<sizeof(FILE_IO_COMPLETION_NOTIFICATION_INFORMATION))*status=STATUS_INFO_LENGTH_MISMATCH;
        else if(*(ULONG *)info&~7u)*status=STATUS_INVALID_PARAMETER;
        else
        {
            AcquireSRWLockExclusive(&files_lock);file->flags|=*(ULONG *)info;
            ReleaseSRWLockExclusive(&files_lock);
        }
    }
    else if(size<sizeof(FILE_COMPLETION_INFORMATION))*status=STATUS_INFO_LENGTH_MISMATCH;
    else
    {
        FILE_COMPLETION_INFORMATION *completion=info;
        if(completion->CompletionPort)
        {
            LONG depth;
            *status=NtQueryIoCompletion(completion->CompletionPort,IoCompletionBasicInformation,&depth,sizeof(depth),NULL);
            if(!*status)*status=NtDuplicateObject(GetCurrentProcess(),completion->CompletionPort,GetCurrentProcess(),&port,0,0,DUPLICATE_SAME_ACCESS);
        }
        if(!*status)
        {
            AcquireSRWLockExclusive(&files_lock);
            if(type==FileCompletionInformation&&file->port)*status=STATUS_INVALID_PARAMETER;
            else { old=file->port;file->port=port;file->key=completion->CompletionKey;port=NULL; }
            ReleaseSRWLockExclusive(&files_lock);
        }
    }
    if(old)CloseHandle(old);if(port)CloseHandle(port);
    if(!*status){io->Status=STATUS_SUCCESS;io->Information=0;}
    release_file(file);return TRUE;
}

/* A native file event may signal before the 64-to-32 status conversion.
 * Wait on our notification event instead, retaining it across handle close. */
static NTSTATUS duplicate_wait_handle(HANDLE handle,HANDLE *wait)
{
    struct file_state *file=get_file(handle,FALSE);
    if(!file){*wait=handle;return STATUS_SUCCESS;}
    NTSTATUS status=NtDuplicateObject(GetCurrentProcess(),file->event,GetCurrentProcess(),wait,0,0,DUPLICATE_SAME_ACCESS);
    release_file(file);return status;
}
NTSTATUS WINAPI probe_NtWaitForSingleObject(HANDLE handle,BOOLEAN alertable,const LARGE_INTEGER *timeout)
{
    HANDLE wait;NTSTATUS status=duplicate_wait_handle(handle,&wait);
    if(status)return status;
    status=NtWaitForSingleObject(wait,alertable,timeout);
    if(wait!=handle)NtClose(wait);return status;
}
NTSTATUS WINAPI probe_NtWaitForMultipleObjects(ULONG count,const HANDLE *handles,WAIT_TYPE type,BOOLEAN alertable,const LARGE_INTEGER *timeout)
{
    HANDLE waits[MAXIMUM_WAIT_OBJECTS];ULONG i;NTSTATUS status=STATUS_SUCCESS;
    if(!count||count>MAXIMUM_WAIT_OBJECTS)return STATUS_INVALID_PARAMETER;
    for(i=0;i<count;i++)if((status=duplicate_wait_handle(handles[i],&waits[i])))break;
    if(!status)status=NtWaitForMultipleObjects(count,waits,type,alertable,timeout);
    while(i--){if(waits[i]!=handles[i])NtClose(waits[i]);}
    return status;
}
NTSTATUS WINAPI probe_NtClose(HANDLE handle)
{
    extern BOOL probe_socket_close(HANDLE,NTSTATUS *);
    AcquireSRWLockExclusive(&files_lock);
    NTSTATUS status;
    if(!probe_socket_close(handle,&status))status=NtClose(handle);
    if(!status)remove_alias_locked(handle);
    ReleaseSRWLockExclusive(&files_lock);return status;
}
NTSTATUS WINAPI probe_NtDuplicateObject(HANDLE source_process,HANDLE source,HANDLE target_process,HANDLE *target,
                                       ACCESS_MASK access,ULONG attributes,ULONG options)
{
    BOOL local_source=source_process==GetCurrentProcess()||GetProcessId(source_process)==GetCurrentProcessId();
    BOOL local_target=target_process==GetCurrentProcess()||GetProcessId(target_process)==GetCurrentProcessId();
    struct file_state *file=local_source?get_file(source,TRUE):NULL;
    AcquireSRWLockExclusive(&files_lock);
    NTSTATUS status=NtDuplicateObject(source_process,source,target_process,target,access,attributes,options);
    if(!status&&local_target&&file&&target&&!add_alias_locked(*target,file))
    { NtClose(*target);*target=NULL;status=STATUS_NO_MEMORY; }
    if(local_source&&(options&DUPLICATE_CLOSE_SOURCE))remove_alias_locked(source);
    ReleaseSRWLockExclusive(&files_lock);
    if(file)release_file(file);return status;
}

struct native_io {
    LIST_ENTRY entry;
    LONG refs;
    union { IO_STATUS_BLOCK io;OVERLAPPED overlapped; };
    IO_STATUS_BLOCK32 *guest;
    HANDLE event,guest_event,thread,port;
    struct file_state *file;
    PIO_APC_ROUTINE apc;
    void *context;
    ULONG_PTR key;
    ULONG flags;
    BOOL suppress_port;
    BOOL completing;
    DWORD issuer;
    void *opaque;
    void (*complete)(void *,IO_STATUS_BLOCK *,void *);
    PTP_WAIT wait;
};
static SRWLOCK requests_lock=SRWLOCK_INIT;
static LIST_ENTRY requests={&requests,&requests};
static CONDITION_VARIABLE requests_changed=CONDITION_VARIABLE_INIT;
static void destroy_io(struct native_io *op)
{
    if(op->complete)op->complete(op->opaque,NULL,NULL);
    if(op->wait)CloseThreadpoolWait(op->wait);
    if(op->event)CloseHandle(op->event);
    if(op->guest_event)CloseHandle(op->guest_event);
    if(op->thread)CloseHandle(op->thread);
    if(op->port)CloseHandle(op->port);
    if(op->file)release_file(op->file);
    HeapFree(GetProcessHeap(),0,op);
}
static void release_io(struct native_io *op)
{
    if(!InterlockedDecrement(&op->refs))destroy_io(op);
}
static void finish_io(struct native_io *op,BOOL signal,BOOL pending)
{
    /* Notification may let the application reuse the same OVERLAPPED before
     * this callback leaves the list. Cancellation must select the new native
     * request, while thread teardown still waits for this publication. */
    AcquireSRWLockExclusive(&requests_lock);op->completing=TRUE;ReleaseSRWLockExclusive(&requests_lock);
    if(op->complete){op->complete(op->opaque,&op->io,op->guest);op->complete=NULL;}
    /* Windows may leave the block untouched on an immediate error. A
     * signaled completion must always publish, even if the numeric status
     * happens to equal the guest block's address. */
    if(signal||op->io.Pointer!=op->guest)
        set_async_iosb((ULONG_PTR)op->guest,op->io.Status,op->io.Information);
    if(signal)
    {
        if(op->guest_event)SetEvent(op->guest_event);
        if(!(op->flags&FILE_SKIP_SET_EVENT_ON_HANDLE))SetEvent(op->file->event);
        if(op->apc)NtQueueApcThread(op->thread,(PNTAPCFUNC)op->apc,(ULONG_PTR)op->context,(ULONG_PTR)op->guest,0);
        else if(op->port&&op->context&&!op->suppress_port&&
                (pending||!(op->flags&FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)))
        {
            extern int probe_trace;
            NTSTATUS status=NtSetIoCompletion(op->port,op->key,(ULONG_PTR)op->context,op->io.Status,op->io.Information);
            if(probe_trace)fprintf(stderr,"IO_PORT key=%llx context=%p iosb=%p status=%08lx bytes=%llu post=%08lx\n",
                                    (ULONGLONG)op->key,op->context,op->guest,op->io.Status,(ULONGLONG)op->io.Information,status);
        }
    }
    AcquireSRWLockExclusive(&requests_lock);RemoveEntryList(&op->entry);
    WakeAllConditionVariable(&requests_changed);ReleaseSRWLockExclusive(&requests_lock);
    release_io(op);
}
static void CALLBACK complete_io(PTP_CALLBACK_INSTANCE instance,void *context,PTP_WAIT wait,TP_WAIT_RESULT result)
{
    (void)instance;(void)wait;(void)result;
    finish_io(context,TRUE,TRUE);
}
static NTSTATUS native_read(HANDLE f,HANDLE e,void *c,IO_STATUS_BLOCK *io,const struct probe_io_args *args)
{ return NtReadFile(f,e,NULL,c,io,args->buffer,args->size,args->offset,args->key); }
static NTSTATUS native_write(HANDLE f,HANDLE e,void *c,IO_STATUS_BLOCK *io,const struct probe_io_args *args)
{ return NtWriteFile(f,e,NULL,c,io,args->buffer,args->size,args->offset,args->key); }
static NTSTATUS native_fs_control(HANDLE f,HANDLE e,void *c,IO_STATUS_BLOCK *io,const struct probe_io_args *args)
{ return NtFsControlFile(f,e,NULL,c,io,args->code,args->buffer,args->size,args->output,args->output_size); }
NTSTATUS probe_submit_io(probe_io_submit call,HANDLE file,HANDLE event,PIO_APC_ROUTINE apc,void *context,
                          IO_STATUS_BLOCK *io,const struct probe_io_args *args,BOOL synchronous_control)
{
    FILE_MODE_INFORMATION mode;
    IO_STATUS_BLOCK query;
    NTSTATUS status;
    if(!event&&!apc&&!args->complete)
    {
        status=NtQueryInformationFile(file,&query,&mode,sizeof(mode),FileModeInformation);
        if(status)return status;
        if(mode.Mode&(FILE_SYNCHRONOUS_IO_ALERT|FILE_SYNCHRONOUS_IO_NONALERT))
            return call(file,NULL,context,io,args);
    }
    if(!io||!io->Pointer)
    { if(args->complete)args->complete(args->opaque,NULL,NULL);return STATUS_INVALID_PARAMETER; }
    struct native_io *op=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*op));
    if(!op){if(args->complete)args->complete(args->opaque,NULL,NULL);return STATUS_NO_MEMORY;}
    op->complete=args->complete;op->opaque=args->opaque;
    op->issuer=GetCurrentThreadId();
    op->guest=io->Pointer;op->io.Pointer=op->guest;
    op->file=get_file(file,TRUE);op->apc=apc;op->context=context;
    op->suppress_port=((ULONG_PTR)event&1)!=0;
    if(!op->file){destroy_io(op);return STATUS_INVALID_HANDLE;}
    AcquireSRWLockShared(&files_lock);
    op->flags=op->file->flags;op->key=op->file->key;
    if(op->file->port)status=NtDuplicateObject(GetCurrentProcess(),op->file->port,GetCurrentProcess(),&op->port,0,0,DUPLICATE_SAME_ACCESS);
    else status=STATUS_SUCCESS;
    ReleaseSRWLockShared(&files_lock);
    if(status){destroy_io(op);return status;}
    if(apc&&!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&op->thread,THREAD_SET_CONTEXT,FALSE,0))
    { destroy_io(op);return STATUS_NO_MEMORY; }
    if(event)
    {
        HANDLE supplied=(HANDLE)((ULONG_PTR)event&~(ULONG_PTR)1);
        if(!DuplicateHandle(GetCurrentProcess(),supplied,GetCurrentProcess(),&op->guest_event,
                            EVENT_MODIFY_STATE,FALSE,0))
        { destroy_io(op);return STATUS_INVALID_HANDLE; }
    }
    if(!(op->event=CreateEventW(NULL,TRUE,FALSE,NULL))||
       !(op->wait=CreateThreadpoolWait(complete_io,op,NULL)))
    { destroy_io(op);return STATUS_NO_MEMORY; }
    if(event&&!ResetEvent(op->guest_event)) { destroy_io(op);return STATUS_INVALID_HANDLE; }
    if(!(op->flags&FILE_SKIP_SET_EVENT_ON_HANDLE))ResetEvent(op->file->event);
    op->refs=1;
    AcquireSRWLockExclusive(&requests_lock);
    InsertTailList(&requests,&op->entry);
    ReleaseSRWLockExclusive(&requests_lock);
    /* Native notifications cannot escape ahead of the guest IOSB. */
    status=call(file,(HANDLE)((ULONG_PTR)op->event|1),NULL,&op->io,args);
    if(status==STATUS_PENDING)
    {
        /* Some Wine callers, including TransactNamedPipe, do not initialize
         * OVERLAPPED.Internal. Publish pending before arming completion. */
        set_async_iosb((ULONG_PTR)op->guest,STATUS_PENDING,0);
        if(!event&&!apc&&synchronous_control)
        {
            /* Peek/disconnect are synchronous Win32 metadata operations even
             * on an overlapped pipe. Retain storage if the kernel pends. */
            WaitForSingleObject(op->event,INFINITE);
            status=op->io.Status;
            finish_io(op,TRUE,TRUE);
            return status;
        }
        SetThreadpoolWait(op->wait,op->event,NULL);
        /* Keep Wine's temporary marker untouched. Only the completion owns
         * the persistent native block and publishes the guest result. */
        return status;
    }
    finish_io(op,WaitForSingleObject(op->event,0)==WAIT_OBJECT_0,FALSE);
    return status;
}
NTSTATUS WINAPI probe_NtReadFile(HANDLE f,HANDLE e,PIO_APC_ROUTINE a,void *c,IO_STATUS_BLOCK *io,
                                void *b,ULONG n,LARGE_INTEGER *o,ULONG *k)
{ const struct probe_io_args args={.buffer=b,.size=n,.offset=o,.key=k};return probe_submit_io(native_read,f,e,a,c,io,&args,FALSE); }
NTSTATUS WINAPI probe_NtWriteFile(HANDLE f,HANDLE e,PIO_APC_ROUTINE a,void *c,IO_STATUS_BLOCK *io,
                                 const void *b,ULONG n,LARGE_INTEGER *o,ULONG *k)
{ const struct probe_io_args args={.buffer=(void *)b,.size=n,.offset=o,.key=k};return probe_submit_io(native_write,f,e,a,c,io,&args,FALSE); }
NTSTATUS WINAPI probe_NtFsControlFile(HANDLE f,HANDLE e,PIO_APC_ROUTINE a,void *c,IO_STATUS_BLOCK *io,
                                     ULONG code,void *in,ULONG in_size,void *out,ULONG out_size)
{
    const struct probe_io_args args={.buffer=in,.size=in_size,.code=code,.output=out,.output_size=out_size};
    return probe_submit_io(native_fs_control,f,e,a,c,io,&args,code==FSCTL_PIPE_PEEK||code==FSCTL_PIPE_DISCONNECT);
}
NTSTATUS (WINAPI *__imp_probe_NtReadFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,void *,IO_STATUS_BLOCK *,void *,ULONG,LARGE_INTEGER *,ULONG *)=probe_NtReadFile;
NTSTATUS (WINAPI *__imp_probe_NtWriteFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,void *,IO_STATUS_BLOCK *,const void *,ULONG,LARGE_INTEGER *,ULONG *)=probe_NtWriteFile;
NTSTATUS (WINAPI *__imp_probe_NtFsControlFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,void *,IO_STATUS_BLOCK *,ULONG,void *,ULONG,void *,ULONG)=probe_NtFsControlFile;

/* Wine's CreateNamedPipe passes a zero NT timeout for the Win32 default.
 * Native NPFS requires a negative relative timeout. Win32's zero default
 * means 50 milliseconds; keep this backend difference outside Wine. */
NTSTATUS WINAPI probe_NtCreateNamedPipeFile(HANDLE *file,ACCESS_MASK access,OBJECT_ATTRIBUTES *attr,
                                           IO_STATUS_BLOCK *io,ULONG sharing,ULONG disposition,ULONG options,
                                           ULONG type,ULONG mode,ULONG completion,ULONG instances,
                                           ULONG inbound,ULONG outbound,LARGE_INTEGER *timeout)
{
    LARGE_INTEGER default_timeout;
    if(timeout&&!timeout->QuadPart)
    { default_timeout.QuadPart=-500000;timeout=&default_timeout; }
    return NtCreateNamedPipeFile(file,access,attr,io,sharing,disposition,options,type,mode,completion,
                                  instances,inbound,outbound,timeout);
}
NTSTATUS (WINAPI *__imp_probe_NtCreateNamedPipeFile)(HANDLE *,ACCESS_MASK,OBJECT_ATTRIBUTES *,IO_STATUS_BLOCK *,ULONG,ULONG,ULONG,ULONG,ULONG,ULONG,ULONG,ULONG,ULONG,LARGE_INTEGER *)=probe_NtCreateNamedPipeFile;

/* Wine passes the guest IOSB address as the cancellation identifier. Keep
 * its original thunk and translate that identifier to the retained native
 * block. A reference pins the block across completion on another thread. */
NTSTATUS WINAPI probe_NtCancelIoFileEx(HANDLE file,IO_STATUS_BLOCK *guest_request,IO_STATUS_BLOCK *reply)
{
    struct native_io *op=NULL;
    NTSTATUS status;
    if(!guest_request)return NtCancelIoFileEx(file,NULL,reply);
    AcquireSRWLockExclusive(&requests_lock);
    for(LIST_ENTRY *entry=requests.Flink;entry!=&requests;entry=entry->Flink)
    {
        struct native_io *candidate=CONTAINING_RECORD(entry,struct native_io,entry);
        /* OVERLAPPED addresses identify outstanding application requests.
         * Let the native kernel check the supplied file object. Comparing
         * numeric handles here would reject valid DuplicateHandle aliases. */
        if(!candidate->completing&&(void *)candidate->guest==(void *)guest_request)
        { op=candidate;InterlockedIncrement(&op->refs);break; }
    }
    ReleaseSRWLockExclusive(&requests_lock);
    if(!op)return STATUS_NOT_FOUND;
    status=NtCancelIoFileEx(file,&op->io,reply);
    release_io(op);
    return status;
}
NTSTATUS (WINAPI *__imp_probe_NtCancelIoFileEx)(HANDLE,IO_STATUS_BLOCK *,IO_STATUS_BLOCK *)=probe_NtCancelIoFileEx;
void probe_io_drain_thread(DWORD issuer)
{
    for(;;)
    {
        struct native_io *op=NULL;
        AcquireSRWLockExclusive(&requests_lock);
        for(LIST_ENTRY *e=requests.Flink;e!=&requests;e=e->Flink)
        {
            struct native_io *candidate=CONTAINING_RECORD(e,struct native_io,entry);
            if(candidate->issuer==issuer){op=candidate;InterlockedIncrement(&op->refs);break;}
        }
        ReleaseSRWLockExclusive(&requests_lock);
        if(!op)break;
        HANDLE file=NULL;
        AcquireSRWLockShared(&files_lock);
        for(LIST_ENTRY *e=aliases.Flink;e!=&aliases;e=e->Flink)
        {
            struct file_alias *alias=CONTAINING_RECORD(e,struct file_alias,entry);
            if(alias->file==op->file)
            { NtDuplicateObject(GetCurrentProcess(),alias->handle,GetCurrentProcess(),&file,0,0,DUPLICATE_SAME_ACCESS);break; }
        }
        ReleaseSRWLockShared(&files_lock);
        if(file){IO_STATUS_BLOCK reply;NtCancelIoFileEx(file,&op->io,&reply);NtClose(file);}
        AcquireSRWLockExclusive(&requests_lock);
        for(;;)
        {
            BOOL found=FALSE;
            for(LIST_ENTRY *e=requests.Flink;e!=&requests;e=e->Flink)if(e==&op->entry){found=TRUE;break;}
            if(!found)break;
            SleepConditionVariableSRW(&requests_changed,&requests_lock,INFINITE,0);
        }
        ReleaseSRWLockExclusive(&requests_lock);release_io(op);
    }
}
NTSTATUS (WINAPI *__imp_probe_NtClose)(HANDLE)=probe_NtClose;
NTSTATUS (WINAPI *__imp_probe_NtDuplicateObject)(HANDLE,HANDLE,HANDLE,HANDLE *,ACCESS_MASK,ULONG,ULONG)=probe_NtDuplicateObject;
NTSTATUS (WINAPI *__imp_probe_NtWaitForSingleObject)(HANDLE,BOOLEAN,const LARGE_INTEGER *)=probe_NtWaitForSingleObject;
NTSTATUS (WINAPI *__imp_probe_NtWaitForMultipleObjects)(ULONG,const HANDLE *,WAIT_TYPE,BOOLEAN,const LARGE_INTEGER *)=probe_NtWaitForMultipleObjects;
