/* Exercise Wine's unixlib lookup when the builtin PE is outside its package. */
#include "test_support.h"
void WINAPI app_entry(void)
{
    static WCHAR path[32768];
    DWORD length=GetEnvironmentVariableW(L"UNIXLIB_SEARCH_DLL",path,32768);
    if(length>=32768)ExitProcess(76);
    HMODULE module=LoadLibraryW(length?path:L"opengl32.dll");
    if(!module){MESSAGE("UNIXLIB_SEARCH_FAILED error=");message_hex(GetLastError());ExitProcess(78);}
    if(!GetProcAddress(module,"wglGetCurrentContext"))ExitProcess(79);
    if(!FreeLibrary(module))ExitProcess(80);
    MESSAGE("UNIXLIB_SEARCH_OK\r\n");
    ExitProcess(0);
}
