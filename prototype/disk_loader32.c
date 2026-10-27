#include <stdio.h>
#include <windows.h>
int main(void)
{
    typedef DWORD (WINAPI *version_size_t)(const char *,DWORD *);
    for(unsigned i=0;i<32;i++)
    {
        HMODULE dll=LoadLibraryA("version.dll");
        if(!dll)return 61;
        version_size_t size=(version_size_t)GetProcAddress(dll,"GetFileVersionInfoSizeA");
        if(!size)return 62;
        DWORD ignored;
        if(size("this-version-test-file-does-not-exist.exe",&ignored))return 63;
        if(!FreeLibrary(dll))return 64;
        if(GetModuleHandleA("version.dll"))return 65;
    }
    HMODULE alias=LoadLibraryA("api-ms-win-core-sysinfo-l1-1-0.dll");
    if(!alias)return 66;
    typedef void (WINAPI *get_time_t)(FILETIME *);
    get_time_t get_time=(get_time_t)GetProcAddress(alias,"GetSystemTimeAsFileTime");
    if(!get_time)return 67;
    FILETIME time={0};get_time(&time);if(!time.dwHighDateTime)return 68;
    if(!FreeLibrary(alias))return 69;
    puts("disk_loader32: 32 DLL load/call/unload cycles and API-set lookup passed");
    return 0;
}
