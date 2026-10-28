#include "test_support.h"
static volatile LONG main_spinning;
static BOOL spin_mode;
static BOOL has_option(const char *wanted)
{
    const char *line=GetCommandLineA();
    for(;*line;line++){const char *a=line,*b=wanted;while(*a&&*b&&*a==*b){a++;b++;}if(!*b)return TRUE;}
    return FALSE;
}
static void request_exit(void)
{
    MESSAGE("worker_process_exit32: requested process exit 37\r\n");
    ExitProcess(37);
}
static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    if(spin_mode)
    {
        for(unsigned i=0;i<3000&&!main_spinning;i++)Sleep(1);
        if(!main_spinning)ExitProcess(72);
    }
    Sleep(50);request_exit();return 79;
}
void WINAPI app_entry(void)
{
    if(!LoadLibraryA("process_exit_library32.dll"))ExitProcess(71);
    if(has_option("--main")){MESSAGE("mode=main\r\n");request_exit();}
    spin_mode=has_option("--spin");
    if(spin_mode){MESSAGE("mode=worker-spin\r\n");}else{MESSAGE("mode=worker\r\n");}
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);
    if(!thread)ExitProcess(73);
    if(spin_mode)
    {
        MESSAGE("phase=spinning-main\r\n");InterlockedExchange(&main_spinning,1);
        DWORD start=GetTickCount();
        while(GetTickCount()-start<5000)__asm__ volatile("pause":::"memory");
    }
    DWORD wait=WaitForSingleObject(thread,5000),code=0xdeadbeef;
    if(wait==WAIT_OBJECT_0)GetExitCodeThread(thread,&code);
    MESSAGE("unexpected.main.worker-wait=");message_hex(wait);
    MESSAGE("unexpected.main.worker-exit=");message_hex(code);
    CloseHandle(thread);
    MESSAGE("unexpected.main.survived-process-exit\r\n");
    ExitProcess(78);
}
