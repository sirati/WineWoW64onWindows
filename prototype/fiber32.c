#include "test_support.h"
#include "stack_test.h"
static void *main_fiber;
static DWORD owner,fls,tls,destructors,expected_destructor;
struct fiber_state { DWORD value,calls,base; void *allocation; };
static void fail_at(DWORD stage)
{ MESSAGE("fiber32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static DWORD stack_base(void)
{ DWORD result;__asm__ volatile("movl %%fs:4,%0":"=r"(result));return result; }
static void WINAPI cleanup(void *value)
{ REQUIRE((DWORD)(ULONG_PTR)value==expected_destructor,30);++destructors; }
static void WINAPI fiber_entry(void *opaque)
{
    struct fiber_state *state=opaque;
    state->base=stack_base();REQUIRE(!FlsGetValue(fls),10);
    MEMORY_BASIC_INFORMATION memory;
    REQUIRE(VirtualQuery((void *)(ULONG_PTR)(state->base-4096),&memory,sizeof(memory)),27);
    state->allocation=memory.AllocationBase;
    REQUIRE(FlsSetValue(fls,(void *)(ULONG_PTR)state->value),11);
    consume_stack(256);REQUIRE(stack_test_checksum,12);
    for(;;)
    {
        REQUIRE(GetCurrentThreadId()==owner&&stack_base()==state->base,13);
        REQUIRE((DWORD)(ULONG_PTR)FlsGetValue(fls)==state->value,14);
        REQUIRE((DWORD)(ULONG_PTR)TlsGetValue(tls)==state->calls,15);
        ++state->calls;REQUIRE(TlsSetValue(tls,(void *)(ULONG_PTR)state->calls),16);
        SwitchToFiber(main_fiber);
    }
}
void WINAPI app_entry(void)
{
    owner=GetCurrentThreadId();DWORD base=stack_base();
    fls=FlsAlloc(cleanup);tls=TlsAlloc();REQUIRE(fls!=FLS_OUT_OF_INDEXES&&tls!=TLS_OUT_OF_INDEXES,17);
    main_fiber=ConvertThreadToFiber(NULL);REQUIRE(main_fiber,18);
    REQUIRE(FlsSetValue(fls,(void *)0x1234),19);
    for(unsigned i=0;i<8;i++)
    {
        struct fiber_state state={i+1,0,0};REQUIRE(TlsSetValue(tls,NULL),20);
        void *fiber=CreateFiberEx(4096,1024*1024,0,fiber_entry,&state);REQUIRE(fiber,21);
        for(unsigned n=1;n<=10;n++)
        {
            SwitchToFiber(fiber);REQUIRE(state.calls==n&&state.base!=base,22);
            REQUIRE(stack_base()==base&&(DWORD)(ULONG_PTR)FlsGetValue(fls)==0x1234,23);
            REQUIRE((DWORD)(ULONG_PTR)TlsGetValue(tls)==n,24);
        }
        expected_destructor=state.value;DeleteFiber(fiber);REQUIRE(destructors==i+1,25);
        MEMORY_BASIC_INFORMATION memory;
        REQUIRE(VirtualQuery(state.allocation,&memory,sizeof(memory))&&memory.State==MEM_FREE,28);
    }
    REQUIRE(ConvertFiberToThread()&&FlsSetValue(fls,NULL)&&FlsFree(fls)&&TlsFree(tls),26);
    MESSAGE("fiber32: Wine fiber allocation/switching, separate stacks and FLS, shared thread TLS and deletion callbacks passed\r\n");ExitProcess(0);
}
