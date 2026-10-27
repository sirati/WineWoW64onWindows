#include "test_support.h"
static volatile DWORD callback_seen;
static volatile DWORD tls_index;
static const DWORD tls_data[]={0x12345678,0xabcdef01,0,0};
static void WINAPI tls_callback(void *module,DWORD reason,void *reserved)
{ if(module&&reason==DLL_PROCESS_ATTACH&&!reserved&&callback_seen==0)callback_seen=1; }
static void WINAPI tls_callback_second(void *module,DWORD reason,void *reserved)
{ if(module&&reason==DLL_PROCESS_ATTACH&&!reserved&&callback_seen==1)callback_seen=2; }
static PIMAGE_TLS_CALLBACK callbacks[]={tls_callback,tls_callback_second,0};
const IMAGE_TLS_DIRECTORY32 _tls_used={
    (DWORD)(uintptr_t)&tls_data,(DWORD)(uintptr_t)(&tls_data+1),
    (DWORD)(uintptr_t)&tls_index,(DWORD)(uintptr_t)callbacks,0,0
};
void WINAPI app_entry(void)
{
    DWORD **slots,*data;
    if(callback_seen!=2) { MESSAGE("tls32: PE TLS callback order failed\r\n");ExitProcess(32); }
    __asm__ __volatile__("movl %%fs:0x2c,%0":"=r"(slots));
    data=slots[tls_index];
    if(data[0]!=0x12345678||data[1]!=0xabcdef01||data[2]||data[3])
    { MESSAGE("TLS index/data: ");message_hex(tls_index);for(unsigned i=0;i<4;i++)message_hex(data[i]);ExitProcess(34); }
    MESSAGE("tls32: callback order and static TLS template passed\r\n");ExitProcess(0);
}
