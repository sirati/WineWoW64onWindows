/* The syscall instructions are the unchanged Wine 11 syscall macros. */
#include <windef.h>
#include <winnt.h>
#include "wine/asm.h"
#include "shared.h"
void *__wine_syscall_dispatcher;
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,void *reserved) { (void)h;(void)reason;(void)reserved;return TRUE; }
__ASM_SYSCALL_FUNC(0x0008,NtWriteFile,36)
__ASM_SYSCALL_FUNC(0x000f,NtClose,4)
__ASM_SYSCALL_FUNC(0x0018,NtAllocateVirtualMemory,24)
__ASM_SYSCALL_FUNC(0x0025,NtQueryInformationThread,20)
__ASM_SYSCALL_FUNC(0x001e,NtFreeVirtualMemory,16)
__ASM_SYSCALL_FUNC(0x0034,NtDelayExecution,8)
__ASM_SYSCALL_FUNC(0x0004,NtWaitForSingleObject,12)
__ASM_SYSCALL_FUNC(0x0055,NtCreateFile,44)
__ASM_SYSCALL_FUNC(0x008d,NtDeleteFile,4)
DEFINE_SYSCALL_HELPER32()
