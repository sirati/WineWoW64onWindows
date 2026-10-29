/* Native x64 companion for Wine's x86 opengl32 builtin.
 *
 * The entry points mirror the null-device part of Wine's existing
 * dlls/opengl32/unix_wgl.c: process/thread attach and detach succeed, while
 * pixel-format and rendering calls remain unavailable until Wine's win32u
 * null display path is present.  Keeping a real table lets the PE's DllMain
 * complete instead of treating an optional Unixlib as a missing DLL.
 */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>

typedef NTSTATUS (WINAPI *unixlib_entry_t)(void *);

static NTSTATUS WINAPI process_attach(void *args)
{
    (void)args;
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI thread_attach(void *args)
{
    (void)args;
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI process_detach(void *args)
{
    (void)args;
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI unavailable(void *args)
{
    (void)args;
    return STATUS_NOT_IMPLEMENTED;
}

__declspec(dllexport) unixlib_entry_t __wine_unix_call_wow64_funcs[4096];
__declspec(dllexport) unixlib_entry_t __wine_unix_call_funcs[4096];

BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,void *reserved)
{
    (void)instance;(void)reserved;
    if(reason==DLL_PROCESS_ATTACH)
    {
        for(unsigned i=0;i<4096;i++)
            __wine_unix_call_wow64_funcs[i]=unavailable;
        __wine_unix_call_wow64_funcs[0]=process_attach;
        __wine_unix_call_wow64_funcs[1]=thread_attach;
        __wine_unix_call_wow64_funcs[2]=process_detach;
        for(unsigned i=0;i<4096;i++)
            __wine_unix_call_funcs[i]=__wine_unix_call_wow64_funcs[i];
    }
    return TRUE;
}
