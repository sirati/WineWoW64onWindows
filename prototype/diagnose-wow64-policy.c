/* Test helper: establish whether native WoW64 actually enables requested CET.
 * This does not load the guest application or change runtime architecture. */
#include <windows.h>
#include <stdio.h>
static ULONG shadow_pointer(void)
{
    ULONG value;
    __asm__ volatile("xorl %0,%0\n\trdsspd %0":"=r"(value)::"cc");
    return value;
}
static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    printf("WOW64_POLICY_THREAD tid=%lu ssp=%08lx\n",GetCurrentThreadId(),shadow_pointer());
    return 0;
}
int main(void)
{
    BOOL wow64=FALSE;DWORD cet=0,aslr=0;
    if(!IsWow64Process(GetCurrentProcess(),&wow64)||
       !GetProcessMitigationPolicy(GetCurrentProcess(),ProcessUserShadowStackPolicy,&cet,sizeof(cet))||
       !GetProcessMitigationPolicy(GetCurrentProcess(),ProcessASLRPolicy,&aslr,sizeof(aslr)))return 2;
    printf("WOW64_POLICY_PROCESS wow64=%d image=%p cet=%lx aslr=%lx ssp=%08lx\n",
           wow64,GetModuleHandleA(NULL),cet,aslr,shadow_pointer());
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);
    if(!thread||WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0)return 3;
    DWORD code;if(!GetExitCodeThread(thread,&code)||code||!CloseHandle(thread))return 4;
    puts("WOW64_POLICY_DIAGNOSTIC_COMPLETE");
    return 0;
}
