#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "shared.h"
#include "cpu_host.h"
#include "native_threads.h"
#include "native_io.h"
#include "out/wine-ntdll-symbols.h"
extern DWORD WINAPI probe_run32(I386_CONTEXT *);
extern void probe_exit64(void),syscall_32to64(void),unix_call_32to64(void);
extern void **__wine_unix_call_dispatcher;
extern UINT cs32_sel,ss32_sel;
static WORD guest_fs_sel;
static BYTE *guest_teb, *guest_peb;
extern NTSTATUS WINAPI probe_library_dispatch(uint64_t,unsigned int,void *);
extern unsigned long probe_nt_calls,probe_library_calls;
extern int probe_trace;
extern void probe_backend_init(void);
extern NTSTATUS WINAPI probe_NtGetNlsSectionPtr(ULONG,ULONG,void *,void **,SIZE_T *);
extern void probe_set_bop_code(void *);
extern void probe_cpu_init(USHORT,USHORT,USHORT);
extern BOOL probe_recover_fs(EXCEPTION_POINTERS *);
extern void probe_resume_guest_context(CONTEXT *);
extern void probe_exception_init(void *,WORD);
extern void probe_apc_init(void *);
extern BOOL probe_dispatch_guest_exception(EXCEPTION_POINTERS *);
extern void probe_exception_thread_cleanup(void);
extern LONG CALLBACK probe_vectored_entry(EXCEPTION_POINTERS *);
extern unsigned long probe_fs_repairs;
struct guest_module { const char *name; BYTE *base; IMAGE_NT_HEADERS32 *nt; WCHAR path[1024]; BOOL imports_done; };
static struct guest_module mods[128];
static char mod_names[128][260];
static unsigned nmods;
static void *early_image_reservation;
extern void mainCRTStartup(void);
/* Keep the usual legacy EXE address available before the host CRT allocates
 * its heap. This reservation is released immediately before mapping the EXE. */
void probe_mainCRTStartup(void)
{
    early_image_reservation=VirtualAlloc((void *)0x400000,64*1024*1024,MEM_RESERVE,PAGE_NOACCESS);
    mainCRTStartup();
}
static struct far_thunk *guest_return_gate;
extern struct guest_cpu *probe_current_cpu(void);
extern TEB32 *probe_current_teb32(void);
static DWORD native_callback_slot=FLS_OUT_OF_INDEXES;
static DWORD callback_on_native_thread(DWORD,const DWORD *,UINT);
static void WINAPI external_guest_thread_end(void *);
DWORD WINAPI probe_callback32(DWORD function,const DWORD *args,UINT count)
{
    struct guest_cpu *parent=probe_current_cpu();
    if(!function||count>64)return 0xffffffffu;
    if(!parent)return callback_on_native_thread(function,args,count);
    struct guest_cpu nested=*parent;
    DWORD address=(parent->ctx.Esp-(count+1)*sizeof(DWORD)-64)&~15u;
    DWORD *stack=(void *)(ULONG_PTR)address;
    stack[0]=(DWORD)(ULONG_PTR)guest_return_gate;
    for(UINT i=0;i<count;i++)stack[i+1]=args[i];
    nested.flags=0;nested.ctx.Eip=function;nested.ctx.Esp=address;
    nested.ctx.ContextFlags=CONTEXT_I386_FULL;
    return probe_run32(&nested.ctx);
}
static void *wine_thread_attach,*wine_thread_detach,*wine_thread_start,*wine_initial_apcs;
static void *native_dispatch_table,*guest_unix_gate;
extern NTSTATUS (WINAPI *pBTCpuNotifyMapViewOfSection)(void *,void *,void *,SIZE_T,ULONG,ULONG);
extern void (WINAPI *pBTCpuNotifyMemoryProtect)(void *,SIZE_T,ULONG,BOOL,NTSTATUS);
static void *guest_export(struct guest_module *m,const char *name,WORD ordinal);
static void fail(const char *s);
static void fail(const char *s) { fprintf(stderr,"FAIL %s, Windows error %lu\n",s,GetLastError()); exit(80); }
static void require_image_relocation_policy(void)
{
    typedef BOOL (WINAPI *query_policy_t)(HANDLE,PROCESS_MITIGATION_POLICY,void *,SIZE_T);
    query_policy_t query=(query_policy_t)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetProcessMitigationPolicy");
    DWORD policy=0;
    if(!query||!query(GetCurrentProcess(),ProcessDynamicCodePolicy,&policy,sizeof(policy)))
        fail("query image relocation policy");
    if(policy&1)
    { SetLastError(ERROR_DYNAMIC_CODE_BLOCKED);fail("image address conflict requires executable relocation"); }
}
/* The loader's bootstrap runs before guest ntdll is callable. Compile Wine's
 * existing relocator for PE32 headers against native memory/heap services.
 * Protection failure is terminal during startup, rather than proceeding with
 * a write after Wine's original unchecked protection request failed. */
static NTSTATUS WINAPI bootstrap_relocation_protect(HANDLE process,void **address,SIZE_T *size,
                                                   ULONG protection,ULONG *previous)
{
    /* Wine iterates all sections, including BSS with no raw file bytes.
     * Native NtProtectVirtualMemory rejects an empty range. It needs no
     * protection change, in either the unprotect or restore pass. */
    if(!*size){*previous=0;return STATUS_SUCCESS;}
    NTSTATUS status=NtProtectVirtualMemory(process,address,size,protection,previous);
    if(status<0){SetLastError(RtlNtStatusToDosError(status));fail("bootstrap relocation protection");}
    return status;
}
static void *get_rva(void *module,DWORD offset) { return (BYTE *)module+offset; }
#define IMAGE_NT_HEADERS IMAGE_NT_HEADERS32
#define LdrProcessRelocationBlock bootstrap_LdrProcessRelocationBlock
#define NtProtectVirtualMemory bootstrap_relocation_protect
#define page_size 4096
#define TRACE(...) do {} while(0)
#define WARN(...) do {} while(0)
#define FIXME(...) do {} while(0)
#include "out/wine-bootstrap-relocations.inc"
#undef FIXME
#undef WARN
#undef TRACE
#undef page_size
#undef NtProtectVirtualMemory
#undef LdrProcessRelocationBlock
#undef IMAGE_NT_HEADERS
struct guest_stack { LIST_ENTRY entry; BYTE *base,*guest_base; SIZE_T reserve,commit; BYTE *limit,*guard; TEB32 *teb; BOOL guest_allocated; };
static SRWLOCK stack_lock=SRWLOCK_INIT;
static LIST_ENTRY stacks={&stacks,&stacks};
/* Windows constructs its native exception frame before the VEH can switch
 * to our saved native stack. Keep 64 KiB committed below the logical guard. */
#define STACK_DISPATCH_SPACE 0x10000
struct thread_stack_info { char *start,*limit,*end;SIZE_T guaranteed;BOOL is_wow; };
static TEB32 *growing_teb;
static NTSTATUS stack_protection_status;
static const SIZE_T host_page_size=4096;
#define VPROT_COMMITTED 1
#define VPROT_GUARD 2
typedef TEB32 WOW_TEB;
static TEB32 *get_wow_teb(TEB *unused) { (void)unused;return growing_teb; }
static void set_page_vprot_bits(void *page,SIZE_T size,BYTE set,BYTE clear)
{
    DWORD old;(void)clear;
    if(!VirtualAlloc(page,size,MEM_COMMIT,PAGE_READWRITE)||
       !VirtualProtect(page,size,PAGE_READWRITE|((set&VPROT_GUARD)?PAGE_GUARD:0),&old))stack_protection_status=STATUS_NO_MEMORY;
}
static void mprotect_range(void *page,SIZE_T size,int set,int clear)
{ (void)page;(void)size;(void)set;(void)clear; }
#include "out/wine-stack-growth.inc"
static void *low_alloc(uintptr_t preferred,SIZE_T n,DWORD protection)
{
    void *p=VirtualAlloc((void *)preferred,n,MEM_COMMIT|MEM_RESERVE,protection);
    if(p) return p;
    for(uintptr_t a=0x10000000;a<0x70000000;a+=0x100000)
        if((p=VirtualAlloc((void *)a,n,MEM_COMMIT|MEM_RESERVE,protection))) return p;
    return 0;
}
static struct guest_stack *stack_find(void *address)
{
    struct guest_stack *found=NULL;BYTE *p=address;
    AcquireSRWLockShared(&stack_lock);
    for(LIST_ENTRY *e=stacks.Flink;e!=&stacks;e=e->Flink)
    {
        struct guest_stack *s=CONTAINING_RECORD(e,struct guest_stack,entry);
        if(p>=s->base&&p<s->base+s->reserve){found=s;break;}
    }
    ReleaseSRWLockShared(&stack_lock);return found;
}
static BYTE *stack_alloc(uintptr_t preferred,SIZE_T reserve,SIZE_T commit,TEB32 *teb)
{
    SIZE_T page=4096;reserve=(reserve+page-1)&~(page-1);commit=(commit+page-1)&~(page-1);
    if(reserve<3*page)return NULL;
    commit=min(max(commit,(SIZE_T)STACK_DISPATCH_SPACE),reserve-2*page);
    BYTE *base=VirtualAlloc((void *)preferred,reserve,MEM_RESERVE,PAGE_READWRITE);
    if(!base)for(uintptr_t a=0x10000000;a<0x70000000&&!base;a+=0x100000)base=VirtualAlloc((void *)a,reserve,MEM_RESERVE,PAGE_READWRITE);
    if(!base)return NULL;
    BYTE *limit=base+reserve-commit,*guard=limit-page;DWORD old;
    BYTE *committed=max(base+page,guard-STACK_DISPATCH_SPACE);
    if(!VirtualAlloc(committed,base+reserve-committed,MEM_COMMIT,PAGE_READWRITE)||
       !VirtualProtect(guard,page,PAGE_READWRITE|PAGE_GUARD,&old)){VirtualFree(base,0,MEM_RELEASE);return NULL;}
    struct guest_stack *s=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*s));
    if(!s){VirtualFree(base,0,MEM_RELEASE);return NULL;}
    s->base=base;s->guest_base=base;s->reserve=reserve;s->commit=commit;s->limit=limit;s->guard=guard;s->teb=teb;
    AcquireSRWLockExclusive(&stack_lock);InsertTailList(&stacks,&s->entry);ReleaseSRWLockExclusive(&stack_lock);
    return base;
}
static void stack_bind(BYTE *base,TEB32 *teb)
{ struct guest_stack *s=stack_find(base);if(s)s->teb=teb; }
static DWORD stack_limit(BYTE *base)
{ struct guest_stack *s=stack_find(base);return (DWORD)(ULONG_PTR)(s?s->limit:base); }
static void stack_free(BYTE *base)
{
    struct guest_stack *s=stack_find(base);if(!s){if(base)VirtualFree(base,0,MEM_RELEASE);return;}
    AcquireSRWLockExclusive(&stack_lock);RemoveEntryList(&s->entry);ReleaseSRWLockExclusive(&stack_lock);
    VirtualFree(s->base,0,MEM_RELEASE);HeapFree(GetProcessHeap(),0,s);
}
/* Wine RtlCreateUserStack uses ProcessThreadStackAllocation for fibers.
 * Keep its returned logical reservation intact and add space below it for
 * Windows to construct a native exception frame before our VEH runs. */
NTSTATUS WINAPI probe_NtSetInformationProcess(HANDLE process,PROCESSINFOCLASS class,void *buffer,ULONG length)
{
    PROCESS_STACK_ALLOCATION_INFORMATION *alloc;
    if(class!=ProcessThreadStackAllocation||
       (process!=GetCurrentProcess()&&GetProcessId(process)!=GetCurrentProcessId()))
        return NtSetInformationProcess(process,class,buffer,length);
    if(length==sizeof(PROCESS_STACK_ALLOCATION_INFORMATION))alloc=buffer;
    else if(length==sizeof(PROCESS_STACK_ALLOCATION_INFORMATION_EX))
        alloc=&((PROCESS_STACK_ALLOCATION_INFORMATION_EX *)buffer)->AllocInfo;
    else return NtSetInformationProcess(process,class,buffer,length);
    SIZE_T reserve=alloc->ReserveSize;
    if(reserve>SIZE_MAX-STACK_DISPATCH_SPACE)return STATUS_NO_MEMORY;
    struct guest_stack *s=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*s));
    if(!s)return STATUS_NO_MEMORY;
    alloc->ReserveSize=reserve+STACK_DISPATCH_SPACE;
    NTSTATUS status=NtSetInformationProcess(process,class,buffer,length);
    alloc->ReserveSize=reserve;
    if(status){HeapFree(GetProcessHeap(),0,s);return status;}
    BYTE *base=alloc->StackBase;
    if(!VirtualAlloc(base+4096,STACK_DISPATCH_SPACE,MEM_COMMIT,PAGE_READWRITE))
    { VirtualFree(base,0,MEM_RELEASE);HeapFree(GetProcessHeap(),0,s);return STATUS_NO_MEMORY; }
    s->base=base;s->guest_base=base+STACK_DISPATCH_SPACE;
    s->reserve=reserve+STACK_DISPATCH_SPACE;s->guest_allocated=TRUE;
    s->limit=s->guest_base+8192;s->guard=s->guest_base+4096;
    AcquireSRWLockExclusive(&stack_lock);InsertTailList(&stacks,&s->entry);ReleaseSRWLockExclusive(&stack_lock);
    alloc->StackBase=s->guest_base;
    return STATUS_SUCCESS;
}
NTSTATUS WINAPI probe_NtFreeVirtualMemory(HANDLE process,void **address,SIZE_T *size,ULONG type)
{
    if(type!=MEM_RELEASE||
       (process!=GetCurrentProcess()&&GetProcessId(process)!=GetCurrentProcessId()))
        return NtFreeVirtualMemory(process,address,size,type);
    AcquireSRWLockExclusive(&stack_lock);
    for(LIST_ENTRY *e=stacks.Flink;e!=&stacks;e=e->Flink)
    {
        struct guest_stack *s=CONTAINING_RECORD(e,struct guest_stack,entry);
        if(!s->guest_allocated||*address!=s->guest_base)continue;
        void *base=s->base;SIZE_T actual_size=*size;
        NTSTATUS status=NtFreeVirtualMemory(process,&base,&actual_size,type);
        if(!status){RemoveEntryList(&s->entry);HeapFree(GetProcessHeap(),0,s);*address=base;*size=actual_size;}
        ReleaseSRWLockExclusive(&stack_lock);return status;
    }
    ReleaseSRWLockExclusive(&stack_lock);return NtFreeVirtualMemory(process,address,size,type);
}
NTSTATUS (WINAPI *__imp_probe_NtSetInformationProcess)(HANDLE,PROCESSINFOCLASS,void *,ULONG)=probe_NtSetInformationProcess;
NTSTATUS (WINAPI *__imp_probe_NtFreeVirtualMemory)(HANDLE,void **,SIZE_T *,ULONG)=probe_NtFreeVirtualMemory;
static BOOL stack_matches_teb(struct guest_stack *s,TEB32 *teb)
{
    if(!teb)return FALSE;
    if(s->guest_allocated&&s->guest_base==(BYTE *)(ULONG_PTR)teb->DeallocationStack)
        s->teb=teb;
    return s->teb==teb;
}
static NTSTATUS stack_grow_locked(struct guest_stack *s,BYTE *page)
{
    BYTE *target=max(s->base+4096,page-STACK_DISPATCH_SPACE);
    BYTE *committed=max(s->base+4096,target-STACK_DISPATCH_SPACE);
    if(!VirtualAlloc(committed,page+4096-committed,MEM_COMMIT,PAGE_READWRITE))
        return STATUS_NO_MEMORY;
    struct thread_stack_info info={(char *)s->base,(char *)s->limit,(char *)s->base+s->reserve,
        max((SIZE_T)STACK_DISPATCH_SPACE,s->teb?(SIZE_T)s->teb->GuaranteedStackBytes:0),TRUE};
    growing_teb=s->teb;stack_protection_status=STATUS_SUCCESS;NTSTATUS status=STATUS_SUCCESS;
    do
    {
        status=grow_thread_stack((char *)page,&info);
        s->limit=(BYTE *)(ULONG_PTR)s->teb->Tib.StackLimit;
        if(status||stack_protection_status)break;
        page-=4096;
    }while(page>target);
    s->guard=status?NULL:s->limit-4096;growing_teb=NULL;
    return status?status:stack_protection_status;
}
static BOOL guest_stack_fault(EXCEPTION_POINTERS *p)
{
    if(p->ExceptionRecord->ExceptionCode!=STATUS_GUARD_PAGE_VIOLATION)return FALSE;
    BYTE *address=(BYTE *)(ULONG_PTR)p->ExceptionRecord->ExceptionInformation[1];
    TEB32 *teb=probe_current_teb32();
    AcquireSRWLockExclusive(&stack_lock);
    struct guest_stack *s=NULL;for(LIST_ENTRY *e=stacks.Flink;e!=&stacks;e=e->Flink)
    { struct guest_stack *candidate=CONTAINING_RECORD(e,struct guest_stack,entry);if(stack_matches_teb(candidate,teb)&&address>=candidate->base+4096&&address<candidate->base+candidate->reserve){s=candidate;break;} }
    if(!s){ReleaseSRWLockExclusive(&stack_lock);return FALSE;}
    NTSTATUS status=stack_grow_locked(s,(BYTE *)((ULONG_PTR)address&~(ULONG_PTR)4095));
    if(status)p->ExceptionRecord->ExceptionCode=status;
    ReleaseSRWLockExclusive(&stack_lock);return !status;
}
static void WINAPI notify_stack_protection(void *address,SIZE_T size,ULONG protection,BOOL after,NTSTATUS status)
{
    if(!after||status||!(protection&PAGE_GUARD))return;
    struct guest_cpu *cpu=probe_current_cpu();if(!cpu)return;
    BYTE *sp=(BYTE *)(ULONG_PTR)cpu->ctx.Esp;
    if(sp<(BYTE *)address||sp>=(BYTE *)address+size)return;
    /* Wine's _resetstkoflw guards its own active page and expects its first
     * stack access to run grow_thread_stack. Windows can consume that guard
     * inside NtProtectVirtualMemory while copying out old_prot, without a
     * user-mode exception. Complete Wine's stack update before its thunk
     * copies the remaining outputs back to that active page. */
    AcquireSRWLockExclusive(&stack_lock);
    for(LIST_ENTRY *e=stacks.Flink;e!=&stacks;e=e->Flink)
    {
        struct guest_stack *s=CONTAINING_RECORD(e,struct guest_stack,entry);
        if(stack_matches_teb(s,probe_current_teb32())&&sp>=s->base+4096+STACK_DISPATCH_SPACE&&sp<s->base+s->reserve)
        { stack_grow_locked(s,(BYTE *)((ULONG_PTR)sp&~(ULONG_PTR)4095));break; }
    }
    ReleaseSRWLockExclusive(&stack_lock);
}
static struct guest_module *load_image(const char *path,const char *name)
{
    HANDLE file=CreateFileA(path,GENERIC_READ|GENERIC_EXECUTE,
                            FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL),section=NULL;
    if(file==INVALID_HANDLE_VALUE)fail(path);
    NTSTATUS status=NtCreateSection(&section,SECTION_MAP_READ|SECTION_MAP_WRITE|SECTION_MAP_EXECUTE,
                                    NULL,NULL,PAGE_EXECUTE_READ,SEC_IMAGE,file);
    CloseHandle(file);
    if(status<0)fail("create bootstrap image section");
    /* A NULL address lets Windows use this file's canonical image mapping.
     * Windows has already relocated the image to the mapped header ImageBase.
     * An explicit preferred address can instead return another view of those
     * same relocated bytes, requiring a second relocation of executable pages. */
    void *image=NULL;SIZE_T size=0;
    status=NtMapViewOfSection(section,GetCurrentProcess(),&image,0,0,NULL,&size,ViewUnmap,0,PAGE_READONLY);
    NtClose(section);
    if(status<0)fail("map bootstrap image");
    BYTE *base=image;
    if(size<sizeof(IMAGE_NT_HEADERS32)||size>UINT32_MAX||(uintptr_t)base>UINT32_MAX-size)
        fail("bootstrap image address range");
    IMAGE_DOS_HEADER *dos=image;
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||
       (SIZE_T)dos->e_lfanew>size-sizeof(IMAGE_NT_HEADERS32))fail("bootstrap DOS header");
    IMAGE_NT_HEADERS32 *nt=(void *)(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||
       nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC)fail("expected i386 PE32 image");
    if(nmods>=128||nt->OptionalHeader.SizeOfImage>64*1024*1024)fail("image limit");
    if((uintptr_t)base!=nt->OptionalHeader.ImageBase)
    {
        require_image_relocation_policy();
        if(!(nt->FileHeader.Characteristics&IMAGE_FILE_DLL))fail("bootstrap EXE address conflict");
        printf("BOOTSTRAP_RELOCATE %s canonical=%08lx mapped=%p\n",name,nt->OptionalHeader.ImageBase,base);
        NTSTATUS relocation=perform_relocations(base,nt,size);
        if(relocation){SetLastError(RtlNtStatusToDosError(relocation));fail("Wine bootstrap relocation");}
        FlushInstructionCache(GetCurrentProcess(),base,size);
    }
    struct guest_module *m;
    m=&mods[nmods];size_t name_len=strlen(name);if(name_len>=sizeof(mod_names[nmods]))name_len=sizeof(mod_names[nmods])-1;
    memcpy(mod_names[nmods],name,name_len);mod_names[nmods][name_len]=0;
    m->name=mod_names[nmods++];m->base=base;
    WCHAR relative[1024];
    MultiByteToWideChar(CP_ACP,0,path,-1,relative,1024);
    if(!GetFullPathNameW(relative,1024,m->path,NULL))fail("module absolute path");
    m->nt=(void *)(base+((IMAGE_DOS_HEADER *)base)->e_lfanew);
    printf("MAP %s PE32 at %p, %lu bytes\n",name,base,m->nt->OptionalHeader.SizeOfImage);
    return m;
}
static void *guest_export(struct guest_module *m,const char *name,WORD ordinal)
{
    IMAGE_DATA_DIRECTORY d=m->nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY *e=(void *)(m->base+d.VirtualAddress);
    DWORD *funcs=(void *)(m->base+e->AddressOfFunctions),*names=(void *)(m->base+e->AddressOfNames);
    WORD *ords=(void *)(m->base+e->AddressOfNameOrdinals);
    DWORD index=(DWORD)-1;
    if(!d.Size) return 0;
    if(!name) index=ordinal-e->Base;
    else for(DWORD i=0;i<e->NumberOfNames;i++)
    {
        const char *export_name=(char *)m->base+names[i];
        if(!strcmp(export_name,name) || ({ const char *at=strchr(name,'@');
                                           at && (size_t)(at-name)==strlen(export_name) && !memcmp(export_name,name,at-name); }))
        { index=ords[i];break; }
    }
    if(index>=e->NumberOfFunctions) return 0;
    DWORD rva=funcs[index];
    if(rva>=d.VirtualAddress&&rva<d.VirtualAddress+d.Size)
    {
        char fwd[256],*dot; struct guest_module *dep=0; const char *target;
        { size_t flen=strlen((char *)m->base+rva); if(flen>=sizeof(fwd)) fail("forwarded guest export too long");
          memcpy(fwd,(char *)m->base+rva,flen+1); }
        dot=strchr(fwd,'.'); if(!dot) fail("invalid forwarded guest export"); *dot++=0;
        for(unsigned i=0;i<nmods;i++) if(!_stricmp(mods[i].name,fwd)) dep=&mods[i];
        if(!dep) { char dllname[260]; snprintf(dllname,sizeof(dllname),"%s.dll",fwd);
                   for(unsigned i=0;i<nmods;i++) if(!_stricmp(mods[i].name,dllname)) dep=&mods[i]; }
        if(!dep) { fprintf(stderr,"UNSUPPORTED forwarded DLL %s\n",fwd); fail("forwarded guest DLL"); }
        target=dot[0]=='#'?NULL:dot;
        return guest_export(dep,target,target?(WORD)0:(WORD)strtoul(dot+1,NULL,10));
    }
    return m->base+rva;
}
static struct guest_module *find_module(const char *name)
{
    for(unsigned i=0;i<nmods;i++) if(!_stricmp(mods[i].name,name))return &mods[i];
    return NULL;
}
/* Bootstrap imports are the only pre-loader fixups. Match Wine's loader by
 * restoring the IAT's original permissions after binding it. Only data pages
 * may be written here, so this works under dynamic-code prohibition. */
static void write_bootstrap_import(DWORD *slot,DWORD target)
{
    MEMORY_BASIC_INFORMATION info;DWORD old;
    if(!VirtualQuery(slot,&info,sizeof(info)))fail("query bootstrap IAT");
    if(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))
        fail("bootstrap IAT shares executable memory");
    if(info.Protect&(PAGE_READWRITE|PAGE_WRITECOPY)){*slot=target;return;}
    if(!VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&old))fail("unprotect bootstrap IAT");
    *slot=target;
    if(!VirtualProtect(slot,sizeof(*slot),old,&old))fail("restore bootstrap IAT protection");
}
static void resolve_imports(struct guest_module *m)
{
    if(m->imports_done)return;
    IMAGE_DATA_DIRECTORY d=m->nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!d.Size) return;
    IMAGE_IMPORT_DESCRIPTOR *imp=(void *)(m->base+d.VirtualAddress);
    for(;imp->Name;imp++)
    {
        struct guest_module *dep=0; const char *name=(char *)m->base+imp->Name;
        dep=find_module(name);
        if(!dep) { fprintf(stderr,"UNSUPPORTED imported DLL %s\n",name); fail("guest import"); }
        DWORD *src=(void *)(m->base+(imp->OriginalFirstThunk?imp->OriginalFirstThunk:imp->FirstThunk));
        DWORD *dst=(void *)(m->base+imp->FirstThunk);
        for(;*src;src++,dst++)
        {
            const char *fn=(*src&IMAGE_ORDINAL_FLAG32)?0:(char *)m->base+*src+2;
            void *target=guest_export(dep,fn,(WORD)*src);
            if(!target) { fprintf(stderr,"UNSUPPORTED %s!%s\n",name,fn?fn:"ordinal");fail("guest symbol"); }
            write_bootstrap_import(dst,(DWORD)(uintptr_t)target);
        }
    }
    m->imports_done=TRUE;
}
static NTSTATUS WINAPI bind_native_bridge(void *unused,void *image,void *unused2,SIZE_T size,ULONG alloc,ULONG protect)
{
    struct guest_module module={.base=image};
    module.nt=(void *)((BYTE *)image+((IMAGE_DOS_HEADER *)image)->e_lfanew);
    void *handle=guest_export(&module,"native_handle",0);
    void *dispatcher=guest_export(&module,"__wine_unix_call_dispatcher",0);
    if(handle&&dispatcher)
    {
        *(uint64_t *)handle=(uintptr_t)native_dispatch_table;
        *(DWORD *)dispatcher=(DWORD)(uintptr_t)guest_unix_gate;
        printf("WINE_BRIDGE_BIND image=%p native_table=%p\n",image,native_dispatch_table);
    }
    (void)unused;(void)unused2;(void)size;(void)alloc;(void)protect;
    return STATUS_SUCCESS;
}
struct far_thunk { BYTE code[16]; };
#pragma pack(push,1)
struct gate_far_pointer { DWORD address; WORD selector; };
#pragma pack(pop)
static struct far_thunk *map_transition_gates(WORD cs)
{
    HANDLE file=CreateFileA("guest/transition32.dll",GENERIC_READ|GENERIC_EXECUTE,
                            FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL),section=NULL;
    if(file==INVALID_HANDLE_VALUE)fail("open transition image");
    NTSTATUS status=NtCreateSection(&section,SECTION_MAP_READ|SECTION_MAP_WRITE|SECTION_MAP_EXECUTE,
                                    NULL,NULL,PAGE_EXECUTE_READ,SEC_IMAGE,file);
    CloseHandle(file);
    if(status<0)fail("create transition image section");
    void *base=(void *)0x20000000;SIZE_T size=0;
    status=NtMapViewOfSection(section,GetCurrentProcess(),&base,0,0,NULL,&size,ViewUnmap,0,PAGE_READONLY);
    if(status<0)for(uintptr_t address=0x10000000;address<0x70000000;address+=0x100000)
    {
        base=(void *)address;size=0;
        status=NtMapViewOfSection(section,GetCurrentProcess(),&base,0,0,NULL,&size,ViewUnmap,0,PAGE_READONLY);
        if(status>=0)break;
    }
    NtClose(section);
    if(status<0)fail("map transition image below 4 GiB");
    if((uintptr_t)base>UINT32_MAX-size)fail("transition image address range");
    struct guest_module image={.base=base};
    image.nt=(void *)((BYTE *)base+((IMAGE_DOS_HEADER *)base)->e_lfanew);
    struct far_thunk *entries=guest_export(&image,"probe_gate_entries",0);
    struct far_thunk *trampolines=guest_export(&image,"probe_gate_trampolines",0);
    struct gate_far_pointer *far=guest_export(&image,"probe_gate_far",0);
    uint64_t *targets=guest_export(&image,"probe_gate_targets",0);
    if(!entries||!trampolines||!far||!targets)fail("transition image exports");
    void *native_targets[]={syscall_32to64,unix_call_32to64,probe_exit64};
    for(unsigned i=0;i<3;i++)
    {
        far[i].address=(DWORD)(uintptr_t)&trampolines[i];far[i].selector=cs;
        targets[i]=(uintptr_t)native_targets[i];
    }
    printf("TRANSITION_IMAGE base=%p entries=%p\n",base,entries);
    return entries;
}
LONG CALLBACK probe_on_exception(EXCEPTION_POINTERS *p)
{
    if(guest_stack_fault(p))
    { probe_resume_guest_context(p->ContextRecord);return EXCEPTION_CONTINUE_EXECUTION; }
    if(probe_recover_fs(p)) return EXCEPTION_CONTINUE_EXECUTION;
    if(probe_dispatch_guest_exception(p))return EXCEPTION_CONTINUE_EXECUTION;
    /* Wine's raised-exception thunk owns its native x64 SEH frame. */
    if(p->ContextRecord->SegCs==0x33)return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr,"EXCEPTION code=%08lx rip=%llx cs=%x ss=%x flags=%lx rsp=%llx r13=%llx r14=%llx\n",p->ExceptionRecord->ExceptionCode,p->ContextRecord->Rip,p->ContextRecord->SegCs,p->ContextRecord->SegSs,p->ContextRecord->EFlags,p->ContextRecord->Rsp,p->ContextRecord->R13,p->ContextRecord->R14);
    fprintf(stderr,"EXCEPTION detail type=%llu address=%llx\n",p->ExceptionRecord->ExceptionInformation[0],p->ExceptionRecord->ExceptionInformation[1]);
    { DWORD *s=(void *)(uintptr_t)p->ContextRecord->Rsp;fprintf(stderr,"STACK32 %lx %lx %lx %lx\n",s[0],s[1],s[2],s[3]); }
    if(p->ContextRecord->SegCs==0x33)
    {
        ULONG64 *frame=(void *)(uintptr_t)p->ContextRecord->Rsp;
        fprintf(stderr,"STACK %llx %llx %llx %llx %llx\n",frame[0],frame[1],frame[2],frame[3],frame[4]);
    }
    fflush(stderr); ExitProcess(99); return EXCEPTION_CONTINUE_SEARCH;
}
static void init_guest_teb(BYTE *image)
{
    DWORD pid=GetCurrentProcessId(),tid=GetCurrentThreadId();
    guest_teb=low_alloc(0x34000000,0x3000,PAGE_READWRITE);
    /* Wine debug options follow the x86 PEB at the next page. The zero
     * sentinel keeps channels disabled without patching debug routines. */
    guest_peb=low_alloc(0x35000000,0x2000,PAGE_READWRITE);
    if(!guest_teb||!guest_peb) fail("guest TEB/PEB allocation");
    memset(guest_teb,0,0x3000);memset(guest_peb,0,0x2000);
    TEB32 *teb32=(void *)guest_teb;
    teb32->Tib.ExceptionList=~0u;
    teb32->StaticUnicodeString.MaximumLength=sizeof(teb32->StaticUnicodeBuffer);
    teb32->StaticUnicodeString.Buffer=(DWORD)(uintptr_t)teb32->StaticUnicodeBuffer;
    teb32->ActivationContextStackPointer=(DWORD)(uintptr_t)&teb32->ActivationContextStack;
    teb32->ActivationContextStack.FrameListCache.Flink=teb32->ActivationContextStack.FrameListCache.Blink=
        (DWORD)(uintptr_t)&teb32->ActivationContextStack.FrameListCache;
    *(DWORD *)(guest_teb+0x04)=0x34003000; *(DWORD *)(guest_teb+0x08)=0x34000000;
    *(DWORD *)(guest_teb+0x18)=(DWORD)(uintptr_t)guest_teb;
    *(DWORD *)(guest_teb+0x20)=pid; *(DWORD *)(guest_teb+0x24)=tid;
    *(DWORD *)(guest_teb+0x2c)=(DWORD)(uintptr_t)(guest_teb+0xe10);
    *(DWORD *)(guest_teb+0x30)=(DWORD)(uintptr_t)guest_peb;
    *(DWORD *)(guest_peb+0x08)=(DWORD)(uintptr_t)image;
    /* Native and Wine heaps have different layouts. Initialize Wine's heap
     * below once its PE code and syscall gates are executable. */
    *(DWORD *)(guest_peb+0x64)=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    RTL_OSVERSIONINFOEXW version={.dwOSVersionInfoSize=sizeof(version)};
    RtlGetVersion(&version);
    *(DWORD *)(guest_peb+0xa4)=version.dwMajorVersion;
    *(DWORD *)(guest_peb+0xa8)=version.dwMinorVersion;
    *(WORD *)(guest_peb+0xac)=(WORD)version.dwBuildNumber;
    *(DWORD *)(guest_peb+0xb0)=VER_PLATFORM_WIN32_NT;
    { LDT_ENTRY entry={0}; struct { ULONG Start,Length; LDT_ENTRY Entries[1]; } info={0}; NTSTATUS st;
      entry.LimitLow=0x0fff;entry.BaseLow=(WORD)(uintptr_t)guest_teb;
      entry.HighWord.Bytes.BaseMid=(BYTE)((uintptr_t)guest_teb>>16);entry.HighWord.Bytes.BaseHi=(BYTE)((uintptr_t)guest_teb>>24);
      entry.HighWord.Bits.Type=0x13;entry.HighWord.Bits.Dpl=3;entry.HighWord.Bits.Pres=1;entry.HighWord.Bits.Default_Big=1;
      info.Start=8;info.Length=sizeof(entry);info.Entries[0]=entry;
      st=NtSetInformationProcess(GetCurrentProcess(),10,&info,sizeof(info));
      fprintf(stderr,"PROCESS_LDT status=%08lx\n",(unsigned long)st);
      { struct { ULONG Selector; LDT_ENTRY Entry; } ti={0}; ti.Selector=0x0f;ti.Entry=entry;
        st=NtSetInformationThread(GetCurrentThread(),6,&ti,sizeof(ti));
        fprintf(stderr,"THREAD_DESCRIPTOR_SET status=%08lx\n",(unsigned long)st); }
    }
}
static void report_mitigations(void)
{
    typedef BOOL (WINAPI *query_policy)(HANDLE,PROCESS_MITIGATION_POLICY,void *,SIZE_T);
    query_policy query=(query_policy)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetProcessMitigationPolicy");
    if(!query)return;
    /* DEP has a trailing BOOLEAN and padding. A DWORD-sized query fails. */
    struct { DWORD flags;BOOLEAN permanent;BYTE padding[3]; } dep={0};
    DWORD aslr=0,cfg=0,dyn=0,cet=0;
    BOOL e=query(GetCurrentProcess(),ProcessDEPPolicy,&dep,sizeof(dep));
    BOOL a=query(GetCurrentProcess(),ProcessASLRPolicy,&aslr,sizeof(aslr));
    BOOL c=query(GetCurrentProcess(),ProcessControlFlowGuardPolicy,&cfg,sizeof(cfg));
    BOOL d=query(GetCurrentProcess(),ProcessDynamicCodePolicy,&dyn,sizeof(dyn));
    BOOL s=query(GetCurrentProcess(),(PROCESS_MITIGATION_POLICY)15,&cet,sizeof(cet));
    printf("MITIGATION DEP=%d enabled=%d permanent=%d ASLR=%d flags=%lx CFG=%d flags=%lx dynamic-code=%d flags=%lx CET=%d flags=%lx\n",
        e,!!(dep.flags&1),dep.permanent,a,aslr,c,cfg,d,dyn,s,cet);
}
static void copy_string32(UNICODE_STRING32 *dst,const WCHAR *str,BYTE **next)
{
    SIZE_T bytes=(wcslen(str)+1)*sizeof(WCHAR);
    memcpy(*next,str,bytes);dst->Length=(USHORT)(bytes-2);dst->MaximumLength=(USHORT)bytes;
    dst->Buffer=(DWORD)(uintptr_t)*next;*next+=bytes;
}
static void init_guest_process_params(struct guest_module *app,int argc,char **argv)
{
    BYTE *mem=low_alloc(0x3c000000,0x20000,PAGE_READWRITE),*next;
    RTL_USER_PROCESS_PARAMETERS32 *p=(void *)mem;
    WCHAR cwd[1024],cmd[16384],dllpath[4096],stock[1024];
    if(!mem) fail("guest process parameters");next=mem+sizeof(*p);
    p->AllocationSize=0x20000;p->Size=sizeof(*p);p->Flags=1;
    p->hStdInput=(DWORD)(uintptr_t)GetStdHandle(STD_INPUT_HANDLE);
    p->hStdOutput=(DWORD)(uintptr_t)GetStdHandle(STD_OUTPUT_HANDLE);
    p->hStdError=(DWORD)(uintptr_t)GetStdHandle(STD_ERROR_HANDLE);
    if(!GetCurrentDirectoryW(1024,cwd)) fail("current directory");
    if(cwd[wcslen(cwd)-1]!=L'\\') wcscat(cwd,L"\\");
    copy_string32(&p->CurrentDirectory.DosPath,cwd,&next);
    /* Wine's startup initializer retains this buffer capacity. Match the
     * Windows startup contract before it copies the staging parameters. */
    if(p->CurrentDirectory.DosPath.MaximumLength<MAX_PATH*sizeof(WCHAR))
    {
        next+=MAX_PATH*sizeof(WCHAR)-p->CurrentDirectory.DosPath.MaximumLength;
        p->CurrentDirectory.DosPath.MaximumLength=MAX_PATH*sizeof(WCHAR);
    }
    HANDLE dir=CreateFileW(cwd,FILE_TRAVERSE|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
    if(dir==INVALID_HANDLE_VALUE) fail("current directory handle");
    p->CurrentDirectory.Handle=(DWORD)(uintptr_t)dir;
    copy_string32(&p->ImagePathName,app->path,&next);
    swprintf(cmd,16384,L"\"%s\"",app->path);
    for(int i=0;i<argc;i++)
    {
        WCHAR arg[4096];MultiByteToWideChar(CP_ACP,0,argv[i],-1,arg,4096);
        size_t used=wcslen(cmd);if(used+wcslen(arg)*2+4>=16384)fail("command line length");
        WCHAR *dst=cmd+used;*dst++=L' ';*dst++=L'"';
        unsigned slashes=0;
        for(WCHAR *src=arg;;src++)
        {
            if(*src==L'\\') { slashes++;continue; }
            unsigned count=(*src==L'"'||!*src)?slashes*2:slashes;
            while(count--)*dst++=L'\\';slashes=0;
            if(!*src)break;
            if(*src==L'"')*dst++=L'\\';
            *dst++=*src;
        }
        *dst++=L'"';*dst=0;
    }
    copy_string32(&p->CommandLine,cmd,&next);
    if(!GetFullPathNameW(L"stock-wine",1024,stock,NULL))fail("Wine DLL directory");
    wcscpy(dllpath,app->path);WCHAR *end_path=wcsrchr(dllpath,L'\\');if(end_path)*end_path=0;
    wcscat(dllpath,L";");wcscat(dllpath,stock);wcscat(dllpath,L";");wcscat(dllpath,cwd);
    copy_string32(&p->DllPath,dllpath,&next);
    WCHAR *env=GetEnvironmentStringsW(),*end=env;
    if(!env) fail("environment");while(*end) end+=wcslen(end)+1;
    SIZE_T env_size=(end-env+1)*sizeof(WCHAR);
    if(next+env_size>mem+0x20000) fail("process parameters size");
    memcpy(next,env,env_size);p->Environment=(DWORD)(uintptr_t)next;p->EnvironmentSize=(DWORD)env_size;
    FreeEnvironmentStringsW(env);
    *(DWORD *)(guest_peb+0x10)=(DWORD)(uintptr_t)p;
    /* API-set entries contain offsets, so the Windows namespace can be
     * copied into guest-addressable memory and parsed by Wine unchanged. */
    const API_SET_NAMESPACE *apiset=NtCurrentTeb()->Peb->ApiSetMap;
    if(apiset&&apiset->Size&&apiset->Size<16*1024*1024)
    {
        void *copy=low_alloc(0x3e000000,apiset->Size,PAGE_READWRITE);
        if(!copy)fail("API-set namespace allocation");
        memcpy(copy,apiset,apiset->Size);
        ((PEB32 *)guest_peb)->ApiSetMap=(DWORD)(uintptr_t)copy;
    }
    for(unsigned i=0;i<3;i++)
    {
        SIZE_T size;void *table=NULL;
        NTSTATUS status=probe_NtGetNlsSectionPtr(i==2?10:11,i==0?GetACP():i==1?GetOEMCP():0,NULL,&table,&size);
        if(status<0) { fprintf(stderr,"NLS status=%08lx table=%u\n",(unsigned long)status,i);fail("guest NLS table"); }
        *(DWORD *)(guest_peb+0x58+i*4)=(DWORD)(uintptr_t)table;
    }
}
static DWORD run_bootstrap_stack(struct guest_cpu *cpu,BYTE *stack,SIZE_T size)
{
    TEB32 *teb=(TEB32 *)guest_teb;
    DWORD base=teb->Tib.StackBase,limit=teb->Tib.StackLimit;
    teb->Tib.StackBase=(DWORD)(uintptr_t)(stack+size);
    teb->Tib.StackLimit=stack_limit(stack);
    DWORD result=probe_run32(&cpu->ctx);
    teb->Tib.StackBase=base;teb->Tib.StackLimit=limit;
    return result;
}
static DWORD call_guest_dllmain(struct guest_module *m,struct far_thunk *exit_gate)
{
    struct guest_cpu cpu={0}; BYTE *stack=stack_alloc(0x3a000000,0x10000,0x8000,(TEB32 *)guest_teb); DWORD *sp;
    cpu.guest_fs_base=(uintptr_t)guest_teb;
    if(!m->nt->OptionalHeader.AddressOfEntryPoint) return 1;
    if(!stack) fail("DllMain stack"); sp=(DWORD *)(stack+0x10000-32);sp[0]=(DWORD)(uintptr_t)exit_gate;sp[1]=(DWORD)(uintptr_t)m->base;sp[2]=1;sp[3]=0;
    cpu.ctx.ContextFlags=CONTEXT_I386_FULL;cpu.ctx.Eip=(DWORD)(uintptr_t)(m->base+m->nt->OptionalHeader.AddressOfEntryPoint);
    cpu.ctx.Esp=(DWORD)(uintptr_t)sp;cpu.ctx.SegCs=cs32_sel;cpu.ctx.SegSs=ss32_sel;cpu.ctx.SegFs=guest_fs_sel;cpu.ctx.EFlags=0x202;
    DWORD result=run_bootstrap_stack(&cpu,stack,0x10000);stack_free(stack);return result;
}
static DWORD call_guest_stdcall(struct guest_module *m,void *proc,struct far_thunk *exit_gate,const DWORD *args,unsigned count)
{
    struct guest_cpu cpu={0}; BYTE *stack=stack_alloc(0x3b000000,0x10000,0x8000,(TEB32 *)guest_teb); DWORD *sp;
    cpu.guest_fs_base=(uintptr_t)guest_teb;
    if(!stack) fail("guest call stack"); sp=(DWORD *)(stack+0x10000-32-count*4);sp[0]=(DWORD)(uintptr_t)exit_gate;
    for(unsigned i=0;i<count;i++) sp[1+i]=args[i];
    cpu.ctx.ContextFlags=CONTEXT_I386_FULL;cpu.ctx.Eip=(DWORD)(uintptr_t)proc;cpu.ctx.Esp=(DWORD)(uintptr_t)sp;
    cpu.ctx.SegCs=cs32_sel;cpu.ctx.SegSs=ss32_sel;cpu.ctx.SegFs=guest_fs_sel;cpu.ctx.EFlags=0x202;
    DWORD result=run_bootstrap_stack(&cpu,stack,0x10000);stack_free(stack);return result;
}
struct guest_thread_job {
    struct guest_cpu cpu;
    BYTE *stack,*teb;
    SIZE_T stack_size;
    struct guest_thread *record;
    HANDLE native;
    PTP_WAIT wait;
    DWORD id;
    BOOL wine_lifecycle;
};
static void free_guest_thread_storage(void *opaque)
{
    struct guest_thread_job *job=opaque;
    if(job->wait)CloseThreadpoolWait(job->wait);
    if(job->record)
        HeapFree(GetProcessHeap(),0,probe_thread_take_exception_state(job->record));
    if(job->native)CloseHandle(job->native);
    if(job->stack)stack_free(job->stack);
    if(job->teb)VirtualFree(job->teb,0,MEM_RELEASE);
    VirtualFree(job,0,MEM_RELEASE);
}
static void free_guest_thread(struct guest_thread_job *job)
{
    /* Context/APC callers pin the record. Its CPU, TEB and stack storage
     * must survive until their final reference, including after native exit. */
    struct guest_thread *record=job->record;
    if(record){probe_thread_remove(record);probe_thread_release(record);}
    else free_guest_thread_storage(job);
}
static void CALLBACK guest_thread_finished(PTP_CALLBACK_INSTANCE instance,void *opaque,PTP_WAIT wait,TP_WAIT_RESULT result)
{
    struct guest_thread_job *job=opaque;(void)instance;(void)wait;(void)result;
    probe_io_drain_thread(job->id);free_guest_thread(job);
}
static struct guest_thread_job *allocate_guest_thread(DWORD entry,DWORD param,SIZE_T commit,SIZE_T reserve,ULONG flags)
{
    struct guest_thread_job *job=VirtualAlloc(NULL,sizeof(*job),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!job)return NULL;
    SIZE_T size=reserve;
    if(!size&&guest_peb&&((PEB32 *)guest_peb)->ImageBaseAddress)
    {
        BYTE *image=(void *)(ULONG_PTR)((PEB32 *)guest_peb)->ImageBaseAddress;
        IMAGE_NT_HEADERS32 *nt=(void *)(image+((IMAGE_DOS_HEADER *)image)->e_lfanew);
        size=nt->OptionalHeader.SizeOfStackReserve;
    }
    if(!size)size=1024*1024;
    if(size<commit)size=commit;
    if(size<64*1024)size=64*1024;
    size=(size+65535)&~(SIZE_T)65535;
    if(size>64*1024*1024){free_guest_thread(job);return NULL;}
    job->stack_size=size;job->stack=stack_alloc(0x38000000,size,commit?commit:0x10000,NULL);
    /* Wine stores per-thread debug buffers after its x86 TEB. */
    job->teb=low_alloc(0x39000000,0x3000,PAGE_READWRITE);
    if(!job->stack||!job->teb){free_guest_thread(job);return NULL;}
    BYTE *teb=job->teb,*stack=job->stack;
    stack_bind(stack,(TEB32 *)teb);
    memcpy(teb,guest_teb,0x1000);
    /* A new thread starts without the parent's current fiber or active
     * activation frames. Wine initializes and switches its own fiber state. */
    ((TEB32 *)teb)->Tib.FiberData=0;
    ((TEB32 *)teb)->SameTebFlags=0;
    memset(&((TEB32 *)teb)->ActivationContextStack,0,sizeof(((TEB32 *)teb)->ActivationContextStack));
    ((TEB32 *)teb)->Tib.ExceptionList=~0u;
    ((TEB32 *)teb)->StaticUnicodeString.Buffer=(DWORD)(uintptr_t)((TEB32 *)teb)->StaticUnicodeBuffer;
    ((TEB32 *)teb)->ActivationContextStackPointer=(DWORD)(uintptr_t)&((TEB32 *)teb)->ActivationContextStack;
    ((TEB32 *)teb)->ActivationContextStack.FrameListCache.Flink=((TEB32 *)teb)->ActivationContextStack.FrameListCache.Blink=
        (DWORD)(uintptr_t)&((TEB32 *)teb)->ActivationContextStack.FrameListCache;
    memset(teb+0xe10,0,64*sizeof(DWORD));
    *(DWORD *)(teb+0xf94)=0; /* TlsExpansionSlots */
    *(DWORD *)(teb+0x38)=0; /* CountOfOwnedCriticalSections */
    *(DWORD *)(teb+0x04)=(DWORD)(uintptr_t)(stack+size);
    *(DWORD *)(teb+0x08)=stack_limit(stack);
    *(DWORD *)(teb+0x18)=(DWORD)(uintptr_t)teb;
    *(DWORD *)(teb+0x24)=GetCurrentThreadId();
    *(DWORD *)(teb+0x2c)=(DWORD)(uintptr_t)(teb+0xe10);
    ((TEB32 *)teb)->ThreadLocalStoragePointer=0;((TEB32 *)teb)->FlsSlots=0;
    ((TEB32 *)teb)->LastErrorValue=0;((TEB32 *)teb)->DeallocationStack=(DWORD)(ULONG_PTR)stack;
    ((TEB32 *)teb)->SkipThreadAttach=(flags&THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH)!=0;
    job->wine_lifecycle=wine_thread_start!=NULL;
    struct guest_cpu *cpu=&job->cpu;cpu->guest_fs_base=(uintptr_t)teb;probe_cpu_prepare(cpu);
    ((WOW64_CPURESERVED *)cpu)->Machine=IMAGE_FILE_MACHINE_I386;
    DWORD *sp=(DWORD *)(stack+size-16);sp[0]=(DWORD)(uintptr_t)guest_return_gate;sp[1]=param;
    cpu->ctx.ContextFlags=CONTEXT_I386_FULL;
    cpu->ctx.Eip=job->wine_lifecycle?(DWORD)(ULONG_PTR)wine_thread_start:entry;cpu->ctx.Esp=(DWORD)(uintptr_t)sp;
    cpu->ctx.SegCs=cs32_sel;cpu->ctx.SegSs=ss32_sel;cpu->ctx.SegFs=guest_fs_sel;cpu->ctx.EFlags=0x202;
    cpu->ctx.Eax=entry;cpu->ctx.Ebx=param;
    return job;
}
static void WINAPI external_guest_thread_end(void *opaque)
{
    struct guest_thread_job *job=opaque;
    probe_io_drain_thread(job->id);
    if(wine_thread_detach)
    {
        job->cpu.ctx.Esp=(DWORD)(ULONG_PTR)(job->stack+job->stack_size-16);
        *(DWORD *)(ULONG_PTR)job->cpu.ctx.Esp=(DWORD)(ULONG_PTR)guest_return_gate;
        ((TEB32 *)job->teb)->Tib.ExceptionList=~0u;
        job->cpu.ctx.Eip=(DWORD)(ULONG_PTR)wine_thread_detach;
        probe_run32(&job->cpu.ctx);
    }
    probe_io_drain_thread(job->id);probe_exception_thread_cleanup();
    probe_thread_bind(NULL);free_guest_thread(job);
}
static DWORD callback_on_native_thread(DWORD function,const DWORD *args,UINT count)
{
    if(native_callback_slot==FLS_OUT_OF_INDEXES)return 0xffffffffu;
    struct guest_thread_job *job=FlsGetValue(native_callback_slot);
    if(!job)
    {
        job=allocate_guest_thread(0,0,0,0,0);if(!job)return 0xffffffffu;
        job->id=GetCurrentThreadId();((TEB32 *)job->teb)->ClientId.UniqueThread=job->id;
        ((TEB32 *)job->teb)->RealClientId=((TEB32 *)job->teb)->ClientId;
        job->record=probe_thread_register(GetCurrentThread(),(void *)job->teb,NULL,free_guest_thread_storage,job);
        if(!job->record){free_guest_thread(job);return 0xffffffffu;}
        probe_thread_bind(job->record);
        if(!FlsSetValue(native_callback_slot,job))
        { probe_thread_bind(NULL);free_guest_thread(job);return 0xffffffffu; }
        if(wine_thread_attach)
        {
            job->cpu.ctx.Eip=(DWORD)(ULONG_PTR)wine_thread_attach;
            if(probe_run32(&job->cpu.ctx))
            { FlsSetValue(native_callback_slot,NULL);external_guest_thread_end(job);return 0xffffffffu; }
        }
    }
    struct guest_cpu nested=job->cpu;
    DWORD address=((DWORD)(ULONG_PTR)(job->stack+job->stack_size)-(count+1)*sizeof(DWORD)-64)&~15u;
    DWORD *stack=(void *)(ULONG_PTR)address;stack[0]=(DWORD)(ULONG_PTR)guest_return_gate;
    for(UINT i=0;i<count;i++)stack[i+1]=args[i];
    nested.flags=0;nested.ctx.ContextFlags=CONTEXT_I386_FULL;
    nested.ctx.Eip=function;nested.ctx.Esp=address;
    return probe_run32(&nested.ctx);
}
static DWORD WINAPI guest_thread_entry(void *opaque)
{
    struct guest_thread_job *job=opaque;struct guest_cpu *cpu=&job->cpu;
    BYTE *teb=job->teb;DWORD result,*sp=(void *)(ULONG_PTR)cpu->ctx.Esp;
    probe_thread_bind(job->record);
    I386_CONTEXT entry_context=cpu->ctx;
    if(wine_thread_attach)
    {
        sp[0]=(DWORD)(ULONG_PTR)guest_return_gate;
        cpu->ctx.Eip=(DWORD)(uintptr_t)wine_thread_attach;
        if(probe_run32(&cpu->ctx)) { result=ERROR_DLL_INIT_FAILED;goto cleanup; }
        cpu->ctx=entry_context;
    }
    probe_thread_ready();
    if(wine_initial_apcs)
    {
        sp[0]=(DWORD)(ULONG_PTR)guest_return_gate;
        cpu->ctx.Eip=(DWORD)(ULONG_PTR)wine_initial_apcs;
        probe_run32(&cpu->ctx);cpu->ctx=entry_context;
    }
    /* Each call owns a new return frame. A completed call may overwrite
     * the consumed return slot, as the image gate does when saving EAX. */
    sp[0]=(DWORD)(ULONG_PTR)guest_return_gate;
    result=probe_run32(&cpu->ctx);
    if(wine_thread_detach&&!job->wine_lifecycle)
    {
        ((TEB32 *)teb)->Tib.ExceptionList=~0u;
        cpu->ctx=entry_context;cpu->ctx.Eip=(DWORD)(uintptr_t)wine_thread_detach;
        sp[0]=(DWORD)(uintptr_t)guest_return_gate;
        probe_run32(&cpu->ctx);
    }
cleanup:
    probe_io_drain_thread(GetCurrentThreadId());
    probe_exception_thread_cleanup();
    return result;
}
NTSTATUS WINAPI probe_NtCreateThreadEx(HANDLE *output,ACCESS_MASK access,OBJECT_ATTRIBUTES *attributes,HANDLE process,
                                     PRTL_THREAD_START_ROUTINE start,void *param,ULONG flags,ULONG_PTR zero_bits,
                                     SIZE_T commit,SIZE_T reserve,PS_ATTRIBUTE_LIST *list)
{
    (void)zero_bits;*output=NULL;
    if(process!=GetCurrentProcess()&&GetProcessId(process)!=GetCurrentProcessId())return STATUS_NOT_SUPPORTED;
    struct guest_thread_job *job=allocate_guest_thread((ULONG_PTR)start,(ULONG_PTR)param,commit,reserve,flags);
    if(!job)return STATUS_NO_MEMORY;
    job->wait=CreateThreadpoolWait(guest_thread_finished,job,NULL);
    if(!job->wait){free_guest_thread(job);return STATUS_NO_MEMORY;}
    NTSTATUS status=NtCreateThreadEx(&job->native,THREAD_ALL_ACCESS,attributes,process,(PRTL_THREAD_START_ROUTINE)guest_thread_entry,job,
                                    flags|THREAD_CREATE_FLAGS_CREATE_SUSPENDED,0,0,0,list);
    if(status){free_guest_thread(job);return status;}
    job->id=GetThreadId(job->native);((TEB32 *)job->teb)->ClientId.UniqueThread=job->id;
    ((TEB32 *)job->teb)->RealClientId=((TEB32 *)job->teb)->ClientId;
    job->record=probe_thread_register(job->native,(void *)job->teb,&job->cpu,free_guest_thread_storage,job);
    if(!job->record)status=STATUS_NO_MEMORY;
    if(!status)status=NtDuplicateObject(GetCurrentProcess(),job->native,GetCurrentProcess(),output,access,
                                       attributes?attributes->Attributes&OBJ_INHERIT:0,0);
    if(status)
    { NtTerminateThread(job->native,status);WaitForSingleObject(job->native,INFINITE);free_guest_thread(job);return status; }
    if(list)for(unsigned i=0;i<(list->TotalLength-sizeof(SIZE_T))/sizeof(PS_ATTRIBUTE);i++)
        if(list->Attributes[i].Attribute==PS_ATTRIBUTE_TEB_ADDRESS)
            *(TEB **)list->Attributes[i].ValuePtr=(TEB *)(job->teb-0x2000);
    HANDLE native=job->native;SetThreadpoolWait(job->wait,native,NULL);
    if(!(flags&THREAD_CREATE_FLAGS_CREATE_SUSPENDED))status=NtResumeThread(native,NULL);
    return status;
}
NTSTATUS WINAPI probe_NtTerminateThread(HANDLE thread,NTSTATUS code)
{
    struct guest_cpu *cpu=probe_current_cpu();
    if(cpu&&(thread==GetCurrentThread()||GetThreadId(thread)==GetCurrentThreadId()))
    {
        cpu->ctx.Eip=(DWORD)(ULONG_PTR)guest_return_gate;
        cpu->flags|=WOW64_CPURESERVED_FLAG_RESET_STATE;
        return code;
    }
    return NtTerminateThread(thread,code);
}
NTSTATUS (WINAPI *__imp_probe_NtCreateThreadEx)(HANDLE *,ACCESS_MASK,OBJECT_ATTRIBUTES *,HANDLE,PRTL_THREAD_START_ROUTINE,void *,ULONG,ULONG_PTR,SIZE_T,SIZE_T,PS_ATTRIBUTE_LIST *)=probe_NtCreateThreadEx;
NTSTATUS (WINAPI *__imp_probe_NtTerminateThread)(HANDLE,NTSTATUS)=probe_NtTerminateThread;
static NTSTATUS WINAPI bridge_get_cwd(void *opaque)
{
    struct bridge_args *a=opaque; DWORD n=GetCurrentDirectoryA(a->extra,(char *)(uintptr_t)a->value);
    a->result=n; return n ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
static NTSTATUS WINAPI bridge_get_tid(void *opaque)
{
    ((struct bridge_args *)opaque)->result=GetCurrentThreadId(); return STATUS_SUCCESS;
}
static NTSTATUS WINAPI bridge_get_heap(void *opaque)
{ ((struct bridge_args *)opaque)->result=(uint32_t)(uintptr_t)GetProcessHeap(); return STATUS_SUCCESS; }
static NTSTATUS WINAPI bridge_tls_alloc(void *opaque)
{ ((struct bridge_args *)opaque)->result=TlsAlloc(); return STATUS_SUCCESS; }
static NTSTATUS WINAPI bridge_tls_free(void *opaque)
{ ((struct bridge_args *)opaque)->result=TlsFree(((struct bridge_args *)opaque)->value); return STATUS_SUCCESS; }
static NTSTATUS WINAPI bridge_tls_get(void *opaque)
{ struct bridge_args *a=opaque; a->result=(uint32_t)(uintptr_t)TlsGetValue(a->value); return STATUS_SUCCESS; }
static NTSTATUS WINAPI bridge_tls_set(void *opaque)
{ struct bridge_args *a=opaque; a->result=TlsSetValue(a->value,(void *)(uintptr_t)a->high); return STATUS_SUCCESS; }
static NTSTATUS WINAPI bridge_create_thread(void *opaque)
{
    struct bridge_args *a=opaque;HANDLE h;
    NTSTATUS status=probe_NtCreateThreadEx(&h,THREAD_ALL_ACCESS,NULL,GetCurrentProcess(),
                                          (void *)(ULONG_PTR)a->value,(void *)(ULONG_PTR)a->high,0,0,0,a->extra,NULL);
    a->result=(ULONG_PTR)h;a->high=h?GetThreadId(h):0;return status;
}
static NTSTATUS WINAPI bridge_get_thread_exit_code(void *opaque)
{
    struct bridge_args *a=opaque; DWORD code=0;
    a->result=GetExitCodeThread((HANDLE)(uintptr_t)a->value,&code)?code:0xffffffffu;
    return STATUS_SUCCESS;
}
static NTSTATUS WINAPI bridge_thread_exit(void *opaque)
{
    struct bridge_args *a=opaque; ExitThread(a->value); return STATUS_SUCCESS;
}
/* The example native DLL is a test fixture, not a runtime dependency. */
static NTSTATUS WINAPI bridge_std_handle(void *opaque)
{ struct bridge_args *a=opaque;a->result=(ULONG_PTR)GetStdHandle(a->value);return STATUS_SUCCESS; }
static NTSTATUS WINAPI bridge_pid(void *opaque)
{ ((struct bridge_args *)opaque)->result=GetCurrentProcessId();return STATUS_SUCCESS; }
static NTSTATUS WINAPI bridge_ticks(void *opaque)
{ struct bridge_args *a=opaque;ULONGLONG ticks=GetTickCount64();a->result=ticks;a->high=ticks>>32;return STATUS_SUCCESS; }
static NTSTATUS WINAPI bridge_unavailable(void *opaque)
{ (void)opaque;return STATUS_NOT_SUPPORTED; }
int main(int argc,char **argv)
{
    WORD cs,ss,fs; BOOL wow=TRUE;
    struct guest_cpu cpu={0};
    _Static_assert(offsetof(typeof(cpu),ctx)==4,"Wine context layout");
    setvbuf(stdout,0,_IONBF,0);setvbuf(stderr,0,_IONBF,0);
    probe_trace=getenv("WINE_PROBE_TRACE")!=0;
    { MEMORY_BASIC_INFORMATION info={0};VirtualQuery((void *)0x400000,&info,sizeof(info));
      printf("EARLY_IMAGE reservation=%p owner=%p region=%p size=%llx state=%lx type=%lx\n",early_image_reservation,info.AllocationBase,info.BaseAddress,(unsigned long long)info.RegionSize,info.State,info.Type); }
    probe_backend_init();
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    AddVectoredExceptionHandler(1,probe_vectored_entry);
    IsWow64Process(GetCurrentProcess(),&wow);
    __asm__("movw %%cs,%0; movw %%ss,%1; movw %%fs,%2":"=r"(cs),"=r"(ss),"=r"(fs));
    guest_fs_sel=fs;
    probe_cpu_init(cs,ss,fs);
    native_callback_slot=FlsAlloc(external_guest_thread_end);
    if(native_callback_slot==FLS_OUT_OF_INDEXES)fail("native callback thread state");
    cs32_sel=0x23;ss32_sel=ss;
    printf("HOST pid=%lu wow64=%d cs=%x ss=%x fs=%x ptrbits=%u\n",GetCurrentProcessId(),wow,cs,ss,fs,(unsigned)(8*sizeof(void *)));
    { DWORD policy=0;typedef BOOL (WINAPI *query_t)(HANDLE,int,void *,SIZE_T);query_t q=(query_t)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetProcessMitigationPolicy");BOOL ok=q?q(GetCurrentProcess(),15,&policy,sizeof(policy)):FALSE;printf("SHADOW_STACK query=%d flags=%lx\n",ok,policy); }
    report_mitigations();
    if(wow||sizeof(void *)!=8) fail("native x64 host required");
    if(argc>1&&!strcmp(argv[1],"--stock"))
    {
        HMODULE m=LoadLibraryA("stock/wow64cpu.dll");
        printf("STOCK_LOAD handle=%p error=%lu\n",m,GetLastError());return m?0:2;
    }
    BOOL wine_components=(argc>1&&!strcmp(argv[1],"--wine-components"));
    const char *app_arg=wine_components ? (argc>2?argv[2]:"guest/hello32.exe") : (argc>1?argv[1]:"guest/hello32.exe");
    static void *builtin_table[BRIDGE_COUNT];
    for(unsigned i=0;i<BRIDGE_COUNT;i++)builtin_table[i]=(void *)bridge_unavailable;
    builtin_table[BRIDGE_GET_STD_HANDLE]=(void *)bridge_std_handle;
    builtin_table[BRIDGE_GET_PID]=(void *)bridge_pid;
    builtin_table[BRIDGE_GET_TICKS]=(void *)bridge_ticks;
    const char *native_path=getenv("WINE_NATIVE_BRIDGE");
    HMODULE native=native_path?LoadLibraryA(native_path):NULL;
    if(native_path&&!native)fail("load native bridge");
    void *table=builtin_table;
    if(native)
    {
        void **callback= (void **)GetProcAddress(native,"native_callback32");
        if(callback)*callback=probe_callback32;
        table=(void *)GetProcAddress(native,"__wine_unix_call_wow64_funcs");
        if(!table)fail("native dispatch table");
    }
    ((void **)table)[BRIDGE_GET_CWD]=(void *)bridge_get_cwd;
    ((void **)table)[BRIDGE_GET_TID]=(void *)bridge_get_tid;
    ((void **)table)[BRIDGE_GET_HEAP]=(void *)bridge_get_heap;
    ((void **)table)[BRIDGE_TLS_ALLOC]=(void *)bridge_tls_alloc;
    ((void **)table)[BRIDGE_TLS_FREE]=(void *)bridge_tls_free;
    ((void **)table)[BRIDGE_TLS_GET]=(void *)bridge_tls_get;
    ((void **)table)[BRIDGE_TLS_SET]=(void *)bridge_tls_set;
    ((void **)table)[BRIDGE_CREATE_THREAD]=(void *)bridge_create_thread;
    ((void **)table)[BRIDGE_GET_THREAD_EXIT_CODE]=(void *)bridge_get_thread_exit_code;
    ((void **)table)[BRIDGE_THREAD_EXIT]=(void *)bridge_thread_exit;
    printf("NATIVE64 module=%p table=%p\n",native,table);
    struct guest_module *nt=load_image(wine_components?"stock-wine/ntdll.dll":"guest/ntdll.dll","ntdll.dll");
    if(wine_components)
    {
        RTL_OSVERSIONINFOEXW *version=low_alloc(0x36000000,4096,PAGE_READWRITE);
        if(!version)fail("Wine version state");
        version->dwOSVersionInfoSize=sizeof(*version);
        if(RtlGetVersion(version))fail("query native version");
        /* Initialize Wine's original version routines through their data,
         * rather than replacing RtlGetVersion executable bytes. */
        *(DWORD *)(nt->base+WINE_NTDLL_current_version)=(DWORD)(ULONG_PTR)version;
    }
    struct guest_module *kb=wine_components?load_image("stock-wine/kernelbase.dll","kernelbase.dll"):0;
    struct guest_module *k32=load_image(wine_components?"stock-wine/kernel32.dll":"guest/kernel32.dll","kernel32.dll");
    struct guest_module *crt=wine_components?load_image("stock-wine/msvcrt.dll","msvcrt.dll"):0;
    struct guest_module *bridge=wine_components?load_image("guest/kernel32.dll","bridge_kernel32.dll"):0;
    struct guest_module *bootstrap=wine_components?load_image("guest/bootstrap32.dll","bootstrap32.dll"):0;
    const char *app_name=app_arg;for(const char *p=app_arg;*p;p++)if(*p=='/'||*p=='\\')app_name=p+1;
    struct guest_module app_data={.name=app_name},*app=&app_data;
    if(wine_components)
    {
        WCHAR relative[1024];MultiByteToWideChar(CP_ACP,0,app_arg,-1,relative,1024);
        if(!GetFullPathNameW(relative,1024,app->path,NULL))fail("application absolute path");
    }
    else
    {
        if(early_image_reservation) { VirtualFree(early_image_reservation,0,MEM_RELEASE);early_image_reservation=NULL; }
        app=load_image(app_arg,app_name);
    }
    init_guest_teb(app->base);
    int arg_start=wine_components?3:2;
    if(wine_components) init_guest_process_params(app,argc>arg_start?argc-arg_start:0,argv+(argc>arg_start?arg_start:argc));
    for(unsigned i=0;i<nmods;i++) if(!wine_components||&mods[i]!=app)resolve_imports(&mods[i]);
    struct far_thunk *gate=map_transition_gates(cs);
    guest_return_gate=&gate[2];
    probe_set_bop_code(&gate[0]);
    native_dispatch_table=table;guest_unix_gate=&gate[1];
    pBTCpuNotifyMapViewOfSection=bind_native_bridge;
    pBTCpuNotifyMemoryProtect=notify_stack_protection;
    ((TEB32 *)guest_teb)->WOW32Reserved=(DWORD)(uintptr_t)&gate[0];
    if(wine_components)
    {
        DWORD *hook=guest_export(nt,"__wine_windows_exit_process",0);
        DWORD *exit_gate=guest_export(bootstrap,"ProbeHostExitGate",0);
        void *exit=guest_export(bootstrap,"ProbeExitProcess",0);
        if(!hook||!exit_gate||!exit)fail("Wine process return hook");
        *exit_gate=(DWORD)(ULONG_PTR)&gate[2];*hook=(DWORD)(ULONG_PTR)exit;
    }
    if(wine_components)probe_exception_init(guest_export(nt,"KiUserExceptionDispatcher",0),ss);
    if(wine_components)probe_apc_init(guest_export(nt,"KiUserApcDispatcher",0));
    *(DWORD *)guest_export(nt,"__wine_syscall_dispatcher",0)=(DWORD)(uintptr_t)&gate[0];
    { void *u=guest_export(nt,"__wine_unix_call_dispatcher",0); if(u) *(DWORD *)(uintptr_t)u=(DWORD)(uintptr_t)&gate[1];
      void *h=guest_export(nt,"__wine_unixlib_handle",0); if(h) *(uint64_t *)(uintptr_t)h=wine_components?BRIDGE_NTDLL_UNIX_HANDLE:(uint64_t)(uintptr_t)table; }
    { struct guest_module *bk=wine_components?bridge:k32;
      *(uint64_t *)guest_export(bk,"bridge_handle",0)=(uint64_t)(uintptr_t)table;
      *(DWORD *)guest_export(bk,"bridge_gate",0)=(DWORD)(uintptr_t)&gate[1];
      *(DWORD *)guest_export(bk,"host_exit_gate",0)=(DWORD)(uintptr_t)&gate[2]; }
    __wine_unix_call_dispatcher=(void **)(uintptr_t)probe_library_dispatch;
    if(wine_components)
    {
        DWORD args[]={HEAP_GROWABLE,0,0,0,0,0};
        DWORD heap=call_guest_stdcall(nt,guest_export(nt,"RtlCreateHeap",0),&gate[2],args,6);
        if(!heap) fail("Wine process heap");
        *(DWORD *)(guest_peb+0x18)=heap;
        printf("WINE_HEAP=%08lx\n",(unsigned long)heap);
        call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeInitRuntime",0),&gate[2],NULL,0);
        DWORD loader_args[]={(DWORD)(uintptr_t)nt->base};
        call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeInitLoader",0),&gate[2],loader_args,1);
        WCHAR *module_path=low_alloc(0x3f000000,4096,PAGE_READWRITE);
        if(!module_path)fail("loader path buffer");
        for(unsigned i=0;i<nmods;i++)
        {
            swprintf(module_path,2048,L"\\??\\%s",mods[i].path);
            if(&mods[i]==nt||&mods[i]==kb||&mods[i]==k32||&mods[i]==crt)
                swprintf(module_path,2048,L"\\??\\C:\\windows\\system32\\%hs",mods[i].name);
            DWORD args[]={(DWORD)(uintptr_t)mods[i].base,(DWORD)(uintptr_t)module_path,mods[i].base==nt->base?1:mods[i].base==k32->base?2:&mods[i]==app?3:0};
            if(call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeRegisterModule",0),&gate[2],args,3))fail("Wine module registration");
        }
        if(!GetFullPathNameW(L"stock-wine",2048,module_path,NULL))fail("Wine DLL directory");
        DWORD directory_args[]={(DWORD)(uintptr_t)module_path};
        if(call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeSetDllDirectory",0),&gate[2],directory_args,1))fail("Wine dynamic DLL directory");
        wcscpy(module_path,app->path);
        DWORD *application_base=(DWORD *)((BYTE *)module_path+3072);
        DWORD load_args[]={(DWORD)(uintptr_t)module_path,(DWORD)(uintptr_t)application_base};
        if(early_image_reservation) { VirtualFree(early_image_reservation,0,MEM_RELEASE);early_image_reservation=NULL; }
        DWORD load_status=call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeLoadApplication",0),&gate[2],load_args,2);
        printf("WINE_LOAD_APPLICATION status=%08lx base=%08lx\n",load_status,*application_base);
        if(load_status)fail("Wine application mapping");
        app->base=(BYTE *)(uintptr_t)*application_base;
        app->nt=(void *)(app->base+((IMAGE_DOS_HEADER *)app->base)->e_lfanew);
        VirtualFree(module_path,0,MEM_RELEASE);
        if((ULONG_PTR)app->base!=app->nt->OptionalHeader.ImageBase)
        {
            require_image_relocation_policy();
            DWORD relocation=call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeRelocateApplication",0),&gate[2],NULL,0);
            printf("WINE_RELOCATE_APPLICATION status=%08lx canonical=%08lx base=%p\n",relocation,app->nt->OptionalHeader.ImageBase,app->base);
            if(relocation){SetLastError(RtlNtStatusToDosError(relocation));fail("Wine application relocation");}
            FlushInstructionCache(GetCurrentProcess(),app->base,app->nt->OptionalHeader.SizeOfImage);
        }
        DWORD params_status=call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeInitProcessParameters",0),&gate[2],NULL,0);
        printf("WINE_PROCESS_PARAMETERS status=%08lx\n",params_status);if(params_status)fail("Wine process parameters initialization");
        DWORD import_status=call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeFixupImports",0),&gate[2],NULL,0);
        printf("WINE_IMPORTS status=%08lx\n",import_status);if(import_status)fail("Wine application imports");
        if(crt)
        {
            DWORD kb_initialized=call_guest_dllmain(kb,&gate[2]);
            printf("WINE_KERNELBASE_ATTACH=%lu\n",kb_initialized);if(!kb_initialized)fail("Wine kernelbase initialization");
            DWORD k32_initialized=call_guest_dllmain(k32,&gate[2]);
            printf("WINE_KERNEL32_ATTACH=%lu\n",k32_initialized);if(!k32_initialized)fail("Wine kernel32 initialization");
            DWORD initialized=call_guest_dllmain(crt,&gate[2]);
            printf("WINE_CRT_ATTACH=%lu\n",initialized);
            if(!initialized) fail("Wine CRT initialization");
        }
        DWORD attach_status=call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeAttachImports",0),&gate[2],NULL,0);
        printf("WINE_ATTACH status=%08lx\n",attach_status);if(attach_status)fail("Wine application dependency attach");
        wine_thread_attach=guest_export(bootstrap,"ProbeInitThread",0);
        wine_thread_start=guest_export(nt,"RtlUserThreadStart",0);
        wine_initial_apcs=guest_export(bootstrap,"ProbeInitialApcs",0);
        wine_thread_detach=guest_export(bootstrap,"ProbeShutdownThread",0);
    }
    SIZE_T stack_size=app->nt->OptionalHeader.SizeOfStackReserve;
    if(stack_size<256*1024)stack_size=256*1024;
    if(stack_size>64*1024*1024)fail("guest stack reserve limit");
    BYTE *stack=stack_alloc(0x30000000,stack_size,app->nt->OptionalHeader.SizeOfStackCommit,(TEB32 *)guest_teb);
    if(!stack) fail("guest stack");
    *(DWORD *)(guest_teb+0x04)=(DWORD)(uintptr_t)(stack+stack_size);
    *(DWORD *)(guest_teb+0x08)=stack_limit(stack);
    ((TEB32 *)guest_teb)->DeallocationStack=(DWORD)(ULONG_PTR)stack;
    cpu.guest_fs_base=(uintptr_t)guest_teb;
    DWORD *sp=(void *)(stack+stack_size-16);sp[0]=(DWORD)(uintptr_t)&gate[2];
    cs32_sel=0x23;ss32_sel=ss;
    cpu.flags=0;cpu.ctx.ContextFlags=CONTEXT_I386_FULL;
    cpu.ctx.Eip=(DWORD)(uintptr_t)(app->base+app->nt->OptionalHeader.AddressOfEntryPoint);
    cpu.ctx.Esp=(DWORD)(uintptr_t)sp;cpu.ctx.SegCs=0x23;cpu.ctx.SegSs=ss;cpu.ctx.SegFs=guest_fs_sel;cpu.ctx.EFlags=0x202;
    DWORD result=probe_run32(&cpu.ctx);
    if(wine_components)
    {
        DWORD flush_args[]={0};
        call_guest_stdcall(crt,guest_export(crt,"fflush",0),&gate[2],flush_args,1);
        call_guest_stdcall(bootstrap,guest_export(bootstrap,"ProbeShutdownProcess",0),&gate[2],NULL,0);
    }
    printf("GUEST_EXIT code=%lu nt_calls=%lu native_dll_calls=%lu fs_repairs=%lu HOST_RETURNED\n",result,probe_nt_calls,probe_library_calls,probe_fs_repairs);
    return result;
}
