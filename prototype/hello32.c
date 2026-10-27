/* A Windows i386 console EXE with ordinary kernel32 imports and no CRT. */
#include <windows.h>
static void print(HANDLE out,const char *s,DWORD n)
{
    DWORD done=0;
    if(!WriteFile(out,s,n,&done,0)||done!=n) ExitProcess(11);
}
void WINAPI app_entry(void)
{
    HANDLE out=GetStdHandle(STD_OUTPUT_HANDLE);
    const char hello[]="hello32: Windows 32-bit console application started\r\n";
    const char ok[]="hello32: stdout, process ID, 64-bit DLL call, memory and sleep passed\r\n";
    unsigned char *p;
    DWORD i,pid=GetCurrentProcessId();
    ULONGLONG before=GetTickCount64();
    if(!pid) ExitProcess(12);
    print(out,hello,sizeof(hello)-1);
    p=VirtualAlloc(0,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!p) ExitProcess(13);
    for(i=0;i<8192;i++) p[i]=(unsigned char)(i*17);
    for(i=0;i<8192;i++) if(p[i]!=(unsigned char)(i*17)) ExitProcess(14);
    if(!VirtualFree(p,0,MEM_RELEASE)) ExitProcess(15);
    Sleep(20);
    if(GetTickCount64()<before) ExitProcess(16);
    print(out,ok,sizeof(ok)-1);
    ExitProcess(0);
}
