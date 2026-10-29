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

/* The native companion is linked without a CRT entry point.  Static table
 * initialization therefore keeps the first Wine calls valid before any
 * PE-side attach callback could run. */
__declspec(dllexport) unixlib_entry_t __wine_unix_call_wow64_funcs[4]={
    process_attach, thread_attach, process_detach, unavailable
};
