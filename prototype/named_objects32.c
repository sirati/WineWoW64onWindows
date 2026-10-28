/* Exercise the public named-object callers used by unchanged C runtimes. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "test_support.h"
static void fail(DWORD code)
{
    DWORD error=GetLastError();
    MESSAGE("NAMED_OBJECTS_FAILED code=");message_hex(code);
    MESSAGE("last_error=");message_hex(error);ExitProcess(code);
}
static void make_name(WCHAR *name,WCHAR kind)
{
    static const WCHAR prefix[]=L"Local\\winewow64-object-";
    static const WCHAR hex[]=L"0123456789abcdef";
    unsigned i=0;while(prefix[i]){name[i]=prefix[i];i++;}
    name[i++]=kind;name[i++]=L'-';
    DWORD pid=GetCurrentProcessId();
    for(int bit=28;bit>=0;bit-=4)name[i++]=hex[(pid>>bit)&15];
    name[i]=0;
}
void WINAPI app_entry(void)
{
    DWORD session;
    if(!ProcessIdToSessionId(GetCurrentProcessId(),&session)||
       session!=NtCurrentTeb()->Peb->SessionId)fail(71);
    WCHAR name[80];make_name(name,L'm');
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,4096,name);
    if(!mapping)fail(72);
    HANDLE alias=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,8192,name);
    if(!alias||GetLastError()!=ERROR_ALREADY_EXISTS)fail(73);
    HANDLE opened=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
    DWORD *a=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,4096);
    DWORD *b=opened?MapViewOfFile(opened,FILE_MAP_ALL_ACCESS,0,0,4096):NULL;
    if(!a||!b)fail(74);
    a[0]=0x1234abcd;if(b[0]!=0x1234abcd)fail(75);
    b[1023]=0xaabbccdd;if(a[1023]!=0xaabbccdd)fail(76);
    UnmapViewOfFile(a);UnmapViewOfFile(b);
    CloseHandle(opened);CloseHandle(alias);CloseHandle(mapping);
    make_name(name,L'e');
    HANDLE event=CreateEventW(NULL,TRUE,FALSE,name);
    HANDLE event2=OpenEventW(EVENT_ALL_ACCESS,FALSE,name);
    if(!event||!event2||!SetEvent(event)||WaitForSingleObject(event2,0)!=WAIT_OBJECT_0||
       !ResetEvent(event2)||WaitForSingleObject(event,0)!=WAIT_TIMEOUT)fail(77);
    CloseHandle(event2);CloseHandle(event);
    make_name(name,L's');
    HANDLE semaphore=CreateSemaphoreW(NULL,2,3,name);
    HANDLE semaphore2=OpenSemaphoreW(SEMAPHORE_ALL_ACCESS,FALSE,name);
    LONG previous;
    if(!semaphore||!semaphore2||WaitForSingleObject(semaphore,0)!=WAIT_OBJECT_0||
       !ReleaseSemaphore(semaphore2,1,&previous)||previous!=1)fail(78);
    CloseHandle(semaphore2);CloseHandle(semaphore);
    make_name(name,L'x');
    HANDLE mutex=CreateMutexW(NULL,FALSE,name);
    HANDLE mutex2=OpenMutexW(MUTEX_ALL_ACCESS,FALSE,name);
    if(!mutex||!mutex2||WaitForSingleObject(mutex2,0)!=WAIT_OBJECT_0||!ReleaseMutex(mutex))fail(79);
    CloseHandle(mutex2);CloseHandle(mutex);
    if(CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,4096,
                         L"Local\\winewow64-no-such-directory\\mapping"))fail(80);
    MESSAGE("named_objects32: session, Local mappings, events, semaphores and mutexes passed\r\n");
    ExitProcess(0);
}
