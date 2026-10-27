#include <windows.h>
#include <stdint.h>
static DWORD tls_index;
static const DWORD initial[]={0x1234abcd,0};
const IMAGE_TLS_DIRECTORY32 _tls_used={
    (DWORD)(uintptr_t)initial,(DWORD)(uintptr_t)(initial+2),
    (DWORD)(uintptr_t)&tls_index,0,0,0
};
__declspec(dllexport) DWORD WINAPI TlsExchange32(DWORD value)
{
    DWORD **slots;__asm__ __volatile__("movl %%fs:0x2c,%0":"=r"(slots));
    DWORD *data=slots[tls_index];
    if(!data||data[0]!=0x1234abcd)return 0xffffffff;
    DWORD previous=data[1];data[1]=value;return previous;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,void *reserved)
{ (void)instance;(void)reason;(void)reserved;return TRUE; }
