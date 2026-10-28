/* Native Windows bootstrap into Wine 11's original PE loader lifecycle. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "../wine-11.0-source/dlls/ntdll/ntdll_misc.h"
#include "out/wine-ntdll-symbols.h"
#define TRACE(...) do {} while(0)
#define TRACE_(channel) TRACE
/* LdrLoadDll deliberately leaves non-main EXEs unrelocated. Once the host
 * selects the mapped image as the PEB main image, use Wine's original main
 * image relocation algorithm, before import binding or loader callbacks. */
static void *get_rva(void *module,DWORD offset) { return (BYTE *)module+offset; }
static NTSTATUS WINAPI main_image_protect(HANDLE process,void **address,SIZE_T *size,
                                          ULONG protection,ULONG *previous)
{
    /* As in the native bootstrap adapter, empty BSS ranges need no native
     * protection call. Initialize old protection for Wine's restore pass. */
    if(!*size){*previous=0;return STATUS_SUCCESS;}
    return NtProtectVirtualMemory(process,address,size,protection,previous);
}
#define page_size 4096
#define WARN(...) do {} while(0)
#define NtProtectVirtualMemory main_image_protect
#include "out/wine-main-relocations.inc"
#undef NtProtectVirtualMemory
#undef WARN
#undef page_size
/* Return through the existing WoW64 gate, without rewriting Wine code. */
__declspec(dllexport) DWORD ProbeHostExitGate;
__declspec(dllexport) __attribute__((naked)) void WINAPI ProbeExitProcess(DWORD status)
{
    __asm__ volatile("movl 4(%esp),%eax\n\tjmp *_ProbeHostExitGate");
}
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
    RtlSetBits(&tls_bitmap,NTDLL_TLS_ERRNO,1);
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
__declspec(dllexport) NTSTATUS WINAPI ProbeInitProcessParameters(void)
{
    PEB *peb=NtCurrentTeb()->Peb;
    RTL_USER_PROCESS_PARAMETERS *initial=peb->ProcessParameters;
    HANDLE directory=initial->CurrentDirectory.Handle;
    /* Wine normally calls this from its first loader_init pass. Our
     * bootstrap already created that loader state, so run the original
     * initializer once the main image exists. It moves startup parameters
     * and the environment into Wine's heap and initializes directory state. */
    ((void (__cdecl *)(void))(wine_ntdll+WINE_NTDLL_init_user_process_params))();
    if(peb->ProcessParameters==initial)return STATUS_NO_MEMORY;
    if(directory)NtClose(directory);
    *(WCHAR **)(wine_ntdll+WINE_NTDLL_default_load_path)=peb->ProcessParameters->DllPath.Buffer;
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI ProbeRelocateApplication(void)
{
    void *module=NtCurrentTeb()->Peb->ImageBaseAddress;
    IMAGE_NT_HEADERS *nt=RtlImageNtHeader(module);
    if(!nt)return STATUS_INVALID_IMAGE_FORMAT;
    return perform_relocations(module,nt,(nt->OptionalHeader.SizeOfImage+4095)&~(SIZE_T)4095);
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
