/* Static module TLS and Win32 TLS slots must have separate storage. */
#include "test_support.h"
static DWORD static_index,dynamic_index;
static const DWORD initial[]={0x1234abcd,0};
const IMAGE_TLS_DIRECTORY32 _tls_used={
    (DWORD)(uintptr_t)initial,(DWORD)(uintptr_t)(initial+2),
    (DWORD)(uintptr_t)&static_index,0,0,0
};
typedef DWORD (WINAPI *exchange_fn)(DWORD);
static exchange_fn exchange;
static DWORD *static_data(void)
{
    DWORD **slots;
    __asm__ __volatile__("movl %%fs:0x2c,%0":"=r"(slots));
    return slots?slots[static_index]:0;
}
static DWORD WINAPI worker(void *unused)
{
    DWORD *data=static_data();(void)unused;
    if(!data||data[0]!=initial[0]||data[1]||TlsGetValue(0)||TlsGetValue(dynamic_index))return 81;
    data[1]=0x2222;
    if(!TlsSetValue(dynamic_index,(void *)0x4444)||exchange(0x6666)!=0||exchange(0)!=0x6666)return 82;
    if(data[1]!=0x2222||TlsGetValue(0)||TlsGetValue(dynamic_index)!=(void *)0x4444)return 83;
    return 0;
}
void WINAPI app_entry(void)
{
    DWORD *data=static_data(),code;
    if(!data||data[0]!=initial[0]||data[1]||TlsGetValue(0))ExitProcess(71);
    dynamic_index=TlsAlloc();
    if(!dynamic_index||dynamic_index==TLS_OUT_OF_INDEXES||TlsGetValue(dynamic_index))ExitProcess(72);
    data[1]=0x1111;
    if(!TlsSetValue(dynamic_index,(void *)0x3333))ExitProcess(73);
    HMODULE module=LoadLibraryA("dynamic_tls_library32.dll");
    if(!module)ExitProcess(74);
    exchange=(exchange_fn)GetProcAddress(module,"TlsExchange32");
    if(!exchange||exchange(0x5555)!=0||TlsGetValue(0))ExitProcess(75);
    HANDLE thread=CreateThread(0,0,worker,0,0,0);
    if(!thread||WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0||
       !GetExitCodeThread(thread,&code)||code)ExitProcess(76);
    if(data[1]!=0x1111||TlsGetValue(0)||TlsGetValue(dynamic_index)!=(void *)0x3333||exchange(0)!=0x5555)
        ExitProcess(77);
    if(!CloseHandle(thread)||!FreeLibrary(module)||!TlsFree(dynamic_index))ExitProcess(78);
    MESSAGE("tls_slots32: static and dynamic TLS storage stays separate across loading and threads passed\r\n");
    ExitProcess(0);
}
