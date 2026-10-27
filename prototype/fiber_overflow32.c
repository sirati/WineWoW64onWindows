/* SEH is compiled with the Microsoft x86 ABI, as in stack_overflow32. */
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned);
__declspec(dllimport) int __stdcall WriteFile(void *,const void *,unsigned,unsigned *,void *);
__declspec(dllimport) void *__stdcall ConvertThreadToFiber(void *);
__declspec(dllimport) int __stdcall ConvertFiberToThread(void);
__declspec(dllimport) void *__stdcall CreateFiberEx(unsigned,unsigned,unsigned,void (__stdcall *)(void *),void *);
__declspec(dllimport) void __stdcall SwitchToFiber(void *);
__declspec(dllimport) void __stdcall DeleteFiber(void *);
__declspec(dllimport) int _resetstkoflw(void);
unsigned long _exception_code(void);
static void *main_fiber;
static volatile unsigned checksum,caught;
__declspec(noinline) static void consume(unsigned depth)
{
    volatile unsigned char page[1024];
    for(unsigned i=0;i<sizeof(page);i++)page[i]=(unsigned char)(depth+i);
    if(depth)consume(depth-1);
    checksum+=page[depth&1023];
}
static void __stdcall body(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<2;i++)
    {
        unsigned handled=0;
        __try { consume(10000); }
        __except(_exception_code()==0xc00000fdUL?1:0) { handled=1;++caught; }
        if(!handled)ExitProcess(10);
        if(!_resetstkoflw())ExitProcess(11);
        consume(128);
    }
    SwitchToFiber(main_fiber);ExitProcess(12);
}
void app_entry(void)
{
    main_fiber=ConvertThreadToFiber(0);if(!main_fiber)ExitProcess(13);
    void *fiber=CreateFiberEx(4096,1024*1024,0,body,0);if(!fiber)ExitProcess(14);
    SwitchToFiber(fiber);if(caught!=2)ExitProcess(15);
    DeleteFiber(fiber);if(!ConvertFiberToThread())ExitProcess(16);
    const char text[]="fiber_overflow32: Wine fiber stack growth, repeated overflow, SEH unwind and CRT reset passed\r\n";
    unsigned written;
    if(!WriteFile(GetStdHandle((unsigned)-11),text,sizeof(text)-1,&written,0)||written!=sizeof(text)-1)ExitProcess(17);
    ExitProcess(0);
}
