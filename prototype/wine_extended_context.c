/* Wine's architecture-independent CONTEXT_EX routines support the x86 and
 * x64 records used by its WoW64 dispatcher. Keep that conversion in Wine. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "cpu_host.h"
#define TRACE(...) do {} while(0)
#define FIXME(...) do {} while(0)
#define xstate_get_size wine_extended_xstate_size
#define RtlGetExtendedContextLength2 wine_RtlGetExtendedContextLength2
#define RtlGetExtendedContextLength wine_RtlGetExtendedContextLength
#define RtlInitializeExtendedContext2 wine_RtlInitializeExtendedContext2
#define RtlInitializeExtendedContext wine_RtlInitializeExtendedContext
#define RtlLocateExtendedFeature2 wine_RtlLocateExtendedFeature2
#define RtlLocateExtendedFeature wine_RtlLocateExtendedFeature
#define RtlLocateLegacyContext wine_RtlLocateLegacyContext
#define RtlSetExtendedFeaturesMask wine_RtlSetExtendedFeaturesMask
#define RtlGetExtendedFeaturesMask wine_RtlGetExtendedFeaturesMask
#define RtlCopyContext wine_RtlCopyContext
#define RtlCopyExtendedContext wine_RtlCopyExtendedContext
NTSTATUS WINAPI wine_RtlCopyExtendedContext(CONTEXT_EX *,ULONG,CONTEXT_EX *);
#include "out/wine-extended-context.inc"
