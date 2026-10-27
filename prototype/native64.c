#include <windows.h>
#include "shared.h"
typedef LONG (WINAPI *entry_t)(void *);
static LONG WINAPI std_handle(void *p)
{
    struct bridge_args *a=p;
    a->result=(uint32_t)(uintptr_t)GetStdHandle(a->value); return 0;
}
static LONG WINAPI pid(void *p)
{
    ((struct bridge_args *)p)->result=GetCurrentProcessId(); return 0;
}
static LONG WINAPI ticks(void *p)
{
    struct bridge_args *a=p; ULONGLONG t=GetTickCount64();
    a->result=(uint32_t)t; a->high=(uint32_t)(t>>32); return 0;
}
static LONG WINAPI gui_check(void *p)
{
    HWND hwnd=CreateWindowExW(0,L"STATIC",L"Native x64 DLL",WS_POPUP,0,0,160,60,0,0,GetModuleHandleW(0),0);
    struct bridge_args *a=p;
    a->result=hwnd!=0;
    if(hwnd) { SetWindowTextW(hwnd,L"32-bit guest called 64-bit GUI"); DestroyWindow(hwnd); }
    return a->result ? 0 : (LONG)0xc0000001;
}
static LONG WINAPI compute(void *p)
{
    struct bridge_args *a=p;WORD cs;uint64_t value=(uint64_t)a->value*UINT64_C(0x100000003)+7;
    __asm__("movw %%cs,%0":"=r"(cs));
    a->result=(uint32_t)value;a->high=(uint32_t)(value>>32);
    a->extra=cs==0x33&&(uintptr_t)compute>UINT32_MAX;return 0;
}
__declspec(dllexport) DWORD (WINAPI *native_callback32)(DWORD,const DWORD *,UINT);
static LONG WINAPI callback(void *opaque)
{
    struct bridge_args *job=opaque;
    DWORD result=0;
    for(DWORD i=0;i<job->extra;i++)
    {
        DWORD args[]={job->high,i};
        result+=native_callback32(job->value,args,2);
    }
    job->result=result;return 0;
}
static DWORD WINAPI callback_worker(void *opaque)
{ return callback(opaque); }
static LONG WINAPI worker_callback(void *opaque)
{
    HANDLE thread=CreateThread(NULL,0,callback_worker,opaque,0,NULL);
    if(!thread)return (LONG)0xc0000017;
    DWORD wait=WaitForSingleObject(thread,INFINITE),code;
    BOOL ok=wait==WAIT_OBJECT_0&&GetExitCodeThread(thread,&code);
    CloseHandle(thread);return ok?code:(LONG)0xc0000001;
}
__declspec(dllexport) entry_t __wine_unix_call_wow64_funcs[BRIDGE_COUNT]={
    [BRIDGE_GET_STD_HANDLE]=std_handle,[BRIDGE_GET_PID]=pid,[BRIDGE_GET_TICKS]=ticks,
    [BRIDGE_GUI_CHECK]=gui_check,[BRIDGE_COMPUTE]=compute,[BRIDGE_CALLBACK]=callback,
    [BRIDGE_WORKER_CALLBACK]=worker_callback
};
