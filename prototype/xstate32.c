#include "test_support.h"
#ifndef CONTEXT_XSTATE
#define CONTEXT_XSTATE (CONTEXT_i386|0x40)
#endif
static void fail_at(DWORD stage)
{ MESSAGE("xstate32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static DWORD seed[8] __attribute__((aligned(32),used))={1,2,3,4,5,6,7,8};
static DWORD changed[8] __attribute__((aligned(32),used))={1,2,3,4,9,10,11,12};
static DWORD observed[8] __attribute__((aligned(32),used));
static volatile DWORD ready,release_worker,apcs,simple_apcs,exceptions __attribute__((used));
static void CALLBACK simple_apc(ULONG_PTR ignored) { (void)ignored;++simple_apcs; }
static void load_state(void) { __asm__ volatile("vmovdqu %0,%%ymm6"::"m"(seed):"xmm6"); }
static void save_state(void) { __asm__ volatile("vmovdqu %%ymm6,%0":"=m"(observed)); }
static BOOL equal(const void *a,const void *b,SIZE_T n)
{ const BYTE *x=a,*y=b;for(SIZE_T i=0;i<n;i++)if(x[i]!=y[i])return FALSE;return TRUE; }
static void CALLBACK apc(ULONG_PTR ignored)
{ (void)ignored;++apcs;__asm__ volatile("vmovdqu %0,%%ymm6"::"m"(changed):"xmm6"); }
static LONG CALLBACK exception_handler(EXCEPTION_POINTERS *p)
{
    if(p->ExceptionRecord->ExceptionCode!=EXCEPTION_ILLEGAL_INSTRUCTION)return EXCEPTION_CONTINUE_SEARCH;
    DWORD length=0;BYTE *avx=LocateXStateFeature(p->ContextRecord,2,&length);
    if(!avx||length<128||!equal(avx+6*16,seed+4,16))
    {
        MESSAGE("xstate exception flags/length/address/high6:\r\n");message_hex(p->ContextRecord->ContextFlags);message_hex(length);message_hex((DWORD)(ULONG_PTR)avx);
        DWORD *ex=(DWORD *)(p->ContextRecord+1);
        MESSAGE("guest CONTEXT_EX all/legacy/xstate offset+length and XSAVE header:\r\n");
        for(unsigned i=0;i<6;i++)message_hex(ex[i]);
        DWORD *xs=(DWORD *)((BYTE *)ex+(LONG)ex[4]);
        for(unsigned i=0;i<4;i++)message_hex(xs[i]);
        if(avx&&length>=128)for(unsigned i=0;i<4;i++)message_hex(((DWORD *)(avx+6*16))[i]);
    }
    REQUIRE(avx&&length>=128&&equal(avx+6*16,seed+4,16),20);
    for(unsigned i=0;i<16;i++)avx[6*16+i]=((BYTE *)(changed+4))[i];
    REQUIRE(SetXStateFeaturesMask(p->ContextRecord,4),21);
    ++exceptions;p->ContextRecord->Eip+=2;return EXCEPTION_CONTINUE_EXECUTION;
}
static DWORD WINAPI worker(void *arg) __attribute__((naked));
static DWORD WINAPI worker(void *arg)
{
    __asm__ volatile("vmovdqu _seed,%ymm6\n\tmovl $1,_ready\n"
                     "1:\tpause\n\tcmpl $0,_release_worker\n\tje 1b\n"
                     "vmovdqu %ymm6,_observed\n\txorl %eax,%eax\n\tret $4");
}
void WINAPI app_entry(void)
{
    REQUIRE(GetEnabledXStateFeatures()&4,10);
    load_state();for(unsigned i=0;i<100;i++)SleepEx(0,FALSE);save_state();REQUIRE(equal(seed,observed,32),12);
    REQUIRE(QueueUserAPC(simple_apc,GetCurrentThread(),0),13);
    load_state();REQUIRE(SleepEx(0,TRUE)==WAIT_IO_COMPLETION,14);REQUIRE(simple_apcs==1,15);
    REQUIRE(QueueUserAPC(apc,GetCurrentThread(),0),16);
    load_state();REQUIRE(SleepEx(0,TRUE)==WAIT_IO_COMPLETION,17);save_state();REQUIRE(apcs==1,18);
    MESSAGE("xstate APC observed/seed/changed first dwords:\r\n");message_hex(observed[0]);message_hex(seed[0]);message_hex(changed[0]);
    REQUIRE(equal(seed,observed,32)||equal(changed,observed,32),141);
    void *veh=AddVectoredExceptionHandler(1,exception_handler);REQUIRE(veh,19);
    load_state();__asm__ volatile("ud2");save_state();
    REQUIRE(exceptions==1&&equal(changed,observed,32)&&RemoveVectoredExceptionHandler(veh),20);
    DWORD flags=CONTEXT_FULL|CONTEXT_FLOATING_POINT|CONTEXT_EXTENDED_REGISTERS|CONTEXT_XSTATE,length=0;
    CONTEXT *context=NULL;InitializeContext(NULL,flags,&context,&length);REQUIRE(length,30);
    void *storage=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,length+64);REQUIRE(storage,31);
    REQUIRE(InitializeContext(storage,flags,&context,&length)&&SetXStateFeaturesMask(context,4),32);
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);REQUIRE(thread,33);
    for(unsigned i=0;i<3000&&!ready;i++)Sleep(1);REQUIRE(ready&&SuspendThread(thread)==0,34);
    REQUIRE(GetThreadContext(thread,context),35);
    DWORD size=0;BYTE *avx=LocateXStateFeature(context,2,&size);
    REQUIRE(avx&&size>=128&&equal(avx+6*16,seed+4,16),36);
    for(unsigned i=0;i<16;i++)avx[6*16+i]=((BYTE *)(changed+4))[i];
    REQUIRE(SetThreadContext(thread,context),37);release_worker=1;
    REQUIRE(ResumeThread(thread)==1&&WaitForSingleObject(thread,3000)==WAIT_OBJECT_0,38);
    if(!equal(changed,observed,32))
    {
        MESSAGE("remote XSTATE observed then expected:\r\n");
        for(unsigned i=0;i<8;i++)message_hex(observed[i]);
        for(unsigned i=0;i<8;i++)message_hex(changed[i]);
    }
    REQUIRE(equal(changed,observed,32),39);
    REQUIRE(CloseHandle(thread)&&HeapFree(GetProcessHeap(),0,storage),40);
    MESSAGE("xstate32: AVX across 100 transitions, APC preservation, exception context edits and remote XSTATE get/set passed\r\n");ExitProcess(0);
}
