/* The same system-program observations run on native Windows and Wine. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "test_support.h"
static PROCESS_INFORMATION child;
static void fail(DWORD code)
{
    DWORD error=GetLastError();
    if(child.hProcess){TerminateProcess(child.hProcess,code);WaitForSingleObject(child.hProcess,5000);}
    MESSAGE("BUILTIN_PROCESS_FAILED code=");message_hex(code);
    MESSAGE("last.error=");message_hex(error);ExitProcess(code);
}
void WINAPI app_entry(void)
{
    static WCHAR image[MAX_PATH],command[MAX_PATH+32];static char path[MAX_PATH*3];
    UINT length=GetSystemDirectoryW(image,ARRAY_SIZE(image));
    if(!length||length+9>=ARRAY_SIZE(image))fail(71);
    lstrcatW(image,L"\\cmd.exe");
    int bytes=WideCharToMultiByte(CP_UTF8,0,image,-1,path,sizeof(path),NULL,NULL);
    if(!bytes)fail(72);
    MESSAGE("system.program=");message(path,bytes-1);MESSAGE("\r\n");
    lstrcpyW(command,L"\"");lstrcatW(command,image);lstrcatW(command,L"\" /c exit 0");
    static STARTUPINFOW startup={.cb=sizeof(startup)};
    if(!CreateProcessW(image,command,NULL,NULL,FALSE,CREATE_SUSPENDED,NULL,NULL,&startup,&child))fail(77);
    if(WaitForSingleObject(child.hProcess,0)!=WAIT_TIMEOUT)fail(78);
    typedef NTSTATUS (WINAPI *query_t)(HANDLE,PROCESSINFOCLASS,void *,ULONG,ULONG *);
    query_t query=(query_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess");
    PROCESS_BASIC_INFORMATION basic;PEB peb;IMAGE_DOS_HEADER dos;IMAGE_NT_HEADERS32 nt;
    if(!query||query(child.hProcess,ProcessBasicInformation,&basic,sizeof(basic),NULL))fail(79);
    if(!ReadProcessMemory(child.hProcess,basic.PebBaseAddress,&peb,sizeof(peb),NULL)||!peb.ImageBaseAddress)fail(80);
    if(!ReadProcessMemory(child.hProcess,peb.ImageBaseAddress,&dos,sizeof(dos),NULL)||
       dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0)fail(81);
    if(!ReadProcessMemory(child.hProcess,(BYTE *)peb.ImageBaseAddress+dos.e_lfanew,&nt,sizeof(nt),NULL)||
       nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_I386)fail(82);
    static CONTEXT context={.ContextFlags=CONTEXT_INTEGER|CONTEXT_CONTROL};
    if(!GetThreadContext(child.hThread,&context)||!context.Eax||!context.Eip||
       context.Ebx!=(DWORD)(ULONG_PTR)basic.PebBaseAddress)fail(83);
    if(!TerminateProcess(child.hProcess,37)||WaitForSingleObject(child.hProcess,5000)!=WAIT_OBJECT_0)fail(84);
    DWORD code;if(!GetExitCodeProcess(child.hProcess,&code)||code!=37)fail(85);
    if(!CloseHandle(child.hThread)||!CloseHandle(child.hProcess))fail(86);
    child.hProcess=NULL;
    MESSAGE("builtin_process32: system command interpreter loads as x86, stays suspended and terminates cleanly passed\r\n");
    ExitProcess(0);
}
