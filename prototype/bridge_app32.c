#include "test_support.h"
#include <stdlib.h>
#include <stdio.h>
__declspec(dllimport) BOOL WINAPI LegacyCompute(DWORD,uint64_t *);
void WINAPI app_entry(void)
{
    uint64_t *results=malloc(1000*sizeof(*results));if(!results)ExitProcess(87);
    for(unsigned i=0;i<1000;i++)
    {
        uint64_t out=0,expected=(uint64_t)i*UINT64_C(0x100000003)+7;
        if(!LegacyCompute(i,&out)||out!=expected) ExitProcess(86);
        results[i]=out;
    }
    for(unsigned i=0;i<1000;i++) if(results[i]!=(uint64_t)i*UINT64_C(0x100000003)+7)ExitProcess(88);
    free(results);
    if(puts("bridge_app32: Wine CRT and replacement x86 DLL made 1000 verified x64 DLL calls above 4 GiB")==EOF||fflush(stdout)==EOF)ExitProcess(89);
    ExitProcess(0);
}
