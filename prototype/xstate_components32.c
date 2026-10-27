#include "test_support.h"
#ifndef CONTEXT_XSTATE
#define CONTEXT_XSTATE (CONTEXT_i386|0x40)
#endif
static volatile DWORD phase,command __attribute__((used));
static void fail_at(DWORD stage)
{ MESSAGE("xstate_components32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static DWORD WINAPI worker(void *unused) __attribute__((naked));
static DWORD WINAPI worker(void *unused)
{
    __asm__ volatile("movl $1,_phase\n"
        "1:\tpause\n\tcmpl $1,_command\n\tjb 1b\n\tmovl $2,_phase\n"
        "2:\tpause\n\tcmpl $2,_command\n\tjb 2b\n\txorl %eax,%eax\n\tret $4");
}
static BYTE value(unsigned feature,unsigned offset)
{ return (BYTE)(0x31+feature*13+offset*7); }
static CONTEXT *allocate_context(void **storage)
{
    DWORD length=0,flags=CONTEXT_FULL|CONTEXT_FLOATING_POINT|CONTEXT_EXTENDED_REGISTERS|CONTEXT_XSTATE;
    CONTEXT *context=NULL;
    InitializeContext(NULL,flags,&context,&length);REQUIRE(length,10);
    *storage=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,length+64);REQUIRE(*storage,11);
    REQUIRE(InitializeContext(*storage,flags,&context,&length),12);
    return context;
}
static void await_phase(DWORD expected)
{ for(unsigned i=0;i<3000&&phase!=expected;i++)Sleep(1);REQUIRE(phase==expected,13); }
void WINAPI app_entry(void)
{
    /* These components contain vector/mask bytes with no privileged control
     * bits. Other feature types need their own architectural validation. */
    const unsigned features[]={2,5,6,7};
    DWORD64 enabled=GetEnabledXStateFeatures(),mask=enabled&0xe4;
    MESSAGE("xstate_components32 enabled/requested masks:\r\n");message_hex((DWORD)enabled);message_hex((DWORD)mask);
    REQUIRE(mask&4,14);
    void *first_storage,*second_storage;
    CONTEXT *first=allocate_context(&first_storage),*second=allocate_context(&second_storage);
    REQUIRE(SetXStateFeaturesMask(first,mask)&&SetXStateFeaturesMask(second,mask),15);
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);REQUIRE(thread,16);
    await_phase(1);REQUIRE(SuspendThread(thread)==0&&GetThreadContext(thread,first),17);
    for(unsigned f=0;f<sizeof(features)/sizeof(features[0]);f++)if(mask&((DWORD64)1<<features[f]))
    {
        DWORD length=0;BYTE *bytes=LocateXStateFeature(first,features[f],&length);
        REQUIRE(bytes&&length,18);
        MESSAGE("xstate component/length:\r\n");message_hex(features[f]);message_hex(length);
        for(unsigned i=0;i<length;i++)bytes[i]=value(features[f],i);
    }
    REQUIRE(SetXStateFeaturesMask(first,mask)&&SetThreadContext(thread,first),19);
    command=1;REQUIRE(ResumeThread(thread)==1,20);await_phase(2);
    REQUIRE(SuspendThread(thread)==0&&GetThreadContext(thread,second),21);
    DWORD64 returned=0;REQUIRE(GetXStateFeaturesMask(second,&returned)&&(returned&mask)==mask,22);
    for(unsigned f=0;f<sizeof(features)/sizeof(features[0]);f++)if(mask&((DWORD64)1<<features[f]))
    {
        DWORD length=0;BYTE *bytes=LocateXStateFeature(second,features[f],&length);REQUIRE(bytes&&length,23);
        for(unsigned i=0;i<length;i++)if(bytes[i]!=value(features[f],i))
        { MESSAGE("xstate mismatch feature/offset/actual/expected:\r\n");message_hex(features[f]);message_hex(i);message_hex(bytes[i]);message_hex(value(features[f],i));fail_at(24); }
    }
    command=2;REQUIRE(ResumeThread(thread)==1&&WaitForSingleObject(thread,3000)==WAIT_OBJECT_0,25);
    REQUIRE(CloseHandle(thread)&&HeapFree(GetProcessHeap(),0,first_storage)&&HeapFree(GetProcessHeap(),0,second_storage),26);
    MESSAGE("xstate_components32: all enabled AVX, opmask, ZMM-high and high-ZMM components survived set/resume/suspend/get passed\r\n");ExitProcess(0);
}
