/* Native services expected by Wine's Unix-side win32u and wow64win.
 * Wine's win32u runs in the x64 host with its null display driver; guest
 * GUI calls never reach the host's win32u. This file supplies the ntdll
 * Unix interfaces win32u uses: a 64-bit TEB/PEB shared with the x86 guest,
 * user-mode callbacks, last-error propagation, a private registry root and
 * the client side of Wine's directory and atom requests. The server side
 * of the protocol is gui_server.c. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntuser.h"
#include "wine/unixlib.h"
#include "wine/server.h"
#include "wine/debug.h"
#include <pthread.h>
#include <io.h>

WINE_DEFAULT_DEBUG_CHANNEL(win32u);

extern TEB32 *probe_current_teb32(void);
extern ULONG_PTR default_zero_bits;
extern ntuser_callback user_callbacks[];
extern RTL_USER_PROCESS_PARAMETERS *probe_gui_process_parameters(const RTL_USER_PROCESS_PARAMETERS32 *);
extern HANDLE probe_server_session_section(void);
extern HKEY reg_create_ascii_key(HKEY,const char *,DWORD,DWORD *);
extern void set_reg_ascii_value(HKEY,const char *,const char *);
extern BOOL probe_runtime_path(WCHAR *,SIZE_T,const WCHAR *);

/* win32u/syscall.c is not used: the x86 service table is wow64win's. */
ULONG_PTR zero_bits = 0x7fffffff;

static inline DWORD native_tid(void)
{ DWORD64 tid;__asm__("movq %%gs:0x48,%0":"=r"(tid));return (DWORD)tid; }
static inline DWORD native_pid(void)
{ DWORD64 pid;__asm__("movq %%gs:0x40,%0":"=r"(pid));return (DWORD)pid; }

/* pthread subset over SRW locks. Wine's recursive mutex attribute is
 * honoured for every mutex, which is compatible with its non-recursive use. */
int pthread_mutex_init(pthread_mutex_t *m,const pthread_mutexattr_t *a) { (void)a;memset(m,0,sizeof(*m));return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m;return 0; }
int pthread_mutex_lock(pthread_mutex_t *m)
{
    DWORD self=native_tid();
    if(ReadAcquire((LONG *)&m->owner)==(LONG)self){m->depth++;return 0;}
    AcquireSRWLockExclusive((SRWLOCK *)&m->lock);
    WriteRelease((LONG *)&m->owner,self);m->depth=1;
    return 0;
}
int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if(--m->depth)return 0;
    WriteRelease((LONG *)&m->owner,0);
    ReleaseSRWLockExclusive((SRWLOCK *)&m->lock);
    return 0;
}
int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->type=0;return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a,int type) { a->type=type;return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a;return 0; }
static BOOL CALLBACK run_once(INIT_ONCE *once,void *func,void **context)
{ (void)once;(void)context;((void (*)(void))func)();return TRUE; }
int pthread_once(pthread_once_t *once,void (*func)(void))
{ InitOnceExecuteOnce((INIT_ONCE *)once,run_once,(void *)func,NULL);return 0; }
int pthread_rwlock_init(pthread_rwlock_t *l,const void *a) { (void)a;memset(l,0,sizeof(*l));return 0; }
int pthread_rwlock_destroy(pthread_rwlock_t *l) { (void)l;return 0; }
int pthread_rwlock_rdlock(pthread_rwlock_t *l) { AcquireSRWLockShared((SRWLOCK *)&l->lock);return 0; }
int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{ AcquireSRWLockExclusive((SRWLOCK *)&l->lock);WriteRelease((LONG *)&l->writer,native_tid());return 0; }
int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    if(ReadAcquire((LONG *)&l->writer)==(LONG)native_tid())
    { WriteRelease((LONG *)&l->writer,0);ReleaseSRWLockExclusive((SRWLOCK *)&l->lock); }
    else ReleaseSRWLockShared((SRWLOCK *)&l->lock);
    return 0;
}

/* Wine's 64-bit TEB/PEB for the guest thread. In Wine's WoW64 layout the
 * x86 TEB GdiBatchCount holds this TEB address: x86 user32 and gdi32 read
 * its Win32ClientInfo and the PEB's GDI handle table directly. Both blocks
 * stay below 4 GiB and are separate from the host's native TEB and PEB. */
struct gui_callback { struct gui_callback *prev;void *ret_ptr;ULONG ret_len; };
struct gui_thread
{
    TEB teb;
    struct gui_callback *callback;
    void **results;
    unsigned int depth,result_count;
};
static PEB *gui_peb;
static void *low_allocate(SIZE_T size)
{
    void *ptr=NULL;
    if(NtAllocateVirtualMemory(NtCurrentProcess(),&ptr,0x7fffffff,&size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE))return NULL;
    return ptr;
}
BOOL probe_gui_attach_teb(TEB32 *teb32)
{
    if(!gui_peb&&!(gui_peb=low_allocate(sizeof(*gui_peb))))return FALSE;
    struct gui_thread *thread=low_allocate(sizeof(*thread));
    if(!thread)return FALSE;
    TEB *teb=&thread->teb;
    teb->Tib.Self=&teb->Tib;
    teb->Peb=gui_peb;
    teb->ClientId.UniqueProcess=ULongToHandle(native_pid());
    teb->WowTebOffset=(LONG)((char *)teb32-(char *)teb);
    /* Wine kernelbase redirects system32 when this TEB exists. Keep the
     * port's existing path mapping unless the guest enables redirection. */
    teb->TlsSlots[WOW64_TLS_FILESYSREDIR]=(void *)TRUE;
    teb32->GdiBatchCount=PtrToUlong(teb);
    return TRUE;
}
void probe_gui_detach_teb(TEB32 *teb32)
{
    struct gui_thread *thread=ULongToPtr(teb32->GdiBatchCount);
    if(!thread)return;
    teb32->GdiBatchCount=0;
    for(unsigned int i=0;i<thread->result_count;i++)free(thread->results[i]);
    free(thread->results);
    SIZE_T size=0;void *ptr=thread;
    NtFreeVirtualMemory(NtCurrentProcess(),&ptr,&size,MEM_RELEASE);
}
TEB * WINAPI NtCurrentTeb(void)
{
    TEB32 *teb32=probe_current_teb32();
    TEB *teb=teb32?ULongToPtr(teb32->GdiBatchCount):NULL;
    if(!teb){fputs("GUI call without a guest TEB\n",stderr);ExitProcess(94);}
    /* Guest TEBs are prepared before their native thread exists. */
    if(!teb->ClientId.UniqueThread)teb->ClientId.UniqueThread=ULongToHandle(native_tid());
    return teb;
}
static struct gui_thread *current_gui_thread(void) { return CONTAINING_RECORD(NtCurrentTeb(),struct gui_thread,teb); }

/* Fill the shared PEB once the guest process parameters exist. Wine's x86
 * locale_init has already stored the code page tables through this PEB. */
void probe_gui_process_init(const PEB32 *peb32)
{
    const RTL_USER_PROCESS_PARAMETERS32 *params32=ULongToPtr(peb32->ProcessParameters);
    gui_peb->ImageBaseAddress=ULongToPtr(peb32->ImageBaseAddress);
    gui_peb->OSMajorVersion=peb32->OSMajorVersion;gui_peb->OSMinorVersion=peb32->OSMinorVersion;
    gui_peb->OSBuildNumber=peb32->OSBuildNumber;gui_peb->OSPlatformId=peb32->OSPlatformId;
    gui_peb->NumberOfProcessors=peb32->NumberOfProcessors;
    /* The in-process server's session, Wine's default_session_id. */
    gui_peb->SessionId=1;
    if(!gui_peb->AnsiCodePageData)gui_peb->AnsiCodePageData=ULongToPtr(peb32->AnsiCodePageData);
    if(!gui_peb->OemCodePageData)gui_peb->OemCodePageData=ULongToPtr(peb32->OemCodePageData);
    if(!gui_peb->UnicodeCaseTableData)gui_peb->UnicodeCaseTableData=ULongToPtr(peb32->UnicodeCaseTableData);
    if(params32&&!(gui_peb->ProcessParameters=probe_gui_process_parameters(params32)))ExitProcess(94);
}

/* Wine's Unix RtlSetLastWin32Error updates both the native and WoW TEB. */
DWORD WINAPI probe_gui_RtlGetLastWin32Error(void) { return NtCurrentTeb()->LastErrorValue; }
void WINAPI probe_gui_RtlSetLastWin32Error(DWORD error)
{
    TEB *teb=NtCurrentTeb();
    teb->LastErrorValue=error;
    ((TEB32 *)((char *)teb+teb->WowTebOffset))->LastErrorValue=error;
}

/* KeUserModeCallback calls wow64win's 64-bit callback, which runs the x86
 * dispatcher through Wow64KiUserCallbackDispatcher and finishes with
 * NtCallbackReturn. That call returns here rather than switching stacks.
 * Its result can be a local of the finished callback, so a copy is kept
 * until the next callback at the same depth, as Wine's stack would be. */
NTSTATUS KeUserModeCallback(ULONG id,const void *args,ULONG len,void **ret_ptr,ULONG *ret_len)
{
    struct gui_thread *thread=current_gui_thread();
    struct gui_callback frame={thread->callback,NULL,0};
    if(id>=NtUserCallCount||!user_callbacks[id])return STATUS_INVALID_PARAMETER;
    if(thread->depth>=thread->result_count)
    {
        void **results=realloc(thread->results,(thread->depth+16)*sizeof(*results));
        if(!results)return STATUS_NO_MEMORY;
        memset(results+thread->result_count,0,(thread->depth+16-thread->result_count)*sizeof(*results));
        thread->results=results;thread->result_count=thread->depth+16;
    }
    free(thread->results[thread->depth]);thread->results[thread->depth]=NULL;
    thread->callback=&frame;thread->depth++;
    NTSTATUS status=user_callbacks[id]((void *)args,len);
    thread->depth--;thread->callback=frame.prev;
    *ret_ptr=frame.ret_ptr;*ret_len=frame.ret_len;
    return status;
}
NTSTATUS WINAPI probe_gui_NtCallbackReturn(void *ret_ptr,ULONG ret_len,NTSTATUS status)
{
    struct gui_thread *thread=current_gui_thread();
    struct gui_callback *frame=thread->callback;
    void *copy=NULL;
    if(!frame)return STATUS_NO_CALLBACK_ACTIVE;
    if(ret_len&&!(copy=malloc(ret_len)))return STATUS_NO_MEMORY;
    if(ret_len)memcpy(copy,ret_ptr,ret_len);
    thread->results[thread->depth-1]=copy;
    frame->ret_ptr=copy;frame->ret_len=ret_len;
    return status;
}

/* ntdll Unix helpers used by win32u. Fonts come from the Windows font
 * directory that Wine's font.c scans first; Wine's data directory is the
 * packaged stock-wine tree, which holds no fonts. File names given to
 * FreeType are the DOS paths themselves. There is no EGL or Vulkan library. */
const WCHAR *ntdll_get_build_dir(void) { return NULL; }
const WCHAR *ntdll_get_data_dir(void)
{
    static WCHAR dir[MAX_PATH+8];
    WCHAR path[MAX_PATH];
    if(!dir[0]&&probe_runtime_path(path,ARRAY_SIZE(path),L"stock-wine"))
    { wcscpy(dir,L"\\??\\");wcscat(dir,path); }
    return dir;
}
NTSTATUS ntdll_get_unix_file_name(const WCHAR *dos,char **unix_name,UINT disposition)
{
    ULONG length;
    *unix_name=NULL;
    if(!wcsncmp(dos,L"\\??\\",4))dos+=4;
    if(disposition==FILE_OPEN&&GetFileAttributesW(dos)==INVALID_FILE_ATTRIBUTES)return STATUS_OBJECT_NAME_NOT_FOUND;
    ULONG size=wcslen(dos)*sizeof(WCHAR);
    if(RtlUnicodeToUTF8N(NULL,0,&length,dos,size)||!(*unix_name=malloc(length+1)))return STATUS_NO_MEMORY;
    RtlUnicodeToUTF8N(*unix_name,length,&length,dos,size);
    (*unix_name)[length]=0;
    return STATUS_SUCCESS;
}
NTSTATUS ntdll_get_dos_file_name(const char *unix_name,WCHAR **dos,UINT disposition)
{
    ULONG length,size=strlen(unix_name);
    *dos=NULL;
    if(RtlUTF8ToUnicodeN(NULL,0,&length,unix_name,size)||!(*dos=malloc(length+5*sizeof(WCHAR))))return STATUS_NO_MEMORY;
    wcscpy(*dos,L"\\??\\");
    RtlUTF8ToUnicodeN(*dos+4,length,&length,unix_name,size);
    (*dos)[4+length/sizeof(WCHAR)]=0;
    if(disposition==FILE_OPEN&&GetFileAttributesW(*dos+4)==INVALID_FILE_ATTRIBUTES)
    { free(*dos);*dos=NULL;return STATUS_OBJECT_NAME_NOT_FOUND; }
    return STATUS_SUCCESS;
}
void ntdll_set_exception_jmp_buf(jmp_buf jmp) { (void)jmp; }
int ntdll_wcsicmp(const WCHAR *str1,const WCHAR *str2)
{
    int ret;
    for(;;){if((ret=RtlUpcaseUnicodeChar(*str1)-RtlUpcaseUnicodeChar(*str2))||!*str1)return ret;str1++;str2++;}
}
int ntdll_wcsnicmp(const WCHAR *str1,const WCHAR *str2,int n)
{
    int ret;
    for(ret=0;n>0;n--,str1++,str2++)if((ret=RtlUpcaseUnicodeChar(*str1)-RtlUpcaseUnicodeChar(*str2))||!*str1)break;
    return ret;
}
int ntdll_wcstoumbs(const WCHAR *src,DWORD srclen,char *dst,DWORD dstlen,BOOL strict)
{
    /* The Unix charset of this host is UTF-8. */
    ULONG len;
    NTSTATUS status=RtlUnicodeToUTF8N(dst,dstlen,&len,src,srclen*sizeof(WCHAR));
    if(strict&&status==STATUS_SOME_NOT_MAPPED)return -1;
    return status&&status!=STATUS_SOME_NOT_MAPPED?-1:(int)len;
}
/* The only library loaded this way is FreeType, from x86_64-native. Its
 * dependencies are found in the same directory. */
void *dlopen(const char *name,int flags)
{
    WCHAR relative[MAX_PATH],path[MAX_PATH*2];
    (void)flags;
    if(strchr(name,'/')||strchr(name,'\\')||_snwprintf(relative,ARRAY_SIZE(relative),L"stock-wine\\x86_64-native\\%hs",name)<0||
       !probe_runtime_path(path,ARRAY_SIZE(path),relative))return NULL;
    return LoadLibraryExW(path,NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
}
void *dlsym(void *handle,const char *name) { return (void *)GetProcAddress(handle,name); }
int dlclose(void *handle) { return !FreeLibrary(handle); }
char *dlerror(void) { return (char *)"library not found"; }
int asprintf(char **ret,const char *format,...)
{
    va_list args;va_start(args,format);
    int len=_vscprintf(format,args);va_end(args);
    if(len<0||!(*ret=malloc(len+1)))return -1;
    va_start(args,format);vsnprintf(*ret,len+1,format,args);va_end(args);
    return len;
}
/* File access used by win32u's FreeType font loading; see
 * gui-include/probe_unix_file.h and gui-include/sys/mman.h. */
struct probe_stat { unsigned long long st_dev,st_ino; long long st_size; unsigned int st_mode; };
static int file_identity(HANDLE file,struct probe_stat *st)
{
    BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(file,&info))return -1;
    st->st_dev=info.dwVolumeSerialNumber;
    st->st_ino=((unsigned long long)info.nFileIndexHigh<<32)|info.nFileIndexLow;
    st->st_size=((long long)info.nFileSizeHigh<<32)|info.nFileSizeLow;
    st->st_mode=info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY?0040000:0100000;
    return 0;
}
int probe_fstat(int fd,struct probe_stat *st) { return file_identity((HANDLE)_get_osfhandle(fd),st); }
int probe_stat(const char *path,struct probe_stat *st)
{
    HANDLE file=CreateFileA(path,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if(file==INVALID_HANDLE_VALUE)return -1;
    int ret=file_identity(file,st);
    CloseHandle(file);
    return ret;
}
void *mmap(void *addr,size_t length,int protection,int flags,int fd,long long offset)
{
    (void)addr;(void)protection;(void)flags;
    HANDLE mapping=CreateFileMappingW((HANDLE)_get_osfhandle(fd),NULL,PAGE_READONLY,0,0,NULL);
    void *view=mapping?MapViewOfFile(mapping,FILE_MAP_READ,(DWORD)(offset>>32),(DWORD)offset,length):NULL;
    if(mapping)CloseHandle(mapping);
    return view?view:(void *)-1;
}
int munmap(void *addr,size_t length) { (void)length;return UnmapViewOfFile(addr)?0:-1; }
NTSTATUS CDECL wine_server_handle_to_fd(HANDLE handle,unsigned int access,int *unix_fd,unsigned int *options)
{ (void)handle;(void)access;(void)options;*unix_fd=-1;return STATUS_NOT_SUPPORTED; }
void CDECL wine_server_send_fd(int fd) { (void)fd; }

/* Wine's Unix client requests for the server namespace and atoms. */
static inline void ascii_to_unicode(WCHAR *dst,const char *src,size_t len) { while(len--)*dst++=(unsigned char)*src++; }
#define NtOpenDirectoryObject probe_gui_server_NtOpenDirectoryObject
#define NtAddAtom probe_gui_NtAddAtom
#define NtDeleteAtom probe_gui_NtDeleteAtom
#define NtFindAtom probe_gui_NtFindAtom
#define NtQueryInformationAtom probe_gui_NtQueryInformationAtom
NTSTATUS WINAPI NtOpenDirectoryObject(HANDLE *,ACCESS_MASK,const OBJECT_ATTRIBUTES *);
NTSTATUS WINAPI NtAddAtom(const WCHAR *,ULONG,RTL_ATOM *);
NTSTATUS WINAPI NtDeleteAtom(RTL_ATOM);
NTSTATUS WINAPI NtFindAtom(const WCHAR *,ULONG,RTL_ATOM *);
NTSTATUS WINAPI NtQueryInformationAtom(RTL_ATOM,ATOM_INFORMATION_CLASS,void *,ULONG,ULONG *);
#include "out/wine-gui-client.inc"
#undef NtOpenDirectoryObject
#undef NtAddAtom
#undef NtDeleteAtom
#undef NtFindAtom
#undef NtQueryInformationAtom

/* Window stations live in the in-process server's session directory. */
NTSTATUS WINAPI probe_gui_NtOpenDirectoryObject(HANDLE *handle,ACCESS_MASK access,const OBJECT_ATTRIBUTES *attr)
{
    static const WCHAR prefix[]=L"\\Sessions\\";
    const UNICODE_STRING *name=attr?attr->ObjectName:NULL;
    if(name&&!attr->RootDirectory&&name->Length>=sizeof(prefix)-sizeof(WCHAR)&&
       !wcsnicmp(name->Buffer,prefix,ARRAY_SIZE(prefix)-1))
        return probe_gui_server_NtOpenDirectoryObject(handle,access,attr);
    return NtOpenDirectoryObject(handle,access,attr);
}
/* win32u maps Wine's shared session section read-only. */
NTSTATUS WINAPI probe_gui_NtOpenSection(HANDLE *handle,ACCESS_MASK access,const OBJECT_ATTRIBUTES *attr)
{
    static const WCHAR session[]=L"\\KernelObjects\\__wine_session";
    const UNICODE_STRING *name=attr?attr->ObjectName:NULL;
    if(name&&!attr->RootDirectory&&name->Length==sizeof(session)-sizeof(WCHAR)&&
       !wcsnicmp(name->Buffer,session,ARRAY_SIZE(session)-1))
    {
        if(!DuplicateHandle(GetCurrentProcess(),probe_server_session_section(),GetCurrentProcess(),handle,
                            access&SECTION_MAP_READ,FALSE,0))return STATUS_OBJECT_NAME_NOT_FOUND;
        return STATUS_SUCCESS;
    }
    return NtOpenSection(handle,access,attr);
}
/* Wine starts explorer.exe /desktop in another process, which would have a
 * separate in-process server. Its failure path creates the desktop window
 * in this server, which is the headless null-driver desktop. */
NTSTATUS WINAPI probe_gui_NtCreateUserProcess(HANDLE *process,HANDLE *thread,ACCESS_MASK process_access,
                                             ACCESS_MASK thread_access,OBJECT_ATTRIBUTES *process_attr,
                                             OBJECT_ATTRIBUTES *thread_attr,ULONG process_flags,ULONG thread_flags,
                                             RTL_USER_PROCESS_PARAMETERS *params,PS_CREATE_INFO *info,PS_ATTRIBUTE_LIST *list)
{
    (void)process;(void)thread;(void)process_access;(void)thread_access;(void)process_attr;(void)thread_attr;
    (void)process_flags;(void)thread_flags;(void)params;(void)info;(void)list;
    return STATUS_NOT_SUPPORTED;
}

/* Wine's registry for win32u is a private tree under the user's key, as a
 * Wine prefix is. Absolute \Registry names move below it; relative names
 * already resolve inside it. The host's real machine keys stay untouched. */
static UNICODE_STRING registry_root;
static void registry_name(const OBJECT_ATTRIBUTES *attr,OBJECT_ATTRIBUTES *copy,UNICODE_STRING *name,WCHAR *buffer,SIZE_T size)
{
    static const WCHAR prefix[]=L"\\Registry\\";
    *copy=*attr;
    const UNICODE_STRING *source=attr->ObjectName;
    if(attr->RootDirectory||!source||source->Length<sizeof(prefix)-sizeof(WCHAR)||
       wcsnicmp(source->Buffer,prefix,ARRAY_SIZE(prefix)-1))return;
    SIZE_T tail=source->Length-(sizeof(prefix)-2*sizeof(WCHAR));
    if(registry_root.Length+tail>size)return;
    memcpy(buffer,registry_root.Buffer,registry_root.Length);
    memcpy((char *)buffer+registry_root.Length,source->Buffer+ARRAY_SIZE(prefix)-2,tail);
    name->Buffer=buffer;name->Length=name->MaximumLength=registry_root.Length+tail;
    copy->ObjectName=name;
}
NTSTATUS WINAPI probe_gui_NtOpenKeyEx(HANDLE *key,ACCESS_MASK access,const OBJECT_ATTRIBUTES *attr,ULONG options)
{
    WCHAR buffer[1024];UNICODE_STRING name;OBJECT_ATTRIBUTES copy;
    registry_name(attr,&copy,&name,buffer,sizeof(buffer));
    return NtOpenKeyEx(key,access,&copy,options);
}
NTSTATUS WINAPI probe_gui_NtCreateKey(HANDLE *key,ACCESS_MASK access,const OBJECT_ATTRIBUTES *attr,ULONG index,
                                     const UNICODE_STRING *class,ULONG options,ULONG *disposition)
{
    WCHAR buffer[1024];UNICODE_STRING name;OBJECT_ATTRIBUTES copy;
    registry_name(attr,&copy,&name,buffer,sizeof(buffer));
    return NtCreateKey(key,access,&copy,index,class,options,disposition);
}
/* win32u links display keys with absolute REG_LINK targets, which the
 * native registry would resolve outside the private tree. */
NTSTATUS WINAPI probe_gui_NtSetValueKey(HANDLE key,const UNICODE_STRING *name,ULONG index,ULONG type,
                                       const void *data,ULONG count)
{
    if(type==REG_LINK)
    {
        WCHAR buffer[1024];UNICODE_STRING target={count,count,(WCHAR *)data},rewritten;
        OBJECT_ATTRIBUTES attr,copy;
        InitializeObjectAttributes(&attr,&target,0,NULL,NULL);
        registry_name(&attr,&copy,&rewritten,buffer,sizeof(buffer));
        if(copy.ObjectName!=&target)return NtSetValueKey(key,name,index,type,rewritten.Buffer,rewritten.Length);
    }
    return NtSetValueKey(key,name,index,type,data,count);
}
static HANDLE create_native_key(HANDLE root,const WCHAR *path)
{
    UNICODE_STRING name;OBJECT_ATTRIBUTES attr;HANDLE key=0;
    RtlInitUnicodeString(&name,path);
    InitializeObjectAttributes(&attr,&name,OBJ_CASE_INSENSITIVE,root,NULL);
    return NtCreateKey(&key,KEY_ALL_ACCESS,&attr,0,NULL,0,NULL)?0:key;
}
static void init_registry(void)
{
    UNICODE_STRING user;
    static WCHAR root[512],path[640];
    if(RtlFormatCurrentUserKeyPath(&user))ExitProcess(94);
    _snwprintf(root,ARRAY_SIZE(root)-1,L"%.*ls\\Software\\WineWoW64onWindows\\Registry",
               (int)(user.Length/sizeof(WCHAR)),user.Buffer);
    RtlInitUnicodeString(&registry_root,root);
    /* Create the private root and the two Wine roots below it. */
    HANDLE key=0;
    static const WCHAR *parts[]={L"Software",L"WineWoW64onWindows",L"Registry"};
    _snwprintf(path,ARRAY_SIZE(path)-1,L"%.*ls",(int)(user.Length/sizeof(WCHAR)),user.Buffer);
    HANDLE parent=create_native_key(NULL,path);
    for(unsigned int i=0;parent&&i<ARRAY_SIZE(parts);i++){key=create_native_key(parent,parts[i]);NtClose(parent);parent=key;}
    if(!parent)ExitProcess(94);
    HANDLE machine=create_native_key(parent,L"Machine"),users=create_native_key(parent,L"User");
    _snwprintf(path,ARRAY_SIZE(path)-1,L"%.*ls",(int)(user.Length/sizeof(WCHAR)-strlen("\\REGISTRY\\USER\\")),
               user.Buffer+strlen("\\REGISTRY\\USER\\"));
    HANDLE hkcu=users?create_native_key(users,path):0;
    if(!machine||!hkcu)ExitProcess(94);
    NtClose(machine);NtClose(users);NtClose(hkcu);NtClose(parent);
    RtlFreeUnicodeString(&user);
}

/* One-time setup before the first win32u service. As Wine's explorer does
 * for Graphics=null, select the null driver for the desktop with the null
 * display GUID that win32u uses when no explorer has published one. */
static BOOL CALLBACK gui_init(INIT_ONCE *once,void *param,void **context)
{
    (void)once;(void)param;(void)context;
    extern void probe_server_init(void);
    zero_bits=default_zero_bits;
    probe_server_init();
    init_registry();
    HKEY key=reg_create_ascii_key(NULL,"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Video\\"
                                  "{00000000-0000-0000-0000-000000000000}\\0000",0,NULL);
    if(!key)ExitProcess(94);
    set_reg_ascii_value(key,"GraphicsDriver","null");
    NtClose(key);
    return TRUE;
}
void probe_gui_init(void)
{
    static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once,gui_init,NULL,NULL);
}
