#include <windows.h>
#include "test_support.h"
/* A 32-bit process sees the 32-bit registry view by default. ProgramFilesDir
 * differs between the views on 64-bit Windows: the 32-bit view names the
 * (x86) directory. Explicit KEY_WOW64_32KEY/KEY_WOW64_64KEY select a view. */
static DWORD read_program_files(REGSAM view,char *value,DWORD size)
{
    HKEY key;DWORD type=0;
    LONG error=RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",0,KEY_READ|view,&key);
    if(error)return error;
    error=RegQueryValueExA(key,"ProgramFilesDir",NULL,&type,(BYTE *)value,&size);
    RegCloseKey(key);
    if(!error&&type!=REG_SZ)error=ERROR_INVALID_DATA;
    return error;
}
static BOOL has_x86_suffix(const char *value)
{
    for(;*value;value++)if(!lstrcmpiA(value," (x86)"))return TRUE;
    return FALSE;
}
void WINAPI app_entry(void)
{
    static char by_default[MAX_PATH],view32[MAX_PATH],view64[MAX_PATH];
    if(read_program_files(0,by_default,sizeof(by_default)))ExitProcess(21);
    if(read_program_files(KEY_WOW64_32KEY,view32,sizeof(view32)))ExitProcess(22);
    if(read_program_files(KEY_WOW64_64KEY,view64,sizeof(view64)))ExitProcess(23);
    MESSAGE("default=");message(by_default,lstrlenA(by_default));
    MESSAGE("\r\nview32=");message(view32,lstrlenA(view32));
    MESSAGE("\r\nview64=");message(view64,lstrlenA(view64));MESSAGE("\r\n");
    if(lstrcmpiA(by_default,view32))ExitProcess(24);
    if(!has_x86_suffix(view32))ExitProcess(25);
    if(has_x86_suffix(view64))ExitProcess(26);
    MESSAGE("registry_view32: default and explicit 32-bit views read WOW6432Node, 64-bit view reads the native key passed\r\n");
    ExitProcess(0);
}
