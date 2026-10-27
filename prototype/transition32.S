/* Position-independent x86 entry gates and x64 trampolines.
 * Targets and far pointers live in writable image data. Executable pages
 * never need relocation or runtime patching, even above/below a new base.
 */
.text
.p2align 4
.globl _probe_gate_entries
_probe_gate_entries:
.code32
.macro gate number
    pushl %eax
    movl %fs:0xc0,%eax /* TEB32.WOW32Reserved names entry zero. */
    ljmp *_probe_gate_far+6*\number-_probe_gate_entries(%eax)
    .p2align 4,0x90
.endm
gate 0
gate 1
gate 2

.code64
.globl _probe_gate_trampolines
_probe_gate_trampolines:
.macro trampoline number
    movl (%rsp),%eax
    leaq 4(%rsp),%rsp
    jmpq *_probe_gate_targets+8*\number(%rip)
    .p2align 4,0x90
.endm
trampoline 0
trampoline 1
trampoline 2

.data
.p2align 3
.globl _probe_gate_far
_probe_gate_far:
    .space 18
.p2align 3
.globl _probe_gate_targets
_probe_gate_targets:
    .space 24
/* Retain a data-only base relocation so SEC_IMAGE may choose another base.
 * The runtime never uses this anchor. All executable references are relative. */
    .long _probe_gate_entries
