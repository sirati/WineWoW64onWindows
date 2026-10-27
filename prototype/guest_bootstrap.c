/* Native Windows bootstrap into Wine 11's original PE loader lifecycle. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "out/wine-ntdll-symbols.h"
#define TRACE(...) do {} while(0)
#define TRACE_(channel) TRACE
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void *reserved)
{ (void)module;(void)reason;(void)reserved;return TRUE; }
__declspec(dllexport) void WINAPI ProbeInitRuntime(void)
{
    /* Same NLS and TLS bitmap initialization used by Wine's locale/loader
     * startup. NLS tables and process parameters come from the Windows host. */
    PEB *peb=NtCurrentTeb()->Peb;
    static RTL_BITMAP tls_bitmap,tls_expansion_bitmap;
    static RTL_CRITICAL_SECTION peb_lock;
    NLSTABLEINFO nls;
    RtlInitNlsTables(peb->AnsiCodePageData,peb->OemCodePageData,peb->UnicodeCaseTableData,&nls);
    RtlResetRtlTranslations(&nls);
    RtlInitializeCriticalSection(&peb_lock);peb->FastPebLock=&peb_lock;
    peb->TlsBitmap=&tls_bitmap;peb->TlsExpansionBitmap=&tls_expansion_bitmap;
    RtlInitializeBitMap(&tls_bitmap,peb->TlsBitmapBits,sizeof(peb->TlsBitmapBits)*8);
    RtlInitializeBitMap(&tls_expansion_bitmap,peb->TlsExpansionBitmapBits,sizeof(peb->TlsExpansionBitmapBits)*8);
    RtlSetBits(&tls_bitmap,0,1);
}
static BYTE *wine_ntdll;
static LDR_DATA_TABLE_ENTRY *main_entry;
__declspec(dllexport) void WINAPI ProbeInitLoader(HMODULE ntdll)
{
    PEB *peb=NtCurrentTeb()->Peb;
    wine_ntdll=(BYTE *)ntdll;
    LIST_ENTRY *hash=(void *)(wine_ntdll+WINE_NTDLL_hash_table);
    for(unsigned i=0;i<32;i++)InitializeListHead(&hash[i]);
    peb->LdrData=(void *)(wine_ntdll+WINE_NTDLL_ldr);
    peb->LoaderLock=(void *)(wine_ntdll+WINE_NTDLL_loader_section);
    peb->FastPebLock=(void *)(wine_ntdll+WINE_NTDLL_peb_lock);
    *(WCHAR **)(wine_ntdll+WINE_NTDLL_default_load_path)=peb->ProcessParameters->DllPath.Buffer;
    *(BOOL *)(wine_ntdll+WINE_NTDLL_imports_fixup_done)=TRUE;
    UINT count=*(UINT *)(wine_ntdll+WINE_NTDLL_tls_module_count);
    *(void **)(wine_ntdll+WINE_NTDLL_tls_dirs)=RtlAllocateHeap(peb->ProcessHeap,HEAP_ZERO_MEMORY,count*sizeof(IMAGE_TLS_DIRECTORY));
}
__declspec(dllexport) NTSTATUS WINAPI ProbeRegisterModule(HMODULE module,const WCHAR *path,UINT role)
{
    typedef LDR_DATA_TABLE_ENTRY *(__attribute__((regparm(3))) *alloc_module_t)(HMODULE,const UNICODE_STRING *,BOOL);
    UNICODE_STRING name;LDR_DATA_TABLE_ENTRY *entry;
    RtlInitUnicodeString(&name,path);
    entry=((alloc_module_t)(wine_ntdll+WINE_NTDLL_alloc_module))(module,&name,role!=3);
    if(!entry)return STATUS_NO_MEMORY;
    if(role==3)main_entry=entry;
    else { entry->Flags&=~LDR_DONT_RESOLVE_REFS;entry->Flags|=LDR_PROCESS_ATTACHED; }
    entry->LoadCount=-1; /* These images are preloaded for the host lifetime. */
    InsertTailList(&NtCurrentTeb()->Peb->LdrData->InInitializationOrderModuleList,&entry->InInitializationOrderLinks);
    if(role==1)*(LDR_DDAG_NODE **)(wine_ntdll+WINE_NTDLL_node_ntdll)=entry->DdagNode;
    if(role==2)*(LDR_DDAG_NODE **)(wine_ntdll+WINE_NTDLL_node_kernel32)=entry->DdagNode;
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI ProbeFixupImports(void)
{
    typedef NTSTATUS (__attribute__((regparm(2))) *fixup_t)(void *,const WCHAR *);
    NTSTATUS status;
    RtlEnterCriticalSection(NtCurrentTeb()->Peb->LoaderLock);
    status=((fixup_t)(wine_ntdll+WINE_NTDLL_fixup_imports))(main_entry,NtCurrentTeb()->Peb->ProcessParameters->DllPath.Buffer);
    RtlLeaveCriticalSection(NtCurrentTeb()->Peb->LoaderLock);
    return status;
}
__declspec(dllexport) NTSTATUS WINAPI ProbeLoadApplication(const WCHAR *path,HMODULE *module)
{
    UNICODE_STRING name;RtlInitUnicodeString(&name,path);
    NTSTATUS status=LdrLoadDll(NtCurrentTeb()->Peb->ProcessParameters->DllPath.Buffer,DONT_RESOLVE_DLL_REFERENCES,&name,module);
    if(status)return status;
    NtCurrentTeb()->Peb->ImageBaseAddress=*module;
    return LdrFindEntryForAddress(*module,&main_entry);
}
__declspec(dllexport) NTSTATUS WINAPI ProbeSetDllDirectory(const WCHAR *path)
{
    UNICODE_STRING name;RtlInitUnicodeString(&name,path);
    return LdrSetDllDirectory(&name);
}
__declspec(dllexport) NTSTATUS WINAPI ProbeAttachImports(void)
{
    typedef void (__cdecl *loader_init_t)(CONTEXT *,void **);
    CONTEXT context={.ContextFlags=CONTEXT_FULL};void *entry=main_entry->EntryPoint;
    ((loader_init_t)(wine_ntdll+WINE_NTDLL_loader_init))(&context,&entry);
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI ProbeInitThread(void) { return ProbeAttachImports(); }
__declspec(dllexport) NTSTATUS WINAPI ProbeInitialApcs(void) { return NtTestAlert(); }
__declspec(dllexport) void WINAPI ProbeShutdownThread(void) { LdrShutdownThread(); }
__declspec(dllexport) void WINAPI ProbeShutdownProcess(void)
{
    LdrShutdownProcess();
}
