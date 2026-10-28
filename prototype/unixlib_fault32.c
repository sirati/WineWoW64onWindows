#include "test_support.h"
typedef LONG (WINAPI *dispatch_t)(ULONGLONG,ULONG,void *);
void WINAPI app_entry(void)
{
    dispatch_t *dispatcher=(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),"__wine_unix_call_dispatcher");
    if(!dispatcher||!*dispatcher){MESSAGE("UNIXLIB_FAULT_FAILED missing-dispatcher\r\n");ExitProcess(77);}
    static const ULONGLONG handles[]={0,0x10,~(ULONGLONG)0};
    MESSAGE("UNIXLIB_FAULT_BEGIN\r\n");
    for(unsigned i=0;i<sizeof(handles)/sizeof(handles[0]);i++)
    {
        LONG status=(*dispatcher)(handles[i],0,NULL);
        MESSAGE("fault.index=");message_hex(i);
        MESSAGE("fault.status=");message_hex(status);
        if(status!=(LONG)0xc0000005){MESSAGE("UNIXLIB_FAULT_FAILED unexpected-status\r\n");ExitProcess(78);}
        if(!GetCurrentProcessId())ExitProcess(79);
        Sleep(0);
    }
    MESSAGE("unixlib_fault32: invalid tables return access-violation status and guest execution continues passed\r\n");
    ExitProcess(0);
}
