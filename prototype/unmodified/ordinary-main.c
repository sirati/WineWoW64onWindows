#include <stdio.h>
#include <windows.h>
int main(int argc,char **argv)
{
    printf("ordinary-main: argc=%d exe=%s pid=%lu\n",argc,argv[0],GetCurrentProcessId());
    HMODULE module=LoadLibraryA("kernel32.dll");
    if(!module||!GetProcAddress(module,"GetCurrentProcessId")) return 5;
    FreeLibrary(module);
    return 0;
}
