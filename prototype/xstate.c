/* Storage for Wine's original XSAVE layout/copy helpers. The native shared
 * configuration describes the same processor and Windows context format. */
#include <stdio.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "ddk/wdm.h"
#include <cpuid.h>
#include "cpu_host.h"
static struct probe_shared_data shared;
struct probe_shared_data *user_shared_data=&shared;
UINT64 probe_xsave_mask,xstate_extended_features;
unsigned int xstate_size=sizeof(XSAVE_AREA_HEADER);
#include "out/wine-xstate.inc"
void probe_xstate_init(void)
{
    unsigned int a,b,c,d;
    __cpuid(1,a,b,c,d);
    if((c&((1u<<26)|(1u<<27)))!=((1u<<26)|(1u<<27)))return;
    unsigned int lo,hi;
    __asm__("xgetbv":"=a"(lo),"=d"(hi):"c"(0));
    probe_xsave_mask=RtlGetEnabledExtendedFeatures(~(UINT64)0)&(((UINT64)hi<<32)|lo);
    shared.XState=((const KUSER_SHARED_DATA *)0x7ffe0000)->XState;
    shared.XState.EnabledFeatures=probe_xsave_mask;
    unsigned int size=sizeof(XSAVE_FORMAT)+sizeof(XSAVE_AREA_HEADER);
    for(unsigned i=2;i<63;i++)if(probe_xsave_mask&((UINT64)1<<i))
    {
        __cpuid_count(0xd,i,a,b,c,d);
        if(!a||b<576||b+a>PROBE_XSAVE_CAPACITY){fprintf(stderr,"Unsupported XSAVE component %u size=%u offset=%u\n",i,a,b);ExitProcess(94);}
        shared.XState.Features[i].Offset=b;shared.XState.Features[i].Size=a;
        if(b+a>size)size=b+a;
    }
    xstate_size=size-sizeof(XSAVE_FORMAT);
    xstate_extended_features=probe_xsave_mask&~(UINT64)3;
    printf("XSTATE mask=%llx size=%u compact-contexts=%u\n",probe_xsave_mask,size,shared.XState.CompactionEnabled);
}
void probe_cpu_prepare(struct guest_cpu *cpu)
{
    /* Wine can query a suspended child's context before its first entry. */
    ((WOW64_CPURESERVED *)cpu)->Machine=IMAGE_FILE_MACHINE_I386;
    cpu->xsave_ptr=(uintptr_t)&cpu->frame.xsave;
    if(cpu->frame.initialized)return;
    memset(&cpu->frame,0,sizeof(cpu->frame));
    cpu->frame.xsave.ControlWord=0x27f;cpu->frame.xsave.MxCsr=0x1f80;
    cpu->frame.xsave.MxCsr_Mask=0xffff;
    cpu->frame.xstate.Mask=3;
    cpu->frame.initialized=TRUE;
}
BOOL probe_context_xstate(CONTEXT *context,CONTEXT_EX **extended)
{
    if((context->ContextFlags&CONTEXT_XSTATE)!=CONTEXT_XSTATE)return FALSE;
    *extended=(CONTEXT_EX *)(context+1);
    return TRUE;
}
