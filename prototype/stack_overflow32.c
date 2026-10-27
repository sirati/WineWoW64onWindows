/* Microsoft x86 SEH output exercises Wine's overflow dispatcher and CRT
 * recovery on the guest stack, including a second overflow after reset. */
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned);
__declspec(dllimport) int __stdcall WriteFile(void *,const void *,unsigned,unsigned *,void *);
__declspec(dllimport) void *__stdcall CreateThread(void *,unsigned,unsigned (__stdcall *)(void *),void *,unsigned,unsigned *);
__declspec(dllimport) unsigned __stdcall WaitForSingleObject(void *,unsigned);
__declspec(dllimport) int __stdcall GetExitCodeThread(void *,unsigned *);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) int _resetstkoflw(void);
unsigned long _exception_code(void);
static volatile unsigned checksum;
__declspec(noinline) static void consume(unsigned depth)
{
    volatile unsigned char page[1024];
    for(unsigned i=0;i<sizeof(page);i++)page[i]=(unsigned char)(depth+i);
    if(depth)consume(depth-1);
    checksum+=page[depth&1023];
}
static unsigned __stdcall check(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<2;i++)
    {
        unsigned caught=0;
        __try { consume(10000); }
        __except(_exception_code()==0xc00000fdUL?1:0) { caught=1; }
        if(!caught)return 10;
        if(!_resetstkoflw())return 11;
        consume(128);
    }
    return 0;
}
void app_entry(void)
{
    unsigned code=check(0);
    if(code)ExitProcess(code);
    void *thread=CreateThread(0,0,check,0,0,0);
    if(!thread||WaitForSingleObject(thread,5000)||!GetExitCodeThread(thread,&code)||code||!CloseHandle(thread))ExitProcess(12);
    const char text[]="stack_overflow32: main and worker overflow, SEH unwind, CRT reset and repeated growth passed\r\n";
    unsigned written;
    if(!WriteFile(GetStdHandle((unsigned)-11),text,sizeof(text)-1,&written,0)||written!=sizeof(text)-1)ExitProcess(13);
    ExitProcess(0);
}
