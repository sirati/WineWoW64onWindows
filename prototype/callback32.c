#include "test_support.h"
__declspec(dllimport) DWORD WINAPI LegacyCallbacks(void *,void *,DWORD);
struct callback_state { DWORD calls,depth,thread; };
static DWORD avx_seed[4]={1,2,3,4};
static DWORD avx_observed[4];
static DWORD WINAPI guest_callback(struct callback_state *state,DWORD value)
{
    if(GetCurrentThreadId()!=state->thread)ExitProcess(211);
    __asm__ volatile("pxor %%xmm6,%%xmm6":::"xmm6");
    state->calls++;
    if(state->depth)
    {
        struct callback_state nested={0,state->depth-1,state->thread};
        DWORD sum=LegacyCallbacks(guest_callback,&nested,3);
        if(sum!=6||nested.calls!=3)ExitProcess(212);
    }
    return value+1;
}
void WINAPI app_entry(void)
{
    __asm__ volatile("movdqu %0,%%xmm6"::"m"(avx_seed):"xmm6");
    struct callback_state state={0,3,GetCurrentThreadId()};
    DWORD result=LegacyCallbacks(guest_callback,&state,100);
    if(result!=5050||state.calls!=100)ExitProcess(213);
    __asm__ volatile("movdqu %%xmm6,%0":"=m"(avx_observed));
    for(unsigned i=0;i<4;i++)if(avx_observed[i]!=avx_seed[i])ExitProcess(214);
    MESSAGE("callback32: native x64 DLL calls guest callbacks, arguments, return values and three nested levels passed\r\n");
    ExitProcess(0);
}
