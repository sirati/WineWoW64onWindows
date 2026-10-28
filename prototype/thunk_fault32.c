/* Faults in native argument conversion must return an NT status. */
#include "test_support.h"
#include <winternl.h>
typedef LONG (WINAPI *set_thread_fn)(HANDLE,ULONG,void *,ULONG);
typedef LONG (WINAPI *open_file_fn)(HANDLE *,ACCESS_MASK,OBJECT_ATTRIBUTES *,IO_STATUS_BLOCK *,ULONG,ULONG);
static set_thread_fn set_thread;
static open_file_fn open_file;
static DWORD WINAPI check_faults(void *unused)
{
    (void)unused;
    WCHAR *inaccessible=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
    if(!inaccessible)return 80;
    for(unsigned i=0;i<32;i++)
    {
        /* Wine's GROUP_AFFINITY32 conversion reads this pointer itself. */
        LONG status=set_thread(GetCurrentThread(),30,NULL,12);
        if(status!=(LONG)0xc0000005u){message_hex(status);return 81;}
        /* The Windows path adapter allocates temporary conversion storage
         * before reading the invalid guest string. Check recovery repeatedly. */
        /* An aligned PAGE_NOACCESS allocation isolates access validation
         * from Windows' separate string-alignment checks. */
        UNICODE_STRING name={.Length=2,.MaximumLength=2,.Buffer=inaccessible};
        OBJECT_ATTRIBUTES attributes={.Length=sizeof(attributes),.ObjectName=&name};
        IO_STATUS_BLOCK io;HANDLE file=NULL;
        status=open_file(&file,GENERIC_READ,&attributes,&io,FILE_SHARE_READ,0);
        if(status!=(LONG)0xc0000005u){message_hex(status);return 82;}
        LARGE_INTEGER counter;
        if(!QueryPerformanceCounter(&counter)||GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_ERROR_RETURN)
            return 83;
    }
    if(!VirtualFree(inaccessible,0,MEM_RELEASE))return 84;
    return 0;
}
void WINAPI app_entry(void)
{
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    set_thread=(set_thread_fn)GetProcAddress(ntdll,"NtSetInformationThread");
    open_file=(open_file_fn)GetProcAddress(ntdll,"NtOpenFile");
    if(!set_thread||!open_file||check_faults(NULL))ExitProcess(71);
    HANDLE thread=CreateThread(NULL,0,check_faults,NULL,0,NULL);DWORD code;
    if(!thread||WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0||
       !GetExitCodeThread(thread,&code)||code||!CloseHandle(thread))ExitProcess(72);
    MESSAGE("thunk_fault32: conversion faults return status and later calls succeed on both threads passed\r\n");
    ExitProcess(0);
}
