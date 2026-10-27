#include "test_support.h"
static void fail_at(DWORD stage)
{ MESSAGE("thread_context32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static volatile DWORD ready,seen,release_worker __attribute__((used));
extern void context_landing(void);
static DWORD WINAPI context_worker(void *arg) __attribute__((naked));
static DWORD WINAPI context_worker(void *arg)
{
    __asm__ __volatile__("pushl %ebx\n\tmovl $0x13572468,%ebx\n\tmovl $1,_ready\n"
                         "1:\tpause\n\tcmpl $0,_release_worker\n\tje 1b\n"
                         ".globl _context_landing\n_context_landing:\n\tmovl %ebx,_seen\n\tpopl %ebx\n\txorl %eax,%eax\n\tret $4");
}
static void context_test(void)
{
    HANDLE worker=CreateThread(NULL,0,context_worker,NULL,0,NULL);REQUIRE(worker,10);
    for(unsigned i=0;i<3000&&!ready;i++)Sleep(1);REQUIRE(ready,11);
    REQUIRE(SuspendThread(worker)==0,12);
    static CONTEXT ctx;ctx.ContextFlags=CONTEXT_FULL|CONTEXT_DEBUG_REGISTERS;
    REQUIRE(GetThreadContext(worker,&ctx)&&ctx.Ebx==0x13572468,13);
    REQUIRE(ctx.SegCs==0x23&&ctx.SegFs!=0&&ctx.Esp,14);
    ctx.Ebx=0x24681357;ctx.Eip=(DWORD)(ULONG_PTR)context_landing;
    REQUIRE(SetThreadContext(worker,&ctx)&&ResumeThread(worker)==1,15);
    REQUIRE(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0&&seen==0x24681357,16);
    REQUIRE(CloseHandle(worker),17);
}
static DWORD (WINAPI *exchange)(DWORD);
static HANDLE start_event,check_event;
static volatile LONG initialized,waiting;
static DWORD tls_index;
static DWORD WINAPI tls_worker(void *opaque)
{
    DWORD value=(DWORD)(ULONG_PTR)opaque;
    InterlockedIncrement(&waiting);
    REQUIRE(WaitForSingleObject(start_event,3000)==WAIT_OBJECT_0,20);
    REQUIRE(exchange(value)==0&&exchange(value)==value,21);
    REQUIRE(TlsSetValue(tls_index,(void *)(ULONG_PTR)value),22);
    InterlockedIncrement(&initialized);
    REQUIRE(WaitForSingleObject(check_event,3000)==WAIT_OBJECT_0,23);
    REQUIRE(!TlsGetValue(tls_index)&&exchange(value)==value,24);
    return 0;
}
static void tls_test(void)
{
    HANDLE workers[8];DWORD indices[80];
    for(unsigned i=0;i<80;i++){indices[i]=TlsAlloc();REQUIRE(indices[i]!=TLS_OUT_OF_INDEXES,30);}
    tls_index=indices[79];REQUIRE(tls_index>=64,31);
    start_event=CreateEventA(NULL,TRUE,FALSE,NULL);check_event=CreateEventA(NULL,TRUE,FALSE,NULL);
    REQUIRE(start_event&&check_event,32);
    for(unsigned i=0;i<8;i++){workers[i]=CreateThread(NULL,0,tls_worker,(void *)(ULONG_PTR)(i+1),0,NULL);REQUIRE(workers[i],33);}
    /* Existing workers must have reached Wine's loader before LoadLibrary. */
    for(unsigned i=0;i<3000&&waiting!=8;i++)Sleep(1);REQUIRE(waiting==8,44);
    HMODULE library=LoadLibraryA("dynamic_tls_library32.dll");REQUIRE(library,34);
    exchange=(void *)GetProcAddress(library,"TlsExchange32");REQUIRE(exchange&&exchange(99)==0,35);
    REQUIRE(SetEvent(start_event),36);
    for(unsigned i=0;i<3000&&initialized!=8;i++)Sleep(1);REQUIRE(initialized==8,37);
    REQUIRE(TlsFree(tls_index),38);DWORD again=TlsAlloc();REQUIRE(again==tls_index,39);
    REQUIRE(SetEvent(check_event)&&WaitForMultipleObjects(8,workers,TRUE,3000)==WAIT_OBJECT_0,40);
    for(unsigned i=0;i<8;i++){DWORD code;REQUIRE(GetExitCodeThread(workers[i],&code)&&!code&&CloseHandle(workers[i]),41);}
    for(unsigned i=0;i<80;i++)REQUIRE(TlsFree(indices[i]),42);
    REQUIRE(FreeLibrary(library)&&CloseHandle(start_event)&&CloseHandle(check_event),43);
}
void WINAPI app_entry(void)
{
    context_test();tls_test();
    MESSAGE("thread_context32: remote suspend/get/set/resume, integer/control/debug context, dynamic static TLS on existing threads and process-wide TLS slot clearing passed\r\n");ExitProcess(0);
}
