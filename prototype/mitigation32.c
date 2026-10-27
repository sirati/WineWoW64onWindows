#include "test_support.h"
static void *target;
static volatile DWORD nx_faults,write_faults;
static void fail_at(DWORD stage)
{ MESSAGE("mitigation32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static LONG CALLBACK handler(EXCEPTION_POINTERS *p)
{
    EXCEPTION_RECORD *record=p->ExceptionRecord;
    if(record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION||record->ExceptionInformation[1]!=(ULONG_PTR)target)
        return EXCEPTION_CONTINUE_SEARCH;
    if(record->ExceptionInformation[0]==8)
    {
        ++nx_faults;p->ContextRecord->Eip=*(DWORD *)(ULONG_PTR)p->ContextRecord->Esp;
        p->ContextRecord->Esp+=4;return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(record->ExceptionInformation[0]==1)
    { ++write_faults;p->ContextRecord->Eip+=3;return EXCEPTION_CONTINUE_EXECUTION; }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void execute(void *p) __attribute__((naked));
static void execute(void *p)
{ __asm__ volatile("movl 4(%esp),%eax\n\tcall *%eax\n\tret"); }
static void write_readonly(void *p) __attribute__((naked));
static void write_readonly(void *p)
{ __asm__ volatile("movl 4(%esp),%eax\n\tmovb $0x90,(%eax)\n\tret"); }
void WINAPI app_entry(void)
{
    static const char *names[]={NULL,"ntdll.dll","kernel32.dll","kernelbase.dll","msvcrt.dll","bootstrap32.dll","bridge_kernel32.dll"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)
    {
        HMODULE module=GetModuleHandleA(names[i]);
        if(i<3)REQUIRE(module,10);else if(!module)continue;
        MEMORY_BASIC_INFORMATION info;REQUIRE(VirtualQuery(module,&info,sizeof(info))&&info.Protect==PAGE_READONLY,11);
        IMAGE_DOS_HEADER *dos=(void *)module;IMAGE_NT_HEADERS32 *nt=(void *)((BYTE *)module+dos->e_lfanew);
        IMAGE_SECTION_HEADER *section=IMAGE_FIRST_SECTION(nt);
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++)if(section[j].Misc.VirtualSize)
        {
            REQUIRE(VirtualQuery((BYTE *)module+section[j].VirtualAddress,&info,sizeof(info)),12);
            if((section[j].Characteristics&(IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE))==IMAGE_SCN_MEM_EXECUTE)
                REQUIRE(!(info.Protect&(PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY|PAGE_READWRITE|PAGE_WRITECOPY)),13);
            if(!(section[j].Characteristics&IMAGE_SCN_MEM_EXECUTE))
                REQUIRE(!(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)),14);
        }
    }
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    DWORD *gate=(DWORD *)GetProcAddress(ntdll,"__wine_syscall_dispatcher");
    if(gate)
    {
        MEMORY_BASIC_INFORMATION info;
        REQUIRE(VirtualQuery((void *)(ULONG_PTR)*gate,&info,sizeof(info))&&info.Protect==PAGE_EXECUTE_READ,15);
    }
    target=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);REQUIRE(target,20);
    *(BYTE *)target=0xc3;void *veh=AddVectoredExceptionHandler(1,handler);REQUIRE(veh,21);
    execute(target);REQUIRE(nx_faults==1,22);
    DWORD old;REQUIRE(VirtualProtect(target,4096,PAGE_READONLY,&old),23);
    write_readonly(target);REQUIRE(write_faults==1&&*(BYTE *)target==0xc3,24);
    REQUIRE(RemoveVectoredExceptionHandler(veh)&&VirtualFree(target,0,MEM_RELEASE),25);
    MESSAGE("mitigation32: bootstrap PE section permissions, NX execution fault and read-only write fault passed\r\n");ExitProcess(0);
}
