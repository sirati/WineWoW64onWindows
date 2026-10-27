/* Microsoft x86 SEH compiler output, dispatched and unwound by Wine. */
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned);
__declspec(dllimport) int __stdcall WriteFile(void *,const void *,unsigned,unsigned *,void *);
unsigned long _exception_code(void);
static volatile unsigned caught,finally_ran;
__declspec(noinline) static int divide_by_zero(void)
{ volatile int zero=0;return 42/zero; }
__declspec(noinline) static void fault_with_cleanup(void)
{
    __try { *(volatile unsigned *)0=1; }
    __finally { finally_ran++; }
}
void app_entry(void)
{
    __try { fault_with_cleanup(); }
    __except(_exception_code()==0xc0000005UL?1:0) { caught++; }
    __try { volatile int result=divide_by_zero();(void)result; }
    __except(_exception_code()==0xc0000094UL?1:0) { caught++; }
    if(caught!=2||finally_ran!=1)ExitProcess(81);
    const char text[]="seh_fault32: access violation, divide by zero and finally unwinding passed\r\n";
    unsigned written;
    if(!WriteFile(GetStdHandle((unsigned)-11),text,sizeof(text)-1,&written,0)||written!=sizeof(text)-1)ExitProcess(82);
    ExitProcess(0);
}
