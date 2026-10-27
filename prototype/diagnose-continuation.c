/* Test helper: enter compatibility mode through the Windows context service.
 * A native VEH records the first guest trap and exits without returning. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef LONG (NTAPI *create_t)(HANDLE *,DWORD,void *,LARGE_INTEGER *,ULONG,ULONG,HANDLE);
typedef LONG (NTAPI *map_t)(HANDLE,HANDLE,void **,ULONG_PTR,SIZE_T,LARGE_INTEGER *,SIZE_T *,int,ULONG,ULONG);
typedef LONG (NTAPI *continue_t)(CONTEXT *,BOOLEAN);
typedef void (NTAPI *restore_t)(CONTEXT *,EXCEPTION_RECORD *);
static ULONG_PTR guest_entry;
static BOOL single_step;
static volatile ULONG_PTR native_stack __attribute__((used));

static LONG CALLBACK __attribute__((used,noreturn)) trap(EXCEPTION_POINTERS *exception)
{
    CONTEXT *context=exception->ContextRecord;
    DWORD code=exception->ExceptionRecord->ExceptionCode;
    /* Windows reports a breakpoint context at INT3, a single-step context
     * immediately after NOP. Both instructions occupy offset one here. */
    ULONG_PTR expected=guest_entry+1;
    DWORD expected_code=single_step?EXCEPTION_SINGLE_STEP:EXCEPTION_BREAKPOINT;
    printf("CONTINUATION_TRAP code=%08lx cs=%04x ip=%llx sp=%llx flags=%08lx expected_ip=%llx\n",
           code,context->SegCs,(unsigned long long)context->Rip,(unsigned long long)context->Rsp,
           context->EFlags,(unsigned long long)expected);
    fflush(stdout);
    ExitProcess(code==expected_code&&context->SegCs==0x23&&context->Rip==expected?0:20);
}
/* Windows invokes the native VEH on the interrupted guest stack. The native
 * CRT must run on the stack described by the native TEB, as in the port. */
static LONG CALLBACK __attribute__((naked)) trap_entry(EXCEPTION_POINTERS *exception)
{
    __asm__ volatile("movq native_stack(%rip),%rsp\n\tandq $-16,%rsp\n\tsubq $0x20,%rsp\n\tcall trap\n\tud2");
}

int main(int argc,char **argv)
{
    if(argc<2)return 2;
    DWORD flags=CONTEXT_FULL;BOOL restore=FALSE;
    for(int i=2;i<argc;i++)
    {
        if(!strcmp(argv[i],"--single-step"))single_step=TRUE;
        else if(!strcmp(argv[i],"--segments"))flags|=CONTEXT_SEGMENTS;
        else if(!strcmp(argv[i],"--all"))flags=CONTEXT_ALL;
        else if(!strcmp(argv[i],"--restore"))restore=TRUE;
        else return 2;
    }
    setvbuf(stdout,NULL,_IONBF,0);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    HMODULE native=GetModuleHandleA("ntdll.dll");
    create_t create=(create_t)GetProcAddress(native,"NtCreateSection");
    map_t map=(map_t)GetProcAddress(native,"NtMapViewOfSection");
    continue_t resume=(continue_t)GetProcAddress(native,"NtContinue");
    restore_t restore_context=(restore_t)GetProcAddress(native,"RtlRestoreContext");
    HANDLE file=CreateFileA(argv[1],GENERIC_READ|GENERIC_EXECUTE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL),section=NULL;
    if(file==INVALID_HANDLE_VALUE||!create||!map||!resume)return 3;
    LONG status=create(&section,SECTION_MAP_READ|SECTION_MAP_EXECUTE,NULL,NULL,PAGE_EXECUTE_READ,SEC_IMAGE,file);
    CloseHandle(file);if(status<0)return 4;
    void *base=NULL;SIZE_T size=0;
    status=map(section,GetCurrentProcess(),&base,0,0,NULL,&size,2,0,PAGE_READONLY);
    CloseHandle(section);if(status<0||(ULONG_PTR)base>0xffffffffu-size)return 5;
    IMAGE_NT_HEADERS32 *nt=(void *)((BYTE *)base+((IMAGE_DOS_HEADER *)base)->e_lfanew);
    guest_entry=(ULONG_PTR)base+nt->OptionalHeader.AddressOfEntryPoint;
    BYTE *stack=NULL;
    for(ULONG_PTR address=0x30000000;address<0x70000000&&!stack;address+=0x100000)
        stack=VirtualAlloc((void *)address,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!stack||!AddVectoredExceptionHandler(1,trap_entry))return 6;
    DWORD policy=0;
    GetProcessMitigationPolicy(GetCurrentProcess(),ProcessUserShadowStackPolicy,&policy,sizeof(policy));
    CONTEXT context={0};
    RtlCaptureContext(&context);
    native_stack=context.Rsp-0x100;
    context.ContextFlags=flags;
    context.Rip=guest_entry;context.Rsp=(ULONG_PTR)stack+0xff00;
    context.SegCs=0x23;context.SegSs=context.SegDs=context.SegEs=0x2b;
    context.EFlags=(context.EFlags&~0x10100u)|0x200u|(single_step?0x100u:0);
    ULONG_PTR shadow_stack;
    __asm__ volatile("xorq %0,%0\n\trdsspq %0":"=r"(shadow_stack)::"cc");
    printf("CONTINUATION_ENTER cet=%lx step=%d flags=%08lx restore=%d ip=%llx sp=%llx ssp=%llx\n",
           policy,single_step,context.ContextFlags,restore,(unsigned long long)context.Rip,
           (unsigned long long)context.Rsp,(unsigned long long)shadow_stack);
    if(restore)
    {
        if(!restore_context)return 8;
        restore_context(&context,NULL);
        return 9;
    }
    status=resume(&context,FALSE);
    printf("CONTINUATION_RETURN status=%08lx\n",status);
    return 7;
}
