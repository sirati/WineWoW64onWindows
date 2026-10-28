/* Check elapsed milliseconds independently of the shared-data conversion. */
#include "test_support.h"
typedef DWORD (WINAPI *nt_ticks_fn)(void);
typedef ULONGLONG (WINAPI *wide_ticks_fn)(void);
void WINAPI app_entry(void)
{
    nt_ticks_fn nt_ticks=(nt_ticks_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtGetTickCount");
    HMODULE kernelbase=GetModuleHandleA("kernelbase.dll");
    nt_ticks_fn base_ticks=(nt_ticks_fn)GetProcAddress(kernelbase,"GetTickCount");
    wide_ticks_fn base_wide_ticks=(wide_ticks_fn)GetProcAddress(kernelbase,"GetTickCount64");
    if(!nt_ticks||!base_ticks||!base_wide_ticks)ExitProcess(71);
    for(unsigned i=0;i<3;i++)
    {
        DWORD before=GetTickCount(),nt_before=nt_ticks();
        ULONGLONG wide_before=GetTickCount64();
        DWORD base_before=base_ticks();
        ULONGLONG base_wide_before=base_wide_ticks();
        Sleep(200);
        DWORD elapsed=GetTickCount()-before,nt_elapsed=nt_ticks()-nt_before;
        ULONGLONG wide_elapsed=GetTickCount64()-wide_before;
        DWORD base_elapsed=base_ticks()-base_before;
        ULONGLONG base_wide_elapsed=base_wide_ticks()-base_wide_before;
        MESSAGE("tick_count32 elapsed kernel32/NtGetTickCount/kernel32-64/kernelbase/kernelbase-64:\r\n");
        message_hex(elapsed);message_hex(nt_elapsed);message_hex((DWORD)wide_elapsed);
        message_hex(base_elapsed);message_hex((DWORD)base_wide_elapsed);
        if(elapsed<150||elapsed>10000||nt_elapsed<150||nt_elapsed>10000||wide_elapsed<150||wide_elapsed>10000||
           base_elapsed<150||base_elapsed>10000||base_wide_elapsed<150||base_wide_elapsed>10000)
            ExitProcess(72);
    }
    MESSAGE("tick_count32: all five tick exports measure elapsed milliseconds passed\r\n");
    ExitProcess(0);
}
