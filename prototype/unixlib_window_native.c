/* Native Windows prerequisite/control for the supplied, unchanged unixlib.
 * The library's public WoW64 table takes low-address plain data even when
 * the control itself is native x64. This is not a Wine guest executable. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct frames { uint32_t frames,timeout_ms,waited_ms,pad; };
struct info { uint32_t address,capacity,length,pad; };
typedef LONG (*unix_entry)(void *);
struct low_state {
    struct frames main,waiters[4];
    struct info info;
    char text[512];
    LONG status[4];
};
static unix_entry *table;
static struct low_state *state;
static DWORD WINAPI waiter(void *arg)
{
    unsigned index=(unsigned)(uintptr_t)arg;
    state->status[index]=table[1](&state->waiters[index]);return 0;
}
static void adapter(void)
{
    state->info=(struct info){(uint32_t)(uintptr_t)state->text,sizeof(state->text)-1,0,0};
    LONG status=table[2](&state->info);
    if(status)snprintf(state->text,sizeof(state->text),"status=%08lx",status);
    else state->text[min(state->info.length,sizeof(state->text)-1)]=0;
}
static DWORD WINAPI exit_worker(void *unused)
{
    (void)unused;Sleep(500);
    struct frames *frames=&state->waiters[0];memset(frames,0,sizeof(*frames));
    table[1](frames);
    printf("NATIVE_WINDOW_WORKER_EXIT frames=%u\n",frames->frames);fflush(stdout);
    ExitProcess(38);
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    if(argc!=3)return 2;
    HMODULE library=LoadLibraryExA(argv[1],NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!library){printf("NATIVE_WINDOW_FAILED load error=%lu\n",GetLastError());return 2;}
    table=(unix_entry *)GetProcAddress(library,"__wine_unix_call_wow64_funcs");
    if(!table)return 2;
    for(uintptr_t address=0x20000000;address<0x70000000;address+=0x10000)
        if((state=VirtualAlloc((void *)address,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)))break;
    if(!state)return 2;
    LONG status=table[0](&state->main);
    if(status){printf("NATIVE_WINDOW_FAILED open status=%08lx\n",status);return 3;}
    BOOL wait=!strcmp(argv[2],"wait");
    state->main=(struct frames){wait?60:30,30000,0,0};
    status=table[1](&state->main);adapter();
    if(status){printf("NATIVE_WINDOW_FAILED first-frames status=%08lx frames=%u waited_ms=%u adapter=%s\n",status,state->main.frames,state->main.waited_ms,state->text);return 3;}
    if(!strcmp(argv[2],"exit-live"))
    {printf("NATIVE_WINDOW_EXIT_LIVE frames=%u adapter=%s\n",state->main.frames,state->text);ExitProcess(37);}
    if(!strcmp(argv[2],"worker-exit-live"))
    {
        HANDLE thread=CreateThread(NULL,0,exit_worker,NULL,0,NULL);if(!thread)return 3;
        state->main=(struct frames){UINT32_MAX,60000,0,0};
        status=table[1](&state->main);
        printf("NATIVE_WINDOW_FAILED blocked call returned status=%08lx waited_ms=%u\n",status,state->main.waited_ms);return 5;
    }
    if(!wait)return 2;
    HANDLE threads[4];uint32_t target=state->main.frames+120;
    for(unsigned i=0;i<4;i++)
    {
        state->waiters[i]=(struct frames){target,30000,0,0};
        threads[i]=CreateThread(NULL,0,waiter,(void *)(uintptr_t)i,0,NULL);
        if(!threads[i])return 3;
    }
    unsigned spins=0;
    while(WaitForMultipleObjects(4,threads,TRUE,0)==WAIT_TIMEOUT){spins++;SwitchToThread();}
    for(unsigned i=0;i<4;i++)
    {
        CloseHandle(threads[i]);
        if(state->status[i]||state->waiters[i].frames<target)return 3;
    }
    if(!spins)return 3;
    state->main=(struct frames){0,10000,0,0};
    status=table[3](&state->main);
    if(status){printf("NATIVE_WINDOW_FAILED close status=%08lx waited_ms=%u\n",status,state->main.waited_ms);return 4;}
    printf("NATIVE_WINDOW_OK mode=wait frames=%u adapter=%s\n",state->main.frames,state->text);
    return 0;
}
