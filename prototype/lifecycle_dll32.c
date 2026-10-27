/* Static TLS, loader notifications and FLS cleanup are all Wine-owned. */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdint.h>
static volatile LONG tls_attach,tls_detach,dll_attach,dll_detach,fls_cleanup,errors;
static DWORD fls_index=FLS_OUT_OF_INDEXES;
static DWORD tls_index;
static const DWORD tls_template[]={0x1234abcd,0,0};
static DWORD *thread_data(void)
{
    DWORD **slots;
    __asm__ __volatile__("movl %%fs:0x2c,%0":"=r"(slots));
    return slots[tls_index];
}
static void WINAPI tls_callback(void *module,DWORD reason,void *reserved)
{
    DWORD *data=thread_data();
    (void)module;(void)reserved;
    if(reason==DLL_PROCESS_ATTACH||reason==DLL_THREAD_ATTACH)
    {
        if(data[0]!=0x1234abcd||data[1]||data[2])InterlockedIncrement(&errors);
        data[2]=1;
        if(reason==DLL_THREAD_ATTACH)InterlockedIncrement(&tls_attach);
    }
    if(reason==DLL_THREAD_DETACH)
    {
        if(!data[1]||data[2]!=2)InterlockedIncrement(&errors);
        InterlockedIncrement(&tls_detach);
    }
}
static PIMAGE_TLS_CALLBACK callbacks[]={tls_callback,0};
const IMAGE_TLS_DIRECTORY32 _tls_used={
    (DWORD)(uintptr_t)tls_template,(DWORD)(uintptr_t)(tls_template+3),
    (DWORD)(uintptr_t)&tls_index,(DWORD)(uintptr_t)callbacks,0,0
};
static void WINAPI cleanup(void *value)
{
    if((DWORD)(uintptr_t)value!=thread_data()[1])InterlockedIncrement(&errors);
    InterlockedIncrement(&fls_cleanup);
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,void *reserved)
{
    (void)instance;(void)reserved;
    if(reason==DLL_PROCESS_ATTACH||reason==DLL_THREAD_ATTACH)
    {
        if(thread_data()[2]!=1)InterlockedIncrement(&errors);
        thread_data()[2]=2;
        if(reason==DLL_PROCESS_ATTACH)
        {
            fls_index=FlsAlloc(cleanup);
            return fls_index!=FLS_OUT_OF_INDEXES;
        }
        InterlockedIncrement(&dll_attach);
    }
    if(reason==DLL_THREAD_DETACH)InterlockedIncrement(&dll_detach);
    if(reason==DLL_PROCESS_DETACH&&fls_index!=FLS_OUT_OF_INDEXES)FlsFree(fls_index);
    return TRUE;
}
__declspec(dllexport) DWORD WINAPI LifecycleSet(DWORD value)
{
    DWORD *data=thread_data();
    if(data[0]!=0x1234abcd||data[1]||data[2]!=2||FlsGetValue(fls_index))return 1;
    data[1]=value;
    return FlsSetValue(fls_index,(void *)(uintptr_t)value)?0:2;
}
__declspec(dllexport) DWORD WINAPI LifecycleGet(void) { return thread_data()[1]; }
__declspec(dllexport) DWORD WINAPI LifecycleCounts(DWORD expected)
{
    return errors||tls_attach!=expected||tls_detach!=expected||dll_attach!=expected||
           dll_detach!=expected||fls_cleanup!=expected;
}
