/* Built for the Microsoft x86 C++ ABI; no replacement exception machinery. */
/* The MSVC compiler-support library normally supplies this RTTI anchor.
 * The test has no MSVC SDK. Throwing, matching and unwinding use msvcr100. */
class type_info { public: virtual ~type_info(); };
type_info::~type_info() {}
type_info compiler_support_anchor;
extern "C" __declspec(dllimport) void __stdcall ExitProcess(unsigned);
extern "C" __declspec(dllimport) void *__stdcall GetStdHandle(unsigned);
extern "C" __declspec(dllimport) int __stdcall WriteFile(void *,const void *,unsigned,unsigned *,void *);
extern "C" __declspec(dllimport) void *__stdcall CreateThread(void *,unsigned,unsigned (__stdcall *)(void *),void *,unsigned,unsigned *);
extern "C" __declspec(dllimport) unsigned __stdcall WaitForSingleObject(void *,unsigned);
extern "C" __declspec(dllimport) int __stdcall GetExitCodeThread(void *,unsigned *);
extern "C" __declspec(dllimport) int __stdcall CloseHandle(void *);
struct State { unsigned destroyed,order; };
struct Guard {
    State &state;unsigned id;
    Guard(State &s,unsigned value):state(s),id(value) {}
    ~Guard(){++state.destroyed;state.order=state.order*10+id;}
};
__declspec(noinline) static void raise_value(State &state){Guard inner(state,2);throw 42;}
static unsigned __stdcall verify(void *)
{
    unsigned caught=0;State state={0,0};
    try {Guard outer(state,1);raise_value(state);}
    catch(int value){if(value==42&&state.destroyed==2&&state.order==21)caught++;}
    try {
        try {Guard rethrow_guard(state,3);throw 17;}
        catch(int value){if(value!=17)return 72;throw;}
    } catch(int value){if(value==17&&state.destroyed==3&&state.order==213)caught++;}
    return caught==2?0:71;
}
extern "C" void app_entry()
{
    if(verify(0))ExitProcess(71);
    void *threads[4];
    for(unsigned i=0;i<4;i++)if(!(threads[i]=CreateThread(0,0,verify,0,0,0)))ExitProcess(74);
    for(unsigned i=0;i<4;i++)
    {
        unsigned code;
        if(WaitForSingleObject(threads[i],5000)||!GetExitCodeThread(threads[i],&code)||code)ExitProcess(75);
        if(!CloseHandle(threads[i]))ExitProcess(76);
    }
    const char message[]="cpp_unwind32: typed catches, rethrow and destructor order on main and workers passed\r\n";
    unsigned written=0;
    if(!WriteFile(GetStdHandle((unsigned)-11),message,sizeof(message)-1,&written,0)||written!=sizeof(message)-1)ExitProcess(73);
    ExitProcess(0);
}
