#include "test_support.h"
/* Fault injection for this host adapter. Enter x64 briefly, clear the FS
 * base, and return to x86. The next TEB access must recover and retry. */
static __attribute__((naked,noinline)) void clear_fs_base(void)
{
    __asm__ __volatile__(
        ".byte 0xea\n\t.long 1f\n\t.word 0x33\n\t"
        ".code64\n1:\n\txorl %r11d,%r11d\n\twrfsbase %r11\n\t"
        "movl (%rsp),%r11d\n\taddq $4,%rsp\n\tpushq $0x23\n\tpushq %r11\n\tlretq\n\t.code32");
}
void WINAPI app_entry(void)
{
    DWORD self;__asm__ __volatile__("movl %%fs:0x18,%0":"=r"(self));
    for(unsigned i=0;i<100;i++)
    {
        DWORD after;clear_fs_base();
        __asm__ __volatile__("movl %%fs:0x18,%0":"=r"(after));
        if(after!=self) ExitProcess(36);
    }
    MESSAGE("fs_recovery32: 100 forced FS-base losses recovered and retried\r\n");ExitProcess(0);
}
