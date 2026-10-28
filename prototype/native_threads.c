/* Windows thread identities and Wine guest state. Loader/TLS algorithms stay
 * in Wine; their thread enumeration sees only threads that host guest code. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "cpu_host.h"
#include "native_threads.h"
#include "native_process.h"
struct guest_thread {
    LIST_ENTRY entry;
    LONG refs;
    DWORD id;
    HANDLE thread;
    TEB32 *teb;
    struct guest_cpu *volatile cpu;
    void *volatile exception_state;
    void *volatile start_address;
    LONG has_start_address;
    LONG ready;
    SRWLOCK apc_lock;
    LIST_ENTRY pending_apcs;
    void (*cleanup)(void *);
    void *cleanup_arg;
};
struct pending_apc { LIST_ENTRY entry;PNTAPCFUNC function;ULONG_PTR args[3]; };
static SRWLOCK threads_lock=SRWLOCK_INIT;
static LIST_ENTRY threads={&threads,&threads};
static DWORD thread_slot=TLS_OUT_OF_INDEXES;
static NTSTATUS (WINAPI *native_alert_multiple)(HANDLE *,ULONG,void *,void *);
void probe_threads_init(void)
{
    thread_slot=TlsAlloc();if(thread_slot==TLS_OUT_OF_INDEXES)ExitProcess(94);
    native_alert_multiple=(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtAlertMultipleThreadByThreadId");
}
NTSTATUS WINAPI probe_NtAlertMultipleThreadByThreadId(HANDLE *threads,ULONG count,void *unknown1,void *unknown2)
{
    /* Wine owns the array conversion and the validate-then-alert algorithm.
     * Older Windows kernels expose only the individual alert primitive. */
    if(native_alert_multiple)return native_alert_multiple(threads,count,unknown1,unknown2);
    for(ULONG i=0;i<count;i++)
    {
        HANDLE thread;CLIENT_ID id={0,threads[i]};OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes,NULL,0,NULL,NULL);
        NTSTATUS status=NtOpenThread(&thread,0,&attributes,&id);
        if(status)return status;
        NtClose(thread);
    }
    for(ULONG i=0;i<count;i++)NtAlertThreadByThreadId(threads[i]);
    return STATUS_SUCCESS;
}
NTSTATUS (WINAPI *__imp_probe_NtAlertMultipleThreadByThreadId)(HANDLE *,ULONG,void *,void *)=probe_NtAlertMultipleThreadByThreadId;
static BOOL same_thread(HANDLE a,HANDLE b)
{
    typedef NTSTATUS (WINAPI *compare_fn)(HANDLE,HANDLE);
    compare_fn compare=(compare_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCompareObjects");
    return a==b||(compare&&compare(a,b)==STATUS_SUCCESS);
}
void probe_thread_release(struct guest_thread *thread)
{
    if(!InterlockedDecrement(&thread->refs))
    {
        while(!IsListEmpty(&thread->pending_apcs))
            HeapFree(GetProcessHeap(),0,CONTAINING_RECORD(RemoveHeadList(&thread->pending_apcs),struct pending_apc,entry));
        if(thread->cleanup)thread->cleanup(thread->cleanup_arg);
        CloseHandle(thread->thread);HeapFree(GetProcessHeap(),0,thread);
    }
}
struct guest_thread *probe_thread_acquire(HANDLE handle)
{
    struct guest_thread *found=NULL;
    AcquireSRWLockShared(&threads_lock);
    for(LIST_ENTRY *e=threads.Flink;e!=&threads;e=e->Flink)
    {
        struct guest_thread *thread=CONTAINING_RECORD(e,struct guest_thread,entry);
        if(same_thread(handle,thread->thread))
        { found=thread;InterlockedIncrement(&thread->refs);break; }
    }
    ReleaseSRWLockShared(&threads_lock);return found;
}
struct guest_cpu *probe_thread_cpu(struct guest_thread *thread)
{ return InterlockedCompareExchangePointer((void *volatile *)&thread->cpu,NULL,NULL); }
TEB32 *probe_thread_teb(struct guest_thread *thread) { return thread->teb; }
void probe_thread_set_start(HANDLE handle,void *address)
{
    struct guest_thread *thread=probe_thread_acquire(handle);
    if(!thread)return;
    /* Wine's server keeps the guest entry point in its thread record.
     * The Windows thread starts in our native wrapper instead. */
    InterlockedExchangePointer(&thread->start_address,address);
    InterlockedExchange(&thread->has_start_address,1);
    probe_thread_release(thread);
}
struct guest_thread *probe_thread_register(HANDLE handle,TEB32 *teb,struct guest_cpu *cpu,
                                         void (*cleanup)(void *),void *cleanup_arg)
{
    struct guest_thread *thread=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*thread));
    if(!thread)return NULL;
    if(!DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&thread->thread,0,FALSE,DUPLICATE_SAME_ACCESS))
    { HeapFree(GetProcessHeap(),0,thread);return NULL; }
    thread->id=GetThreadId(handle);thread->teb=teb;thread->cpu=cpu;thread->refs=2;
    thread->cleanup=cleanup;thread->cleanup_arg=cleanup_arg;
    thread->ready=cpu==NULL;InitializeSRWLock(&thread->apc_lock);InitializeListHead(&thread->pending_apcs);
    AcquireSRWLockExclusive(&threads_lock);InsertTailList(&threads,&thread->entry);ReleaseSRWLockExclusive(&threads_lock);
    return thread;
}
void probe_thread_bind(struct guest_thread *thread) { TlsSetValue(thread_slot,thread); }
void probe_thread_remove(struct guest_thread *thread)
{
    AcquireSRWLockExclusive(&threads_lock);RemoveEntryList(&thread->entry);ReleaseSRWLockExclusive(&threads_lock);
    probe_thread_release(thread);
}
void probe_thread_exception_state(void *state)
{
    struct guest_thread *thread=TlsGetValue(thread_slot);
    if(thread)InterlockedExchangePointer(&thread->exception_state,state);
}
void *probe_thread_take_exception_state(struct guest_thread *thread)
{ return InterlockedExchangePointer(&thread->exception_state,NULL); }
void probe_thread_ready(void)
{
    struct guest_thread *thread=TlsGetValue(thread_slot);
    if(!thread)return;
    AcquireSRWLockExclusive(&thread->apc_lock);
    while(!IsListEmpty(&thread->pending_apcs))
    {
        struct pending_apc *apc=CONTAINING_RECORD(RemoveHeadList(&thread->pending_apcs),struct pending_apc,entry);
        NtQueueApcThread(thread->thread,apc->function,apc->args[0],apc->args[1],apc->args[2]);
        HeapFree(GetProcessHeap(),0,apc);
    }
    thread->ready=TRUE;
    ReleaseSRWLockExclusive(&thread->apc_lock);
}
NTSTATUS WINAPI probe_NtQueueApcThread(HANDLE handle,PNTAPCFUNC function,ULONG_PTR a,ULONG_PTR b,ULONG_PTR c)
{
    struct guest_thread *thread=probe_thread_acquire(handle);
    if(!thread)
    {
        NTSTATUS status=NtQueueApcThread(handle,function,a,b,c);
        /* Wine's queue_apc server handler returns STATUS_UNSUCCESSFUL when
         * an exited thread cannot accept the request. Native x64 Windows
         * uses STATUS_THREAD_IS_TERMINATING for the same condition. */
        return status==STATUS_THREAD_IS_TERMINATING?STATUS_UNSUCCESSFUL:status;
    }
    /* Serialize the initial queue drain with new requests to preserve FIFO. */
    AcquireSRWLockExclusive(&thread->apc_lock);
    NTSTATUS status;
    if(thread->ready)status=NtQueueApcThread(handle,function,a,b,c);
    else
    {
        OBJECT_BASIC_INFORMATION info;
        status=NtQueryObject(handle,ObjectBasicInformation,&info,sizeof(info),NULL);
        if(!status&&!(info.GrantedAccess&THREAD_SET_CONTEXT))status=STATUS_ACCESS_DENIED;
        LARGE_INTEGER zero={0};
        if(!status&&NtWaitForSingleObject(thread->thread,FALSE,&zero)==STATUS_SUCCESS)status=STATUS_THREAD_IS_TERMINATING;
        struct pending_apc *apc=NULL;
        if(!status&&!(apc=HeapAlloc(GetProcessHeap(),0,sizeof(*apc))))status=STATUS_NO_MEMORY;
        if(!status)
        {
            apc->function=function;apc->args[0]=a;apc->args[1]=b;apc->args[2]=c;
            InsertTailList(&thread->pending_apcs,&apc->entry);
        }
    }
    ReleaseSRWLockExclusive(&thread->apc_lock);
    probe_thread_release(thread);
    return status==STATUS_THREAD_IS_TERMINATING?STATUS_UNSUCCESSFUL:status;
}
NTSTATUS WINAPI probe_NtQueueApcThreadEx2(HANDLE handle,HANDLE reserve,ULONG flags,
                                         PNTAPCFUNC function,ULONG_PTR a,ULONG_PTR b,ULONG_PTR c)
{
    /* Reserve objects and callback-data contexts need their own native
     * lifetime adapters. Ordinary APCs share the existing startup FIFO. */
    if(reserve)return STATUS_NOT_IMPLEMENTED;
    if(function&&(flags&QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC))
    {
        union { OBJECT_TYPE_INFORMATION type;BYTE bytes[512]; } object;
        OBJECT_BASIC_INFORMATION basic;
        NTSTATUS status=NtQueryObject(handle,ObjectTypeInformation,&object,sizeof(object),NULL);
        if(status)return status;
        if(object.type.TypeName.Length!=6*sizeof(WCHAR)||
           memcmp(object.type.TypeName.Buffer,L"Thread",6*sizeof(WCHAR)))return STATUS_OBJECT_TYPE_MISMATCH;
        status=NtQueryObject(handle,ObjectBasicInformation,&basic,sizeof(basic),NULL);
        if(status)return status;
        if(!(basic.GrantedAccess&THREAD_SET_CONTEXT))return STATUS_ACCESS_DENIED;
        /* server/thread.c's queue_apc handler rejects special APCs for
         * WoW64 targets. Every guest target in this port is x86. */
        return STATUS_NOT_SUPPORTED;
    }
    if(function&&(flags&QUEUE_USER_APC_CALLBACK_DATA_CONTEXT))return STATUS_NOT_IMPLEMENTED;
    return probe_NtQueueApcThread(handle,function,a,b,c);
}
void probe_thread_enter(struct guest_cpu *cpu,struct guest_cpu **previous)
{
    struct guest_thread *thread=TlsGetValue(thread_slot);
    if(!thread)
    {
        thread=probe_thread_register(GetCurrentThread(),(void *)cpu->guest_fs_base,NULL,NULL,NULL);
        if(!thread)ExitProcess(94);
        probe_thread_bind(thread);probe_thread_release(thread);
        probe_thread_set_start(GetCurrentThread(),(void *)(ULONG_PTR)cpu->ctx.Eip);
    }
    *previous=InterlockedExchangePointer((void *volatile *)&thread->cpu,cpu);
}
void probe_thread_leave(struct guest_cpu *previous)
{
    struct guest_thread *thread=TlsGetValue(thread_slot);
    if(thread)
        InterlockedExchangePointer((void *volatile *)&thread->cpu,previous);
}
void probe_thread_unregister(void)
{
    struct guest_thread *thread=TlsGetValue(thread_slot);
    if(!thread)return;
    TlsSetValue(thread_slot,NULL);probe_thread_remove(thread);
}
NTSTATUS WINAPI probe_RtlWow64GetThreadSelectorEntry(HANDLE handle,THREAD_DESCRIPTOR_INFORMATION *info,
                                                    ULONG size,ULONG *returned)
{
    NTSTATUS status=RtlWow64GetThreadSelectorEntry(handle,info,size,returned);
    if(!status)
    {
        struct guest_thread *thread=probe_thread_acquire(handle);
        if(thread)
        {
            struct guest_cpu *cpu=probe_thread_cpu(thread);
            /* Windows validates the selector and supplies its attributes.
             * The guest FS base lives in our separately allocated Wine TEB. */
            if(cpu&&((info->Selector^cpu->ctx.SegFs)&~3u)==0)
            {
                ULONG base=(ULONG)(ULONG_PTR)thread->teb;
                info->Entry.BaseLow=base;info->Entry.HighWord.Bytes.BaseMid=base>>16;
                info->Entry.HighWord.Bytes.BaseHi=base>>24;
            }
            probe_thread_release(thread);
        }
    }
    return status;
}
NTSTATUS (WINAPI *__imp_probe_RtlWow64GetThreadSelectorEntry)(HANDLE,THREAD_DESCRIPTOR_INFORMATION *,ULONG,ULONG *)=probe_RtlWow64GetThreadSelectorEntry;
void probe_system_process_tebs(void *info,ULONG size)
{
    /* Wine's Unix backend fills TebBase from its own thread records before
     * put_system_proc_info converts it to x86. Use the same guest records,
     * since Windows reports each host thread's separate native x64 TEB. */
    ULONG offset=0;
    AcquireSRWLockShared(&threads_lock);
    while(size-offset>=offsetof(SYSTEM_PROCESS_INFORMATION,ti))
    {
        SYSTEM_PROCESS_INFORMATION *process=(void *)((BYTE *)info+offset);
        ULONG length=process->NextEntryOffset?process->NextEntryOffset:size-offset;
        if(length>size-offset||length<offsetof(SYSTEM_PROCESS_INFORMATION,ti))break;
        if(process->UniqueProcessId==ULongToHandle(GetCurrentProcessId())&&
           process->dwThreadCount<=(length - offsetof(SYSTEM_PROCESS_INFORMATION,ti))/sizeof(SYSTEM_EXTENDED_THREAD_INFORMATION))
        {
            SYSTEM_EXTENDED_THREAD_INFORMATION *list=(void *)process->ti;
            for(ULONG i=0;i<process->dwThreadCount;i++)
                for(LIST_ENTRY *entry=threads.Flink;entry!=&threads;entry=entry->Flink)
                {
                    struct guest_thread *thread=CONTAINING_RECORD(entry,struct guest_thread,entry);
                    if(list[i].ThreadInfo.ClientId.UniqueThread==ULongToHandle(thread->id))
                    {
                        /* Extended snapshots expose the native TEB, while
                         * ThreadBasicInformation adds Wine's WoW64 offset. */
                        list[i].TebBase=(BYTE *)thread->teb-0x2000;
                        break;
                    }
                }
        }
        if(!process->NextEntryOffset)break;
        offset+=process->NextEntryOffset;
    }
    ReleaseSRWLockShared(&threads_lock);
}
NTSTATUS WINAPI probe_NtQueryInformationThread(HANDLE handle,THREADINFOCLASS type,void *info,ULONG size,ULONG *returned)
{
    NTSTATUS status=NtQueryInformationThread(handle,type,info,size,returned);
    /* Wine's Win32 wrappers use the backend's relative priority directly.
     * Windows NT represents the two extreme values as +/-16 instead of 15. */
    if(!status&&type==ThreadBasicInformation)
    {
        THREAD_BASIC_INFORMATION *basic=info;
        if(basic->BasePriority==THREAD_PRIORITY_TIME_CRITICAL+1)--basic->BasePriority;
        else if(basic->BasePriority==THREAD_PRIORITY_IDLE-1)++basic->BasePriority;
    }
    if(!status&&(type==ThreadBasicInformation||type==ThreadQuerySetWin32StartAddress))
    {
        struct guest_thread *thread=probe_thread_acquire(handle);
        if(thread)
        {
            if(type==ThreadBasicInformation)
            {
                /* Wine's original 32-bit thunk adds its fixed TEB offset. */
                ((THREAD_BASIC_INFORMATION *)info)->TebBaseAddress=(TEB *)((BYTE *)thread->teb-0x2000);
            }
            else if(InterlockedCompareExchange(&thread->has_start_address,0,0))
                *(void **)info=InterlockedCompareExchangePointer(&thread->start_address,NULL,NULL);
            probe_thread_release(thread);
        }
        else if(type==ThreadBasicInformation)
        {
            ULONG_PTR teb;
            if(probe_remote_guest_thread(handle,NULL,&teb,NULL))
                ((THREAD_BASIC_INFORMATION *)info)->TebBaseAddress=(TEB *)(teb-0x2000);
        }
    }
    return status;
}
NTSTATUS WINAPI probe_NtSetInformationThread(HANDLE handle,THREADINFOCLASS type,void *info,ULONG size)
{
    if(type==ThreadQuerySetWin32StartAddress)
    {
        NTSTATUS status=NtSetInformationThread(handle,type,info,size);
        if(!status)probe_thread_set_start(handle,*(void **)info);
        return status;
    }
    if(type==ThreadBasePriority&&size==sizeof(LONG)&&info)
    {
        LONG priority=*(LONG *)info;
        if(priority==THREAD_PRIORITY_TIME_CRITICAL)++priority;
        else if(priority==THREAD_PRIORITY_IDLE)--priority;
        return NtSetInformationThread(handle,type,&priority,sizeof(priority));
    }
    if(type!=ThreadZeroTlsCell)return NtSetInformationThread(handle,type,info,size);
    if(size!=sizeof(ULONG))return STATUS_INFO_LENGTH_MISMATCH;
    ULONG index=*(ULONG *)info;
    if(index>=64+1024)return STATUS_INVALID_PARAMETER;
    AcquireSRWLockShared(&threads_lock);
    for(LIST_ENTRY *e=threads.Flink;e!=&threads;e=e->Flink)
    {
        struct guest_thread *thread=CONTAINING_RECORD(e,struct guest_thread,entry);
        if(index<64)thread->teb->TlsSlots[index]=0;
        else if(thread->teb->TlsExpansionSlots)((ULONG *)(ULONG_PTR)thread->teb->TlsExpansionSlots)[index-64]=0;
    }
    ReleaseSRWLockShared(&threads_lock);return STATUS_SUCCESS;
}
NTSTATUS WINAPI probe_NtGetNextThread(HANDLE process,HANDLE previous,ACCESS_MASK access,ULONG attributes,ULONG flags,HANDLE *result)
{
    if(process!=GetCurrentProcess()&&GetProcessId(process)!=GetCurrentProcessId())return STATUS_NOT_SUPPORTED;
    if(flags)return STATUS_INVALID_PARAMETER;
    BOOL next=!previous;NTSTATUS status=STATUS_NO_MORE_ENTRIES;*result=NULL;
    AcquireSRWLockShared(&threads_lock);
    for(LIST_ENTRY *e=threads.Flink;e!=&threads;e=e->Flink)
    {
        struct guest_thread *thread=CONTAINING_RECORD(e,struct guest_thread,entry);
        if(next)
        { status=NtDuplicateObject(GetCurrentProcess(),thread->thread,GetCurrentProcess(),result,access,attributes,0);break; }
        if(same_thread(previous,thread->thread))next=TRUE;
    }
    ReleaseSRWLockShared(&threads_lock);return status;
}
NTSTATUS (WINAPI *__imp_probe_NtQueryInformationThread)(HANDLE,THREADINFOCLASS,void *,ULONG,ULONG *)=probe_NtQueryInformationThread;
NTSTATUS (WINAPI *__imp_probe_NtSetInformationThread)(HANDLE,THREADINFOCLASS,void *,ULONG)=probe_NtSetInformationThread;
NTSTATUS (WINAPI *__imp_probe_NtGetNextThread)(HANDLE,HANDLE,ACCESS_MASK,ULONG,ULONG,HANDLE *)=probe_NtGetNextThread;
NTSTATUS (WINAPI *__imp_probe_NtQueueApcThread)(HANDLE,PNTAPCFUNC,ULONG_PTR,ULONG_PTR,ULONG_PTR)=probe_NtQueueApcThread;
NTSTATUS (WINAPI *__imp_probe_NtQueueApcThreadEx2)(HANDLE,HANDLE,ULONG,PNTAPCFUNC,ULONG_PTR,ULONG_PTR,ULONG_PTR)=probe_NtQueueApcThreadEx2;
