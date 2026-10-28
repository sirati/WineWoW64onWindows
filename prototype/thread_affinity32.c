/* Exercise Wine's original CreateRemoteThreadEx group-affinity conversion. */
#include "test_support.h"

static DWORD WINAPI query_affinity(void *opaque)
{
    return GetThreadGroupAffinity(GetCurrentThread(),opaque)?0:GetLastError();
}

static LPPROC_THREAD_ATTRIBUTE_LIST create_attributes(GROUP_AFFINITY *requested)
{
    SIZE_T size=0;
    if(InitializeProcThreadAttributeList(NULL,1,0,&size)||GetLastError()!=ERROR_INSUFFICIENT_BUFFER)ExitProcess(72);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes=HeapAlloc(GetProcessHeap(),0,size);
    if(!attributes||!InitializeProcThreadAttributeList(attributes,1,0,&size))ExitProcess(72);
    if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_GROUP_AFFINITY,
                                  requested,sizeof(*requested),NULL,NULL))ExitProcess(73);
    return attributes;
}

static void free_attributes(LPPROC_THREAD_ATTRIBUTE_LIST attributes)
{
    DeleteProcThreadAttributeList(attributes);HeapFree(GetProcessHeap(),0,attributes);
}

static void check_affinity(GROUP_AFFINITY *requested)
{
    GROUP_AFFINITY actual={0};DWORD code=~0u;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes=create_attributes(requested);
    HANDLE thread=CreateRemoteThreadEx(GetCurrentProcess(),NULL,0,query_affinity,&actual,0,attributes,NULL);
    DWORD error=thread?0:GetLastError();
    free_attributes(attributes);
    MESSAGE("requested.mask=");message_hex(requested->Mask);
    MESSAGE("create.error=");message_hex(error);
    if(!thread)ExitProcess(74);
    if(WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0||!GetExitCodeThread(thread,&code)||code)ExitProcess(75);
    if(!CloseHandle(thread)||actual.Group!=requested->Group||actual.Mask!=requested->Mask)ExitProcess(76);
    MESSAGE("actual.mask=");message_hex(actual.Mask);
}

void WINAPI app_entry(void)
{
    GROUP_AFFINITY initial={0};
    if(!GetThreadGroupAffinity(GetCurrentThread(),&initial)||!initial.Mask)ExitProcess(71);
    GROUP_AFFINITY requested=initial;
    check_affinity(&requested);
    /* Select one CPU, preferring a different bit from the first one. */
    ULONG_PTR remaining=initial.Mask&(initial.Mask-1);
    requested.Mask=remaining?(remaining&-remaining):(initial.Mask&-initial.Mask);
    check_affinity(&requested);
    ULONG_PTR invalid=~initial.Mask;
    if(invalid)
    {
        requested.Mask=invalid&-invalid;
        LPPROC_THREAD_ATTRIBUTE_LIST attributes=create_attributes(&requested);
        HANDLE thread=CreateRemoteThreadEx(GetCurrentProcess(),NULL,0,query_affinity,&initial,0,attributes,NULL);
        DWORD error=GetLastError();
        free_attributes(attributes);
        if(thread){WaitForSingleObject(thread,10000);CloseHandle(thread);ExitProcess(78);}
        if(error!=ERROR_INVALID_PARAMETER)ExitProcess(79);
        MESSAGE("invalid.mask=");message_hex(requested.Mask);
    }
    MESSAGE("thread_affinity32: thread creation preserves group affinity and rejects invalid CPUs passed\r\n");
    ExitProcess(0);
}
