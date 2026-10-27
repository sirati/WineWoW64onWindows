#include "test_support.h"
void WINAPI app_entry(void)
{
    HANDLE heap=GetProcessHeap();DWORD idx=TlsAlloc();
    if(!heap||idx==TLS_OUT_OF_INDEXES) ExitProcess(40);
    if(!TlsSetValue(idx,(void *)0x12345678)||TlsGetValue(idx)!=(void *)0x12345678) ExitProcess(41);
    for(unsigned n=1;n<1000;n++)
    {
        BYTE *p=HeapAlloc(heap,HEAP_ZERO_MEMORY,n);if(!p) ExitProcess(42);
        for(unsigned i=0;i<n;i++) { if(p[i])ExitProcess(43);p[i]=(BYTE)i; }
        p=HeapReAlloc(heap,HEAP_ZERO_MEMORY,p,n+100);if(!p) ExitProcess(44);
        for(unsigned i=0;i<n+100;i++) if(p[i]!=(i<n?(BYTE)i:0)) ExitProcess(45);
        if(!HeapFree(heap,0,p)) ExitProcess(46);
    }
    MESSAGE("heap32: Wine heap alloc/realloc/free and dynamic TLS get/set passed\r\n");ExitProcess(0);
}
