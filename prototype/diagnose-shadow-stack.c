/* Test helper: measure Windows native shadow-stack address selection.
 * No policy changes or guest services are installed. */
#include <windows.h>
#include <stdio.h>

typedef LONG (NTAPI *set_process_t)(HANDLE,int,void *,ULONG);
typedef LONG (NTAPI *create_thread_t)(HANDLE *,ACCESS_MASK,void *,HANDLE,
                                    LPTHREAD_START_ROUTINE,void *,ULONG,ULONG_PTR,SIZE_T,SIZE_T,void *);
struct shadow_allocation
{
    ULONGLONG reserve,commit;
    ULONG preferred_node,reserved;
    void *ssp;
};
_Static_assert(sizeof(struct shadow_allocation)==32,"native fiber shadow-stack request");

static ULONG_PTR shadow_pointer(void)
{
    ULONG_PTR value;
    __asm__ volatile("xorq %0,%0\n\trdsspq %0":"=r"(value)::"cc");
    return value;
}
static DWORD WINAPI thread_entry(void *argument)
{
    CONTEXT context={0};RtlCaptureContext(&context);
    printf("SHADOW_THREAD zero_bits=%llx thread=%lu sp=%llx ssp=%llx\n",
           (unsigned long long)(ULONG_PTR)argument,GetCurrentThreadId(),
           (unsigned long long)context.Rsp,(unsigned long long)shadow_pointer());
    return 0;
}

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    DWORD policy=0;
    if(!GetProcessMitigationPolicy(GetCurrentProcess(),ProcessUserShadowStackPolicy,&policy,sizeof(policy)))return 2;
    printf("SHADOW_PROCESS image=%p cet=%lx ssp=%llx\n",GetModuleHandleA(NULL),policy,(unsigned long long)shadow_pointer());
    HMODULE native=GetModuleHandleA("ntdll.dll");
    set_process_t set_process=(set_process_t)GetProcAddress(native,"NtSetInformationProcess");
    create_thread_t create_thread=(create_thread_t)GetProcAddress(native,"NtCreateThreadEx");
    if(!set_process||!create_thread)return 3;
    const ULONG_PTR requested[]={0,0x40000000};
    for(unsigned i=0;i<sizeof(requested)/sizeof(requested[0]);i++)
    {
        struct shadow_allocation allocation={0x100000,0x10000,0,0,(void *)requested[i]};
        LONG status=set_process(GetCurrentProcess(),98,&allocation,sizeof(allocation));
        printf("SHADOW_ALLOCATION requested=%p status=%08lx ssp=%p reserve=%llx commit=%llx\n",
               (void *)requested[i],status,allocation.ssp,allocation.reserve,allocation.commit);
        if(status<0)continue;
        if(allocation.ssp)
        {
            MEMORY_BASIC_INFORMATION info={0};
            SIZE_T queried=VirtualQuery((BYTE *)allocation.ssp-8,&info,sizeof(info));
            printf("SHADOW_VIEW query=%llu base=%p allocation=%p size=%llx protect=%lx type=%lx\n",
                   (unsigned long long)queried,info.BaseAddress,info.AllocationBase,
                   (unsigned long long)info.RegionSize,info.Protect,info.Type);
            status=set_process(GetCurrentProcess(),99,&allocation.ssp,sizeof(allocation.ssp));
            printf("SHADOW_FREE status=%08lx\n",status);
        }
    }
    const ULONG_PTR zero_bits[]={0,0x7fffffff,0xffffffff};
    for(unsigned i=0;i<sizeof(zero_bits)/sizeof(zero_bits[0]);i++)
    {
        HANDLE thread=NULL;
        LONG status=create_thread(&thread,THREAD_ALL_ACCESS,NULL,GetCurrentProcess(),thread_entry,
                                  (void *)zero_bits[i],0,zero_bits[i],0x10000,0x100000,NULL);
        printf("SHADOW_CREATE zero_bits=%llx status=%08lx\n",(unsigned long long)zero_bits[i],status);
        if(status<0)continue;
        if(WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0)return 4;
        DWORD code;if(!GetExitCodeThread(thread,&code)||code)return 5;
        CloseHandle(thread);
    }
    puts("SHADOW_DIAGNOSTIC_COMPLETE");return 0;
}
