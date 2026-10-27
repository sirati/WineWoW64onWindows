#include <windows.h>
#include <stdint.h>
static void message(const char *s,DWORD n)
{
    DWORD written=0;
    if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&written,0)||written!=n) ExitProcess(70);
}
#define MESSAGE(s) message(s,sizeof(s)-1)
static void message_hex(DWORD value)
{
    char s[11];const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<8;i++)s[i]=hex[(value>>(28-i*4))&15];s[8]='\r';s[9]='\n';s[10]=0;
    message(s,10);
}
