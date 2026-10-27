#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "shared.h"
#include "wine/unixlib.h"
__declspec(dllexport) NTSTATUS (WINAPI *__wine_unix_call_dispatcher)(unixlib_handle_t,unsigned int,void *);
__declspec(dllexport) uint64_t native_handle;
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,void *reserved)
{ (void)h;(void)reason;(void)reserved;return TRUE; }
/* This is the ABI-compatible x86 front of the replacement DLL. The work is
 * in native64.dll and uses Wine's existing Unix-call transition protocol. */
__declspec(dllexport) BOOL WINAPI LegacyCompute(DWORD input,uint64_t *output)
{
    struct bridge_args a={input,0,0,0};
    if(__wine_unix_call(native_handle,BRIDGE_COMPUTE,&a)<0||!a.extra) return FALSE;
    *output=((uint64_t)a.high<<32)|a.result;return TRUE;
}
__declspec(dllexport) DWORD WINAPI LegacyCallbacks(void *callback,void *context,DWORD count)
{
    struct bridge_args args={(DWORD)(ULONG_PTR)callback,0,(DWORD)(ULONG_PTR)context,count};
    if(__wine_unix_call(native_handle,BRIDGE_CALLBACK,&args)<0)return 0xffffffffu;
    return args.result;
}
__declspec(dllexport) DWORD WINAPI LegacyWorkerCallbacks(void *callback,void *context,DWORD count)
{
    struct bridge_args args={(DWORD)(ULONG_PTR)callback,0,(DWORD)(ULONG_PTR)context,count};
    if(__wine_unix_call(native_handle,BRIDGE_WORKER_CALLBACK,&args)<0)return 0xffffffffu;
    return args.result;
}
