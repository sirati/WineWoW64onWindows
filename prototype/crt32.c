#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void WINAPI app_entry(void)
{
    char *p=malloc(256);if(!p)ExitProcess(81);
    memset(p,0x5a,256);p=realloc(p,512);if(!p)ExitProcess(82);
    for(unsigned i=0;i<256;i++)if(p[i]!=0x5a)ExitProcess(83);
    free(p);
    if(puts("crt32: Wine malloc/realloc/free and ordinary CRT output passed")==EOF)ExitProcess(84);
    if(fflush(stdout)==EOF)ExitProcess(85);
    ExitProcess(0);
}
