/* Check elapsed milliseconds independently of the shared-data conversion. */
#include "test_support.h"
typedef DWORD (WINAPI *nt_ticks_fn)(void);
void WINAPI app_entry(void)
{
    nt_ticks_fn nt_ticks=(nt_ticks_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtGetTickCount");
    if(!nt_ticks)ExitProcess(71);
    for(unsigned i=0;i<3;i++)
    {
        DWORD before=GetTickCount(),nt_before=nt_ticks();
        ULONGLONG wide_before=GetTickCount64();
        Sleep(200);
        DWORD elapsed=GetTickCount()-before,nt_elapsed=nt_ticks()-nt_before;
        ULONGLONG wide_elapsed=GetTickCount64()-wide_before;
        MESSAGE("tick_count32 elapsed GetTickCount/NtGetTickCount/GetTickCount64:\r\n");
        message_hex(elapsed);message_hex(nt_elapsed);message_hex((DWORD)wide_elapsed);
        if(elapsed<150||elapsed>10000||nt_elapsed<150||nt_elapsed>10000||wide_elapsed<150||wide_elapsed>10000)
            ExitProcess(72);
    }
    MESSAGE("tick_count32: all three tick APIs measure elapsed milliseconds passed\r\n");
    ExitProcess(0);
}
