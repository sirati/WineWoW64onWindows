/* Image-backed compatibility-mode instructions for the continuation helper. */
.text
.code32
.globl _diagnostic_entry
_diagnostic_entry:
    nop
    int3
    ud2
