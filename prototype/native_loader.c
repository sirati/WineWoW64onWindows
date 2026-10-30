/* Native Windows services for Wine's original builtin unixlib lookup/cache.
 * Guest PE mapping, import resolution and DllMain stay in Wine's PE loader. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wine/list.h"
#include "native_loader.h"
extern BOOL probe_socket_builtin_unixlib(const char *,ULONGLONG *);
extern void probe_diagnostic(const char *format,...);

static CRITICAL_SECTION virtual_mutex;
static const char *dll_dir,*build_dir,*alt_build_dir;
static const USHORT current_machine=IMAGE_FILE_MACHINE_AMD64;
static const char **dll_paths;
static char *dll_path_environment;
static WCHAR runtime_directory[32768];
static size_t dll_path_maxlen;
/* Wine's PE builtins are not all paired with a Unixlib.  In the normal Unix
 * build, a builtin which has no <arch>-unix companion simply has no Unix-call
 * table; loading the PE itself is still valid.  Keep a bounded table for that
 * case on Windows.  Calls made by a PE which genuinely needs a Unix backend
 * receive Wine's normal unsupported-service status, while the loader does
 * not turn an otherwise usable builtin into STATUS_DLL_NOT_FOUND. */
#define OPTIONAL_UNIXLIB_ENTRIES 4096
static NTSTATUS WINAPI optional_unixlib_call(void *args)
{
    (void)args;
    return STATUS_NOT_SUPPORTED;
}
static void *optional_unixlib_table[OPTIONAL_UNIXLIB_ENTRIES];
static BOOL optional_unixlib_initialized;
typedef int sigset_t;
#define server_enter_uninterrupted_section(lock,mask) EnterCriticalSection(lock)
#define server_leave_uninterrupted_section(lock,mask) LeaveCriticalSection(lock)
#define RTLD_NOW 0
#define WARN_(channel) WARN
#define WARN(...) do {} while(0)
#include "out/wine-native-dos-path.inc"

static WCHAR *wide_path(const char *path)
{
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,NULL,0);
    WCHAR *result=count?malloc(count*sizeof(WCHAR)):NULL;
    if(result)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,result,count);
    if(result)for(WCHAR *p=result;*p;p++)if(*p==L'/')*p=L'\\';
    WCHAR *dos=result;
    if(result&&!wcsncmp(result,L"\\??\\",4))dos=get_dos_path(result);
    DWORD needed=dos?GetFullPathNameW(dos,0,NULL,NULL):0;
    WCHAR *absolute=needed?malloc(needed*sizeof(WCHAR)):NULL;
    if(absolute&&(!GetFullPathNameW(dos,needed,absolute,NULL)))
    {free(absolute);absolute=NULL;}
    free(result);return absolute;
}
static char *utf8_path(const WCHAR *path)
{
    int count=WideCharToMultiByte(CP_UTF8,0,path,-1,NULL,0,NULL,NULL);
    char *result=count?malloc(count):NULL;
    if(result)WideCharToMultiByte(CP_UTF8,0,path,-1,result,count,NULL,NULL);
    return result;
}
/* Windows binds a module's imports to any loaded module with the same base
 * name, so a native library named like a system DLL would capture other
 * modules' imports of that DLL, as an opengl32.dll unixlib did. Refuse it.
 * The absolute path also supplies the directory for native dependencies. */
HMODULE probe_load_native_library(const WCHAR *path)
{
    WCHAR system[MAX_PATH*2];
    const WCHAR *base=wcsrchr(path,L'\\');
    UINT length=GetSystemDirectoryW(system,MAX_PATH);
    base=base?base+1:path;
    if(!length||length>=MAX_PATH||length+1+wcslen(base)>=ARRAY_SIZE(system))
    {SetLastError(ERROR_FILENAME_EXCED_RANGE);return NULL;}
    wcscat(system,L"\\");wcscat(system,base);
    if(GetFileAttributesW(system)!=INVALID_FILE_ATTRIBUTES)
    {
        probe_diagnostic("NATIVE_LIBRARY_SHADOWS_SYSTEM_DLL path=%ls system=%ls\n",path,system);
        SetLastError(ERROR_ALREADY_EXISTS);
        return NULL;
    }
    return LoadLibraryExW(path,NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
}
static void *native_dlopen(const char *path,int flags)
{
    WCHAR *name=wide_path(path);(void)flags;
    /* No Wine guest DLL is passed to the Windows loader. */
    HMODULE module=name?probe_load_native_library(name):NULL;
    if(!module)probe_diagnostic("NATIVE_UNIXLIB_LOAD_FAILED path=%s error=%lu\n",path,GetLastError());
    free(name);return module;
}
#define dlopen native_dlopen
#define dlsym(module,name) ((void *)GetProcAddress(module,name))
#define dlclose(module) FreeLibrary(module)
#include "out/wine-unixlib-cache.inc"

static BOOL native_unixlib_exists(const char *path)
{
    WCHAR *name=wide_path(path);
    DWORD attributes=name?GetFileAttributesW(name):INVALID_FILE_ATTRIBUTES;
    free(name);
    return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_DIRECTORY);
}
/* As in find_builtin_dll, a unixlib is named <module>.so. That name also
 * keeps it apart from system DLLs: the Windows loader binds an import to any
 * loaded module with the same base name, so a native opengl32.dll unixlib
 * would capture a later native module's opengl32.dll import. A unixlib under
 * the module's own name is still found. ext points into path. */
static BOOL find_unixlib_file(char *path,char *ext)
{
    char original[16];
    if(!ext||strlen(ext)<3||strlen(ext)>=sizeof(original))return native_unixlib_exists(path);
    strcpy(original,ext);
    strcpy(ext,".so");
    if(native_unixlib_exists(path))return TRUE;
    strcpy(ext,original);
    return native_unixlib_exists(path);
}
#include "out/wine-unixlib-paths.inc"

static BOOL is_stock_wine_directory(const char *path)
{
    static const char suffix[] = "\\stock-wine";
    size_t length = path ? strlen(path) : 0;
    return length >= sizeof(suffix) - 1 &&
           !_stricmp(path + length - (sizeof(suffix) - 1), suffix);
}

static char *environment_path(const WCHAR *name)
{
    DWORD count=GetEnvironmentVariableW(name,NULL,0);
    WCHAR *value=count?malloc(count*sizeof(WCHAR)):NULL;
    char *result=NULL;
    if(value&&GetEnvironmentVariableW(name,value,count)<count)result=utf8_path(value);
    free(value);return result;
}
static void publish_dll_directory(const WCHAR *variable,const char *path)
{
    WCHAR *directory=wide_path(path);
    UNICODE_STRING nt_directory;
    if(!directory||RtlDosPathNameToNtPathName_U_WithStatus(directory,&nt_directory,NULL,NULL))ExitProcess(94);
    if(!SetEnvironmentVariableW(variable,nt_directory.Buffer))ExitProcess(94);
    RtlFreeUnicodeString(&nt_directory);free(directory);
}
void probe_loader_init(void)
{
    WCHAR *path=runtime_directory;
    DWORD count=GetModuleFileNameW(NULL,path,ARRAY_SIZE(runtime_directory));
    if(!count||count>=ARRAY_SIZE(runtime_directory))ExitProcess(94);
    WCHAR *tail=wcsrchr(path,L'\\');if(!tail)ExitProcess(94);*tail=0;
    /* Like Wine's so_dir, x86_64-native is relative to the installed host.
     * Existing packaged PEs also remain on the stock-wine search path. */
    dll_dir=utf8_path(path);
    dll_path_environment=environment_path(L"WINEDLLPATH");
    build_dir=environment_path(L"WINEBUILDDIR");
    if(!dll_dir)ExitProcess(94);
    InitializeCriticalSection(&virtual_mutex);
    for(unsigned i=0;i<ARRAY_SIZE(optional_unixlib_table);i++)
        optional_unixlib_table[i]=(void *)optional_unixlib_call;
    optional_unixlib_initialized=TRUE;
    set_dll_path();
    /* Wine's PE fallback finder consumes WINEDLLDIRn, which its Unix loader
     * normally publishes from this same dll_paths array.  There is no prefix
     * containing placeholder DLLs on this native host, so use Wine's original
     * prefix-bootstrap search for missing logical system DLL files. */
    unsigned i;
    for(i=0;dll_paths[i];i++)
    {
        WCHAR variable[32];
        swprintf(variable,ARRAY_SIZE(variable),L"WINEDLLDIR%u",i);
        publish_dll_directory(variable,dll_paths[i]);
    }
    WCHAR variable[32];swprintf(variable,ARRAY_SIZE(variable),L"WINEDLLDIR%u",i);
    SetEnvironmentVariableW(variable,NULL);
    if(build_dir)publish_dll_directory(L"WINEBUILDDIR",build_dir);
}
BOOL probe_runtime_path(WCHAR *path,SIZE_T capacity,const WCHAR *relative)
{
    if(wcslen(runtime_directory)+wcslen(relative)+2>capacity)return FALSE;
    wcscpy(path,runtime_directory);wcscat(path,L"\\");wcscat(path,relative);
    return TRUE;
}
BOOL probe_loader_append_paths(WCHAR *path,SIZE_T capacity)
{
    /* Feed Wine's own search_dll_file default path. As in
     * find_builtin_without_file, try the architecture dir before the root. */
    for(unsigned i=0;dll_paths[i];i++)
    {
        char *name=build_path(dll_paths[i],get_pe_dir(IMAGE_FILE_MACHINE_I386));
        WCHAR *directory=name?wide_path(name):NULL;free(name);
        if(!directory)return FALSE;
        if(wcslen(path)+wcslen(directory)+2>capacity){free(directory);return FALSE;}
        wcscat(path,L";");wcscat(path,directory);free(directory);
        directory=wide_path(dll_paths[i]);if(!directory)return FALSE;
        if(wcslen(path)+wcslen(directory)+2>capacity){free(directory);return FALSE;}
        wcscat(path,L";");wcscat(path,directory);free(directory);
    }
    return TRUE;
}

static char *mapped_builtin_directory(void *module)
{
    SIZE_T size=sizeof(UNICODE_STRING)+32768*sizeof(WCHAR),returned;
    UNICODE_STRING *name=malloc(size);
    if(!name)return NULL;
    char *root=NULL;
    if(!NtQueryVirtualMemory(GetCurrentProcess(),module,MemoryMappedFilenameInformation,name,size,&returned))
    {
        /* A native section name is \Device\..., accepted by the Win32
         * loader through GLOBALROOT. It need not be a native loader module. */
        WCHAR path[32768];
        if(name->Length/sizeof(WCHAR)+15<ARRAY_SIZE(path))
        {
            wcscpy(path,L"\\\\?\\GLOBALROOT");
            size_t offset=wcslen(path);
            memcpy(path+offset,name->Buffer,name->Length);
            path[offset+name->Length/sizeof(WCHAR)]=0;
            WCHAR *tail=wcsrchr(path,L'\\');
            if(tail)
            {
                *tail=0;tail=wcsrchr(path,L'\\');
                if(tail&&!wcsicmp(tail+1,L"i386-windows"))*tail=0;
                root=utf8_path(path);
            }
        }
    }
    free(name);return root;
}
NTSTATUS probe_builtin_unixlib(void *module,ULONGLONG *handle)
{
    MEMORY_BASIC_INFORMATION info;
    if(!VirtualQuery(module,&info,sizeof(info))||info.AllocationBase!=module||info.Type!=MEM_IMAGE)
        return STATUS_DLL_NOT_FOUND;
    IMAGE_DOS_HEADER *dos=module;
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return STATUS_DLL_NOT_FOUND;
    IMAGE_NT_HEADERS32 *nt=(void *)((BYTE *)module+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386)
        return STATUS_DLL_NOT_FOUND;
    ULONG size;
    IMAGE_EXPORT_DIRECTORY *exports=RtlImageDirectoryEntryToData(module,TRUE,IMAGE_DIRECTORY_ENTRY_EXPORT,&size);
    if(!exports||!exports->Name||exports->Name>=nt->OptionalHeader.SizeOfImage)return STATUS_DLL_NOT_FOUND;
    const char *name=(char *)module+exports->Name;
    SIZE_T length=strnlen(name,nt->OptionalHeader.SizeOfImage-exports->Name);
    if(length==nt->OptionalHeader.SizeOfImage-exports->Name||length>MAX_PATH)return STATUS_DLL_NOT_FOUND;
    if(probe_socket_builtin_unixlib(name,handle))return STATUS_SUCCESS;
    if(dos->e_lfanew<sizeof(*dos)+sizeof("Wine builtin DLL")||
       memcmp(dos+1,"Wine builtin DLL",sizeof("Wine builtin DLL")))return STATUS_DLL_NOT_FOUND;
#ifdef PROBE_TEST_NO_NATIVE_UNIXLIB
    /* Negative-control executable only. Keep the same platform fixes and
     * socket backend, with generic native unixlib lookup disabled. */
    probe_diagnostic("MISSING_NATIVE_UNIXLIB module=%s\n",name);
    return STATUS_DLL_NOT_FOUND;
#endif
    ANSI_STRING exp_name={(USHORT)length,(USHORT)(length+1),(char *)name};
    UNICODE_STRING nt_name={0};
    const void *funcs=NULL;
    BOOL optional=FALSE;
    EnterCriticalSection(&virtual_mutex);
    struct builtin_module *builtin;
    BOOL present=FALSE;
    LIST_FOR_EACH_ENTRY(builtin,&builtin_modules,struct builtin_module,entry)
        if(builtin->module==module){present=TRUE;break;}
    if(!present)
    {
        add_builtin_module(module,NULL);
        char *mapped_root=mapped_builtin_directory(module);
        if(mapped_root)
        {
            char *directory=build_path(mapped_root,get_so_dir(IMAGE_FILE_MACHINE_AMD64));
            char *path=directory?build_path(directory,name):NULL;
            const char *dot=strrchr(name,'.');
            char *ext=path&&dot?path+strlen(path)-strlen(dot):NULL;
            if(path&&find_unixlib_file(path,ext))load_builtin_unixlib(module,path);
            free(path);free(directory);free(mapped_root);
        }
        if(find_native_unixlib(module,&nt_name,&exp_name)==STATUS_NO_MEMORY)
        {LeaveCriticalSection(&virtual_mutex);return STATUS_NO_MEMORY;}
    }
    /* Re-read the mapped directory for already-cached modules as well.  Wine
     * may query the same builtin more than once during import/attach, and
     * the optional no-unixlib rule must remain stable across those queries. */
    char *root=mapped_builtin_directory(module);
    if(root)
    {
        optional=is_stock_wine_directory(root);
        free(root);
    }
    NTSTATUS status=get_builtin_unix_funcs(module,TRUE,&funcs);
    LeaveCriticalSection(&virtual_mutex);
    if(!status)*handle=(ULONG_PTR)funcs;
    else if(status==STATUS_DLL_NOT_FOUND && optional && optional_unixlib_initialized)
    {
        /* Stock Wine PE builtins without a native companion remain loadable.
         * This is the same absence represented by a NULL Unix handle in
         * Wine's own loader; the bounded table only gives the Windows
         * dispatcher a safe status-returning target if the PE asks for one. */
        *handle=(ULONG_PTR)optional_unixlib_table;
        status=STATUS_SUCCESS;
    }
    if(status)probe_diagnostic("MISSING_NATIVE_UNIXLIB %s status=%08lx\n",name,status);
    return status;
}
void WINAPI probe_unixlib_unmap(void *module,BOOL post,NTSTATUS status)
{
    if(!post||status<0)return;
    EnterCriticalSection(&virtual_mutex);
    release_builtin_module(module);
    LeaveCriticalSection(&virtual_mutex);
}
NTSTATUS WINAPI probe_NtQueryVirtualMemory(HANDLE process,const void *address,MEMORY_INFORMATION_CLASS class,
                                          void *buffer,SIZE_T size,SIZE_T *returned)
{
    if(class==MemoryWineUnixWow64Funcs||class==MemoryWineUnixFuncs)
    {
        if(size!=sizeof(ULONGLONG))return STATUS_INFO_LENGTH_MISMATCH;
        if(process!=GetCurrentProcess())return STATUS_INVALID_HANDLE;
        NTSTATUS status=probe_builtin_unixlib((void *)address,buffer);
        if(!status&&returned)*returned=sizeof(ULONGLONG);
        return status;
    }
    return NtQueryVirtualMemory(process,address,class,buffer,size,returned);
}
NTSTATUS (WINAPI *__imp_probe_NtQueryVirtualMemory)(HANDLE,const void *,MEMORY_INFORMATION_CLASS,void *,SIZE_T,SIZE_T *)=probe_NtQueryVirtualMemory;
