/* Native Windows debugger for diagnosing our own host's guest exceptions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
static void dump_threads(HANDLE process,DWORD pid)
{
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 entry={.dwSize=sizeof(entry)};
    SymInitialize(process,NULL,TRUE);
    if(snapshot!=INVALID_HANDLE_VALUE&&Thread32First(snapshot,&entry))do
    {
        if(entry.th32OwnerProcessID!=pid)continue;
        HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SUSPEND_RESUME|THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);
        if(!thread)continue;
        DWORD suspended=SuspendThread(thread);CONTEXT context={.ContextFlags=CONTEXT_ALL};
        if(suspended!=(DWORD)-1&&GetThreadContext(thread,&context))
        {
            fprintf(stderr,"DEBUG_TIMEOUT_THREAD tid=%lu rip=%llx rsp=%llx cs=%x\n",
                    entry.th32ThreadID,context.Rip,context.Rsp,context.SegCs);
            STACKFRAME64 frame={0};frame.AddrPC.Offset=context.Rip;frame.AddrPC.Mode=AddrModeFlat;
            frame.AddrStack.Offset=context.Rsp;frame.AddrStack.Mode=AddrModeFlat;
            frame.AddrFrame.Offset=context.Rbp;frame.AddrFrame.Mode=AddrModeFlat;
            for(unsigned i=0;i<20&&context.SegCs==0x33;i++)
            {
                if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,thread,&frame,&context,NULL,
                                SymFunctionTableAccess64,SymGetModuleBase64,NULL))break;
                fprintf(stderr,"DEBUG_TIMEOUT_FRAME tid=%lu index=%u pc=%llx base=%llx sp=%llx\n",
                        entry.th32ThreadID,i,(unsigned long long)frame.AddrPC.Offset,
                        (unsigned long long)SymGetModuleBase64(process,frame.AddrPC.Offset),
                        (unsigned long long)frame.AddrStack.Offset);
            }
        }
        if(suspended!=(DWORD)-1)ResumeThread(thread);
        CloseHandle(thread);
    }while(Thread32Next(snapshot,&entry));
    if(snapshot!=INVALID_HANDLE_VALUE)CloseHandle(snapshot);
    SymCleanup(process);
}
typedef LONG (WINAPI *create_section_t)(HANDLE *,DWORD,void *,LARGE_INTEGER *,ULONG,ULONG,HANDLE);
typedef LONG (WINAPI *map_section_t)(HANDLE,HANDLE,void **,ULONG_PTR,SIZE_T,LARGE_INTEGER *,SIZE_T *,int,ULONG,ULONG);
static BOOL reserve_application_image(HANDLE process,DWORD pid,const char *path)
{
    HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    IMAGE_DOS_HEADER dos;IMAGE_NT_HEADERS32 nt;DWORD read=0;
    if(file==INVALID_HANDLE_VALUE)return FALSE;
    BOOL valid=ReadFile(file,&dos,sizeof(dos),&read,NULL)&&read==sizeof(dos)&&
        dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>=0&&
        SetFilePointer(file,dos.e_lfanew,NULL,FILE_BEGIN)!=INVALID_SET_FILE_POINTER&&
        ReadFile(file,&nt,sizeof(nt),&read,NULL)&&read==sizeof(nt)&&
        nt.Signature==IMAGE_NT_SIGNATURE&&nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    CloseHandle(file);if(!valid)return FALSE;
    void *preferred=(void *)(ULONG_PTR)nt.OptionalHeader.ImageBase;
    void *reserved=VirtualAllocEx(process,preferred,0x10000,MEM_RESERVE,PAGE_NOACCESS);
    fprintf(stderr,"DEBUG_APPLICATION_RESERVATION pid=%lu image=%s preferred=%p reserved=%p error=%lu\n",
            pid,path,preferred,reserved,reserved?0:GetLastError());
    return reserved==preferred;
}
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
            dump_threads(pi.hProcess,pi.dwProcessId);
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
                fprintf(stderr,"DEBUG_EXCEPTION #%u pid=%lu tid=%lu first=%lu code=%08lx rip=%llx rsp=%llx cs=%x info0=%llx fault=%llx eax=%llx ecx=%llx edx=%llx edi=%llx esi=%llx ebp=%llx bytes=",++count,event.dwProcessId,event.dwThreadId,ex->dwFirstChance,ex->ExceptionRecord.ExceptionCode,c.Rip,c.Rsp,c.SegCs,(unsigned long long)ex->ExceptionRecord.ExceptionInformation[0],(unsigned long long)ex->ExceptionRecord.ExceptionInformation[1],c.Rax,c.Rcx,c.Rdx,c.Rdi,c.Rsi,c.Rbp);
                for(unsigned i=0;i<16;i++)fprintf(stderr,"%02x",code[i]);fputc('\n',stderr);
                DWORD stack[12]={0};ReadProcessMemory(process,(void *)c.Rsp,stack,sizeof(stack),&read);
                fputs("DEBUG_STACK",stderr);for(unsigned i=0;i<12;i++)fprintf(stderr," %08lx",stack[i]);fputc('\n',stderr);
                if(c.SegCs==0x23&&ex->ExceptionRecord.ExceptionInformation[0]==8&&stack[0]>=6)
                {
                    BYTE call[6]={0};DWORD slot=0,target=0;
                    if(ReadProcessMemory(process,(void *)(ULONG_PTR)(stack[0]-6),call,sizeof(call),&read)&&
                       call[0]==0xff&&call[1]==0x15)
                    {
                        memcpy(&slot,call+2,sizeof(slot));
                        ReadProcessMemory(process,(void *)(ULONG_PTR)slot,&target,sizeof(target),&read);
                        fprintf(stderr,"DEBUG_INDIRECT_CALL tid=%lu return=%08lx slot=%08lx target=%08lx\n",
                                event.dwThreadId,stack[0],slot,target);
                    }
                }
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
            const char *application=getenv("WINE_TEST_OCCUPY_APPLICATION_IMAGE");
            if(application&&strstr(path,"wine-host.exe")&&!reserve_application_image(event.u.CreateProcessInfo.hProcess,event.dwProcessId,application))
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
