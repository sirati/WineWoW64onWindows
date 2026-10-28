#ifndef PROBE_CPU_HOST_H
#define PROBE_CPU_HOST_H
#include <stdint.h>
#define PROBE_XSAVE_CAPACITY 16384
struct syscall_frame {
    XSAVE_FORMAT xsave __attribute__((aligned(64)));
    XSAVE_AREA_HEADER xstate;
    BYTE extended[PROBE_XSAVE_CAPACITY-sizeof(XSAVE_FORMAT)-sizeof(XSAVE_AREA_HEADER)];
    ULONG restore_flags;
    BOOL initialized;
};
/* Wine's CPU reserved flag immediately precedes I386_CONTEXT. The appended
 * bases belong to this invocation, allowing concurrent guest threads. */
struct guest_cpu {
    DWORD flags;
    I386_CONTEXT ctx;
    uintptr_t guest_fs_base,host_fs_base;
    uint64_t magic;
    uintptr_t xsave_ptr;
    struct syscall_frame frame;
    DWORD context_state;
} __attribute__((aligned(64)));
_Static_assert(offsetof(struct guest_cpu,ctx)==4,"Wine CPU flags layout");
_Static_assert(offsetof(struct guest_cpu,guest_fs_base)-offsetof(struct guest_cpu,ctx)==0x2cc,"guest FS offset");
_Static_assert(offsetof(struct guest_cpu,host_fs_base)-offsetof(struct guest_cpu,ctx)==0x2d4,"host FS offset");
_Static_assert(offsetof(struct guest_cpu,magic)-offsetof(struct guest_cpu,ctx)==0x2dc,"CPU cookie offset");
#define PROBE_CPU_MAGIC UINT64_C(0x57494e4543505531)
#define PROBE_CONTEXT_EXCEPTION 1u
#define PROBE_CONTEXT_SERVICE 2u
_Static_assert(offsetof(struct guest_cpu,xsave_ptr)-offsetof(struct guest_cpu,ctx)==0x2e4,"XSAVE pointer offset");
_Static_assert(offsetof(struct guest_cpu,frame.xsave)%64==0,"XSAVE alignment");
struct probe_shared_data { XSTATE_CONFIGURATION XState; };
extern struct probe_shared_data *user_shared_data;
extern UINT64 probe_xsave_mask,xstate_extended_features;
extern unsigned int xstate_size;
void probe_xstate_init(void);
void probe_cpu_prepare(struct guest_cpu *);
void copy_xstate(XSAVE_AREA_HEADER *,XSAVE_AREA_HEADER *,UINT64);
unsigned int xstate_get_size(UINT64,UINT64);
BOOL probe_context_xstate(CONTEXT *context,CONTEXT_EX **extended);
#endif
