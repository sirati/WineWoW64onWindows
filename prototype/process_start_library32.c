/* Attach is observable before the child's executable entry point runs. */
#include <windows.h>
#include <stdint.h>
__declspec(dllexport) DWORD process_start_marker(void) { return GetCurrentProcessId(); }
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void *reserved)
{
    (void)module;(void)reserved;
    if(reason==DLL_PROCESS_ATTACH)
    {
        WCHAR value[16];DWORD handle=0;
        DWORD length=GetEnvironmentVariableW(L"WINE_CHILD_ATTACH_EVENT",value,16);
        if(length&&length<16)
        {
            for(DWORD i=0;i<length;i++)
            {
                WCHAR ch=value[i];
                if(ch>='0'&&ch<='9')handle=handle*16+ch-'0';
                else if(ch>='a'&&ch<='f')handle=handle*16+ch-'a'+10;
                else return FALSE;
            }
            if(!SetEvent((HANDLE)(uintptr_t)handle))return FALSE;
        }
    }
    return TRUE;
}
