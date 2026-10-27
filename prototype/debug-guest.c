/* Native Windows debugger for diagnosing our own host's guest exceptions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
typedef LONG (WINAPI *create_section_t)(HANDLE *,DWORD,void *,LARGE_INTEGER *,ULONG,ULONG,HANDLE);
typedef LONG (WINAPI *map_section_t)(HANDLE,HANDLE,void **,ULONG_PTR,SIZE_T,LARGE_INTEGER *,SIZE_T *,int,ULONG,ULONG);
/* Test helper: keep the file's canonical image view alive in the debugger,
 * and reserve that address in the child before the port maps its bootstrap. */
static BOOL reserve_bootstrap_image(HANDLE process,DWORD pid,const char *path)
{
    HMODULE native=GetModuleHandleA("ntdll.dll");
    create_section_t create=(create_section_t)GetProcAddress(native,"NtCreateSection");
    map_section_t map=(map_section_t)GetProcAddress(native,"NtMapViewOfSection");
    HANDLE file=CreateFileA(path,GENERIC_READ|GENERIC_EXECUTE,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL),section=NULL;
    if(file==INVALID_HANDLE_VALUE||!create||!map)return FALSE;
    LONG status=create(&section,SECTION_MAP_READ|SECTION_MAP_EXECUTE,NULL,NULL,PAGE_EXECUTE_READ,SEC_IMAGE,file);
    CloseHandle(file);if(status<0)return FALSE;
    void *view=NULL;SIZE_T size=0;
    status=map(section,GetCurrentProcess(),&view,0,0,NULL,&size,2,0,PAGE_READONLY);
    CloseHandle(section);if(status<0)return FALSE;
    IMAGE_NT_HEADERS32 *nt=(void *)((BYTE *)view+((IMAGE_DOS_HEADER *)view)->e_lfanew);
    void *canonical=(void *)(ULONG_PTR)nt->OptionalHeader.ImageBase;
    void *reserved=VirtualAllocEx(process,canonical,nt->OptionalHeader.SizeOfImage,MEM_RESERVE,PAGE_NOACCESS);
    fprintf(stderr,"DEBUG_BOOTSTRAP_RESERVATION pid=%lu image=%s canonical=%p view=%p reserved=%p size=%lx error=%lu\n",
            pid,path,canonical,view,reserved,nt->OptionalHeader.SizeOfImage,reserved?0:GetLastError());
    /* The debugger process exits with the child, releasing the reference view. */
    return reserved==canonical;
}
int main(void)
{
    char *line=GetCommandLineA();
    if(*line=='"'){line++;while(*line&&*line!='"')line++;if(*line)line++;}
    else while(*line&&*line!=' ')line++;
    while(*line==' ')line++;
    BOOL children=!strncmp(line,"--children ",11);
    if(children)line+=11;
    STARTUPINFOA si={.cb=sizeof(si)};PROCESS_INFORMATION pi={0};DEBUG_EVENT event;
    if(!CreateProcessA(NULL,line,NULL,NULL,TRUE,children?DEBUG_PROCESS:DEBUG_ONLY_THIS_PROCESS,NULL,NULL,&si,&pi))return 80;
    unsigned count=0;
    for(;;)
    {
        if(!WaitForDebugEvent(&event,15000))
        {
            SuspendThread(pi.hThread);CONTEXT c={.ContextFlags=CONTEXT_ALL};GetThreadContext(pi.hThread,&c);
            fprintf(stderr,"DEBUG_TIMEOUT rip=%llx rsp=%llx cs=%x eax=%llx ebx=%llx ecx=%llx edx=%llx esi=%llx edi=%llx ebp=%llx\n",c.Rip,c.Rsp,c.SegCs,c.Rax,c.Rbx,c.Rcx,c.Rdx,c.Rsi,c.Rdi,c.Rbp);
            DWORD stack[16]={0};SIZE_T read;ReadProcessMemory(pi.hProcess,(void *)c.Rsp,stack,sizeof(stack),&read);
            fputs("DEBUG_TIMEOUT_STACK",stderr);for(unsigned i=0;i<16;i++)fprintf(stderr," %08lx",stack[i]);fputc('\n',stderr);
            TerminateProcess(pi.hProcess,98);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 98;
        }
        DWORD disposition=DBG_CONTINUE;
        if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT)
        {
            EXCEPTION_DEBUG_INFO *ex=&event.u.Exception;
            /* Windows also reports an initial compatibility-mode loader
             * breakpoint as STATUS_WX86_BREAKPOINT. */
            if(ex->ExceptionRecord.ExceptionCode!=EXCEPTION_BREAKPOINT&&
               ex->ExceptionRecord.ExceptionCode!=0x4000001f)
            {
                disposition=DBG_EXCEPTION_NOT_HANDLED;
                HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,event.dwThreadId);
                CONTEXT c={.ContextFlags=CONTEXT_ALL};GetThreadContext(thread,&c);CloseHandle(thread);
                HANDLE process=OpenProcess(PROCESS_VM_READ,FALSE,event.dwProcessId);
                BYTE code[16]={0};SIZE_T read;ReadProcessMemory(process,(void *)c.Rip,code,sizeof(code),&read);
                fprintf(stderr,"DEBUG_EXCEPTION #%u pid=%lu first=%lu code=%08lx rip=%llx rsp=%llx cs=%x info0=%llx fault=%llx eax=%llx ecx=%llx edx=%llx edi=%llx esi=%llx ebp=%llx bytes=",++count,event.dwProcessId,ex->dwFirstChance,ex->ExceptionRecord.ExceptionCode,c.Rip,c.Rsp,c.SegCs,(unsigned long long)ex->ExceptionRecord.ExceptionInformation[0],(unsigned long long)ex->ExceptionRecord.ExceptionInformation[1],c.Rax,c.Rcx,c.Rdx,c.Rdi,c.Rsi,c.Rbp);
                for(unsigned i=0;i<16;i++)fprintf(stderr,"%02x",code[i]);fputc('\n',stderr);
                DWORD stack[12]={0};ReadProcessMemory(process,(void *)c.Rsp,stack,sizeof(stack),&read);
                fputs("DEBUG_STACK",stderr);for(unsigned i=0;i<12;i++)fprintf(stderr," %08lx",stack[i]);fputc('\n',stderr);
                CloseHandle(process);
            }
        }
        if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT)
        {
            char path[1024]="?";
            HANDLE file=event.u.CreateProcessInfo.hFile;
            if(file)GetFinalPathNameByHandleA(file,path,sizeof(path),0);
            fprintf(stderr,"DEBUG_IMAGE pid=%lu base=%p path=%s\n",event.dwProcessId,event.u.CreateProcessInfo.lpBaseOfImage,path);
            if(file)CloseHandle(file);
            const char *bootstrap=getenv("WINE_TEST_OCCUPY_BOOTSTRAP_IMAGE");
            if(bootstrap&&strstr(path,"wine-host.exe")&&!reserve_bootstrap_image(event.u.CreateProcessInfo.hProcess,event.dwProcessId,bootstrap))
                TerminateProcess(event.u.CreateProcessInfo.hProcess,82);
            if(getenv("WINE_TEST_OCCUPY_GATE_BASE"))
            {
                void *reservation=VirtualAllocEx(event.u.CreateProcessInfo.hProcess,(void *)0x20000000,
                                                 0x10000,MEM_RESERVE,PAGE_NOACCESS);
                fprintf(stderr,"DEBUG_GATE_RESERVATION pid=%lu base=%p error=%lu\n",
                        event.dwProcessId,reservation,reservation?0:GetLastError());
                if(!reservation)TerminateProcess(event.u.CreateProcessInfo.hProcess,82);
            }
        }
        if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT&&event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);
        if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT)
        {
            DWORD code=event.u.ExitProcess.dwExitCode;fprintf(stderr,"DEBUG_EXIT pid=%lu code=%lu\n",event.dwProcessId,code);
            if(event.dwProcessId==pi.dwProcessId)
            { ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code; }
        }
        ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition);
    }
}
