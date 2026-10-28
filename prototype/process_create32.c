/* Public parent/child observations, unchanged for native Windows and Wine. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "test_support.h"
__declspec(dllimport) DWORD process_start_marker(void);
static PROCESS_INFORMATION child_process;
static void fail(DWORD code)
{
    DWORD error=GetLastError();
    if(child_process.hProcess)
    {TerminateProcess(child_process.hProcess,code);WaitForSingleObject(child_process.hProcess,5000);}
    MESSAGE("PROCESS_CREATE_FAILED code=");message_hex(code);
    MESSAGE("last.error=");message_hex(error);ExitProcess(code);
}
static BOOL same(const WCHAR *a,const WCHAR *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static BOOL contains(const WCHAR *text,const WCHAR *needle)
{
    for(;*text;text++)
    {const WCHAR *a=text,*b=needle;while(*b&&*a==*b){a++;b++;}if(!*b)return TRUE;}
    return FALSE;
}
static void handle_string(HANDLE handle,WCHAR *value)
{
    const WCHAR hex[]=L"0123456789abcdef";
    for(unsigned i=0;i<8;i++)value[i]=hex[((ULONG_PTR)handle>>(28-4*i))&15];value[8]=0;
}
static const char child_output[]="process_create32: child Unicode environment and directory passed\r\n";
static void run_child(void)
{
    static WCHAR environment[64],expected_directory[1024],directory[1024];
    if(!GetEnvironmentVariableW(L"WINE_CHILD_VALUE",environment,ARRAY_SIZE(environment))||
       !same(environment,L"child-\x03a9-\xd83d\xde00"))fail(91);
    if(!GetEnvironmentVariableW(L"WINE_CHILD_DIRECTORY",expected_directory,ARRAY_SIZE(expected_directory))||
       !GetCurrentDirectoryW(ARRAY_SIZE(directory),directory)||!same(directory,expected_directory))fail(92);
    if(process_start_marker()!=GetCurrentProcessId())fail(93);
    DWORD written;
    if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),child_output,sizeof(child_output)-1,&written,NULL)||
       written!=sizeof(child_output)-1)fail(94);
    ExitProcess(41);
}
void WINAPI app_entry(void)
{
    if(contains(GetCommandLineW(),L" --child"))run_child();
    if(process_start_marker()!=GetCurrentProcessId())fail(71);
    static WCHAR image[1024],directory[1024],command[2048],handle_text[16];
    if(!GetModuleFileNameW(NULL,image,ARRAY_SIZE(image))||!GetTempPathW(ARRAY_SIZE(directory),directory))fail(72);
    lstrcatW(directory,L"wine-child-\x03a9-");
    handle_string((HANDLE)(ULONG_PTR)GetCurrentProcessId(),handle_text);lstrcatW(directory,handle_text);
    if(!CreateDirectoryW(directory,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS)fail(73);
    if(!SetEnvironmentVariableW(L"WINE_CHILD_DIRECTORY",directory)||
       !SetEnvironmentVariableW(L"WINE_CHILD_VALUE",L"child-\x03a9-\xd83d\xde00"))fail(74);
    SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
    for(unsigned suspended=0;suspended<2;suspended++)
    {
        if(suspended)MESSAGE("phase=suspended-child\r\n");else MESSAGE("phase=unsuspended-child\r\n");
        HANDLE attach=CreateEventW(&security,TRUE,FALSE,NULL),read_pipe,write_pipe;
        if(!attach||!CreatePipe(&read_pipe,&write_pipe,&security,0)||
           !SetHandleInformation(read_pipe,HANDLE_FLAG_INHERIT,0))fail(75);
        handle_string(attach,handle_text);
        if(!SetEnvironmentVariableW(L"WINE_CHILD_ATTACH_EVENT",handle_text))fail(76);
        static STARTUPINFOW startup;
        startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=startup.hStdError=write_pipe;
        lstrcpyW(command,L"\"");lstrcatW(command,image);lstrcatW(command,L"\" --child");
        if(!CreateProcessW(image,command,NULL,NULL,TRUE,suspended?CREATE_SUSPENDED:0,
                           NULL,directory,&startup,&child_process))fail(77);
        if(suspended)
        {
            if(WaitForSingleObject(attach,0)!=WAIT_TIMEOUT)fail(78);
            typedef NTSTATUS (WINAPI *query_t)(HANDLE,PROCESSINFOCLASS,void *,ULONG,ULONG *);
            query_t query=(query_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess");
            PROCESS_BASIC_INFORMATION basic;
            if(!query||query(child_process.hProcess,ProcessBasicInformation,&basic,sizeof(basic),NULL))fail(79);
            PEB peb;
            if(!ReadProcessMemory(child_process.hProcess,basic.PebBaseAddress,&peb,sizeof(peb),NULL)||!peb.ImageBaseAddress)fail(80);
            static CONTEXT context;
            context.ContextFlags=CONTEXT_INTEGER|CONTEXT_CONTROL;
            BOOL context_ok=GetThreadContext(child_process.hThread,&context);
            if(!context_ok||context.Ebx!=(DWORD)(ULONG_PTR)basic.PebBaseAddress||!context.Eax||!context.Eip)
            {
                MESSAGE("initial.eax=");message_hex(context.Eax);
                MESSAGE("initial.ebx=");message_hex(context.Ebx);
                MESSAGE("initial.eip=");message_hex(context.Eip);
                MESSAGE("initial.peb=");message_hex((DWORD)(ULONG_PTR)basic.PebBaseAddress);
                fail(81);
            }
            if(!SetThreadContext(child_process.hThread,&context))fail(82);
            if(SuspendThread(child_process.hThread)!=1||ResumeThread(child_process.hThread)!=2||
               WaitForSingleObject(attach,0)!=WAIT_TIMEOUT||ResumeThread(child_process.hThread)!=1)fail(83);
        }
        CloseHandle(write_pipe);
        if(WaitForSingleObject(child_process.hProcess,10000)!=WAIT_OBJECT_0)fail(84);
        DWORD exit_code;
        if(!GetExitCodeProcess(child_process.hProcess,&exit_code)||exit_code!=41||
           WaitForSingleObject(attach,0)!=WAIT_OBJECT_0)fail(85);
        char output[sizeof(child_output)+128];DWORD total=0,bytes;
        while(total<sizeof(output)&&ReadFile(read_pipe,output+total,sizeof(output)-total,&bytes,NULL)&&bytes)total+=bytes;
        if(total!=sizeof(child_output)-1){if(total)message(output,total);fail(86);}
        for(DWORD i=0;i<total;i++)if(output[i]!=child_output[i])fail(87);
        CloseHandle(read_pipe);CloseHandle(attach);
        CloseHandle(child_process.hProcess);CloseHandle(child_process.hThread);
        child_process.hProcess=child_process.hThread=NULL;
        SetEnvironmentVariableW(L"WINE_CHILD_ATTACH_EVENT",NULL);
    }
    if(!RemoveDirectoryW(directory))fail(88);
    MESSAGE("completed.children=00000002\r\n");
    MESSAGE("process_create32: child loading, suspended DLL attach, Unicode parameters, inherited handles and redirected output passed\r\n");
    ExitProcess(0);
}
