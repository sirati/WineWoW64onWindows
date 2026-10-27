#include "test_support.h"
void WINAPI app_entry(void)
{
    HMODULE m=LoadLibraryA("kernel32.dll");
    if(!m||!GetProcAddress(m,"GetCurrentProcessId")) ExitProcess(53);
    FreeLibrary(m);MESSAGE("dynamic32: dynamic load/export lookup passed\r\n");ExitProcess(0);
}
