#include "test_support.h"
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void *reserved)
{
    (void)module;(void)reserved;
    if(reason==DLL_PROCESS_ATTACH)MESSAGE("process_exit_library32: process attach\r\n");
    if(reason==DLL_PROCESS_DETACH)MESSAGE("process_exit_library32: process detach\r\n");
    return TRUE;
}
