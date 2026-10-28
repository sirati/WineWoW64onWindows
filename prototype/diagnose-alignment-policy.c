/* Test-only bridge: check whether Windows' alignment policy avoids the
 * native dispatcher recursion. This DLL is never a runtime dependency. */
#include "native64.c"
#include "test_support.h"
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void *reserved)
{
    (void)module;(void)reserved;
    if(reason==DLL_PROCESS_ATTACH)
    {
        DWORD before=GetErrorMode();
        SetErrorMode(before|SEM_NOALIGNMENTFAULTEXCEPT);
        MESSAGE("ALIGNMENT_POLICY before/after:\r\n");
        message_hex(before);message_hex(GetErrorMode());
    }
    return TRUE;
}
