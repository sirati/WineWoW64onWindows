#include "test_support.h"
static void fail_at(DWORD stage)
{ MESSAGE("thread_create32 failure stage/error: ");message_hex(stage);message_hex(GetLastError());ExitProcess(stage); }
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static volatile DWORD entered,parameter,identity,apc_count,apc_thread;
static void CALLBACK initial_apc(ULONG_PTR value)
{
    REQUIRE(!entered&&value==apc_count+1,26);
    apc_thread=GetCurrentThreadId();++apc_count;
}
static DWORD WINAPI worker(void *arg)
{
    if(!entered)REQUIRE(apc_count==4&&apc_thread==GetCurrentThreadId(),27);
    parameter=(DWORD)(ULONG_PTR)arg;identity=GetCurrentThreadId();InterlockedIncrement((LONG *)&entered);
    return parameter+7;
}
typedef LONG (WINAPI *create_thread_ex_fn)(HANDLE *,ACCESS_MASK,void *,HANDLE,void *,void *,ULONG,ULONG,SIZE_T,SIZE_T,void *);
static void check_exit(HANDLE thread,DWORD expected)
{
    DWORD code;
    REQUIRE(WaitForSingleObject(thread,3000)==WAIT_OBJECT_0&&GetExitCodeThread(thread,&code)&&code==expected,10);
    REQUIRE(CloseHandle(thread),11);
}
void WINAPI app_entry(void)
{
    SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};DWORD tid=0,flags=0;
    HANDLE thread=CreateThread(&security,128*1024,worker,(void *)9,CREATE_SUSPENDED|STACK_SIZE_PARAM_IS_A_RESERVATION,&tid);
    REQUIRE(thread&&tid&&GetThreadId(thread)==tid&&!entered,12);
    REQUIRE(GetHandleInformation(thread,&flags)&&(flags&HANDLE_FLAG_INHERIT),13);
    REQUIRE(SuspendThread(thread)==1&&ResumeThread(thread)==2&&!entered,14);
    static CONTEXT context;context.ContextFlags=CONTEXT_FULL;
    REQUIRE(GetThreadContext(thread,&context)&&context.Eip&&context.Esp,15);
    REQUIRE(SetThreadContext(thread,&context),16);
    for(DWORD i=1;i<=4;i++)REQUIRE(QueueUserAPC(initial_apc,thread,i),28);
    REQUIRE(!apc_count,29);
    REQUIRE(ResumeThread(thread)==1,17);check_exit(thread,16);
    REQUIRE(entered==1&&parameter==9&&identity==tid,18);
    create_thread_ex_fn create=(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtCreateThreadEx");REQUIRE(create,19);
    thread=NULL;
    REQUIRE(!create(&thread,THREAD_ALL_ACCESS,NULL,GetCurrentProcess(),worker,(void *)33,0,0,0,0,NULL)&&thread,20);
    check_exit(thread,40);REQUIRE(entered==2&&parameter==33,21);
    DWORD before,after;Sleep(30);REQUIRE(GetProcessHandleCount(GetCurrentProcess(),&before),22);
    for(unsigned i=0;i<64;i++)
    {
        thread=CreateThread(NULL,0,worker,(void *)1,CREATE_SUSPENDED,NULL);REQUIRE(thread,23);
        REQUIRE(QueueUserAPC(initial_apc,thread,99),30);
        REQUIRE(TerminateThread(thread,77),24);check_exit(thread,77);
    }
    Sleep(30);REQUIRE(GetProcessHandleCount(GetCurrentProcess(),&after)&&after<=before+4&&entered==2&&apc_count==4,25);
    MESSAGE("thread_create32: suspended creation, nested suspend count, native identity/inheritance, initial context/APC FIFO, direct NtCreateThreadEx, Wine thread return and cancelled-start cleanup passed\r\n");ExitProcess(0);
}
