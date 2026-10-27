/* The implementation below is the complete Wine 11 wow64_NtClose thunk.
 * It is kept in a separate translation unit so the full syscall table does
 * not force every unimplemented service into this small native-Windows host. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "../wine-11.0-source/dlls/wow64/wow64_private.h"
#include "native_io.h"
#define NtClose probe_NtClose
extern BOOLEAN (WINAPI *pBTCpuIsProcessorFeaturePresent)(UINT);

#include "out/wine-base-thunks.inc"
