/* In-process Wine server state for Wine's own GUI stack.
 * Wine's original window, message queue, class, hook, atom, winstation,
 * user handle, region, clipboard, object, handle, directory, event and
 * symlink units are compiled unchanged into the host. This adapter replaces
 * the Unix transport: requests run on the calling thread under one lock,
 * timeouts run on a native timer thread, in-process syncs are native events
 * and the shared session mapping is a native section. */
#include "config.h"
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "object.h"
#include "file.h"
#include "handle.h"
#include "thread.h"
#include "process.h"
#include "request.h"
#include "user.h"
#include "security.h"
#include "unicode.h"
/* The client protocol header declares wine_server_call for import from
 * ntdll; this file provides that entry point for the in-process server. */
#define wine_server_call probe_unused_wine_server_call
#include "wine/server.h"
#undef wine_server_call
unsigned int CDECL wine_server_call(void *req_ptr);

struct thread *current = NULL;
unsigned int global_error = 0;
int debug_level = 0;
struct _KUSER_SHARED_DATA *user_shared_data = NULL;

/* Type descriptors referenced by Wine's object directory initialization.
 * The objects themselves are native kernel objects on this host. */
#define PROBE_TYPE(var,text) \
    static const WCHAR var##_name[] = L##text; \
    struct type_descr var = { { var##_name, sizeof(var##_name) - sizeof(WCHAR) }, STANDARD_RIGHTS_ALL | 0xffff };
PROBE_TYPE(completion_type,"IoCompletion")
PROBE_TYPE(debug_obj_type,"DebugObject")
PROBE_TYPE(device_type,"Device")
PROBE_TYPE(file_type,"File")
PROBE_TYPE(job_type,"Job")
PROBE_TYPE(key_type,"Key")
PROBE_TYPE(mapping_type,"Section")
PROBE_TYPE(mutex_type,"Mutant")
PROBE_TYPE(process_type,"Process")
PROBE_TYPE(semaphore_type,"Semaphore")
PROBE_TYPE(thread_type,"Thread")
PROBE_TYPE(timer_type,"Timer")
PROBE_TYPE(token_type,"Token")

/* Wine Unix-transport routines reused unchanged; see extract-wine-gui.py. */
#include "out/wine-gui-server-routines.inc"

static HANDLE server_mutex,timer_event;
static DWORD thread_slot=TLS_OUT_OF_INDEXES;
static struct process *server_process;
static struct list server_threads=LIST_INIT(server_threads);
static INIT_ONCE server_once=INIT_ONCE_STATIC_INIT;

static void server_lock(void)
{
    /* A terminated issuer abandons this mutex rather than leaving every
     * other GUI thread blocked; Wine's state is then used as left. */
    WaitForSingleObject(server_mutex,INFINITE);
}
static void server_unlock(void) { ReleaseMutex(server_mutex); }

static VOID (WINAPI *pQueryInterruptTimePrecise)(ULONGLONG *);
timeout_t monotonic_counter(void)
{
    /* Interrupt time is the native tick-count clock, in 100 ns units. */
    ULONGLONG now;
    if(pQueryInterruptTimePrecise){pQueryInterruptTimePrecise(&now);return now;}
    return (timeout_t)GetTickCount64()*10000;
}
unsigned int probe_server_tick_count(void)
{
    /* The clock behind the clients' GetTickCount (patches 0004 and 0011). */
    return (unsigned int)GetTickCount64();
}
void set_current_time(void)
{
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    current_time=((timeout_t)now.dwHighDateTime<<32)|now.dwLowDateTime;
    monotonic_time=monotonic_counter();
}

/* Native in-process synchronization. Wine's ntsync backend gives each
 * waitable server object an in-process sync the client waits on directly.
 * Here that sync is a native event; client handles are native duplicates. */
struct inproc_sync { struct object obj; HANDLE event; };
static void native_sync_dump(struct object *obj,int verbose)
{ fprintf(stderr,"Native sync event=%p\n",((struct inproc_sync *)obj)->event); }
static int native_sync_signal(struct object *obj,unsigned int access,int signal)
{
    struct inproc_sync *sync=(struct inproc_sync *)obj;
    if(signal)SetEvent(sync->event);else ResetEvent(sync->event);
    return 1;
}
static void native_sync_destroy(struct object *obj) { CloseHandle(((struct inproc_sync *)obj)->event); }
static struct object_ops native_sync_ops;
int get_inproc_device_fd(void) { return 0; }
static struct inproc_sync *create_native_sync(int manual,int signaled)
{
    struct inproc_sync *sync=alloc_object(&native_sync_ops);
    if(!sync)return NULL;
    if(!(sync->event=CreateEventW(NULL,manual,signaled,NULL)))
    { sync->event=NULL;release_object(sync);set_error(STATUS_NO_MEMORY);return NULL; }
    return sync;
}
struct inproc_sync *create_inproc_internal_sync(int manual,int signaled) { return create_native_sync(manual,signaled); }
struct inproc_sync *create_inproc_event_sync(int manual,int signaled) { return create_native_sync(manual,signaled); }

/* Client handles to waitable objects are native handles to their sync. */
extern obj_handle_t wine_alloc_handle(struct process *,void *,unsigned int,unsigned int);
obj_handle_t alloc_handle(struct process *process,void *ptr,unsigned int access,unsigned int attr)
{
    struct object *obj=ptr,*sync=obj->ops->get_sync?obj->ops->get_sync(obj):NULL;
    if(sync&&sync->ops==&native_sync_ops)
    {
        HANDLE handle=NULL;
        if(!DuplicateHandle(GetCurrentProcess(),((struct inproc_sync *)sync)->event,GetCurrentProcess(),
                            &handle,SYNCHRONIZE,(attr&OBJ_INHERIT)!=0,0))
            set_error(STATUS_NO_MEMORY);
        release_object(sync);
        return (obj_handle_t)(ULONG_PTR)handle;
    }
    if(sync)release_object(sync);
    return wine_alloc_handle(process,ptr,access,attr);
}

/* No server-side waits occur: every guest wait is a native kernel wait. */
int add_queue(struct object *obj,struct wait_queue_entry *entry) { (void)obj;(void)entry;set_error(STATUS_NOT_SUPPORTED);return 0; }
void remove_queue(struct object *obj,struct wait_queue_entry *entry) { (void)obj;(void)entry; }
void wake_up(struct object *obj,int max) { (void)obj;(void)max; }
int wake_thread_queue_entry(struct wait_queue_entry *entry) { (void)entry;return 0; }
struct thread *get_wait_queue_thread(struct wait_queue_entry *entry) { (void)entry;return NULL; }
enum select_opcode get_wait_queue_select_op(struct wait_queue_entry *entry) { (void)entry;return SELECT_NONE; }
client_ptr_t get_wait_queue_key(struct wait_queue_entry *entry) { (void)entry;return 0; }

/* Objects for namespace entries implemented by native Windows. */
static void native_object_dump(struct object *obj,int verbose) { fprintf(stderr,"Native-backed namespace object\n"); }
static struct object_ops native_object_ops;
static struct object *native_object(struct object *root,const struct unicode_str *name,unsigned int attr,
                                    const struct security_descriptor *sd)
{
    if(name&&name->len)return create_named_object(root,&native_object_ops,name,attr,sd);
    return alloc_object(&native_object_ops);
}
struct object *create_named_pipe_device(struct object *root,const struct unicode_str *name,unsigned int attr,const struct security_descriptor *sd)
{ return native_object(root,name,attr,sd); }
struct object *create_mailslot_device(struct object *root,const struct unicode_str *name,unsigned int attr,const struct security_descriptor *sd)
{ return native_object(root,name,attr,sd); }
struct object *create_console_device(struct object *root,const struct unicode_str *name,unsigned int attr,const struct security_descriptor *sd)
{ return native_object(root,name,attr,sd); }
struct object *create_socket_device(struct object *root,const struct unicode_str *name,unsigned int attr,const struct security_descriptor *sd)
{ return native_object(root,name,attr,sd); }
struct object *create_unix_device(struct object *root,const struct unicode_str *name,unsigned int attr,const struct security_descriptor *sd,const char *path)
{ (void)path;return native_object(root,name,attr,sd); }
struct mapping { struct object obj; };
struct mapping *create_fd_mapping(struct object *root,const struct unicode_str *name,struct fd *fd,unsigned int attr,const struct security_descriptor *sd)
{ (void)fd;return (struct mapping *)native_object(root,name,attr,sd); }
struct object *create_user_data_mapping(struct object *root,const struct unicode_str *name,unsigned int attr,const struct security_descriptor *sd)
{ return native_object(root,name,attr,sd); }
struct list *no_kernel_obj_list(struct object *obj) { (void)obj;return NULL; }
void free_kernel_objects(struct object *obj) { (void)obj; }

/* The session mapping is a reserved native section. Wine's allocator below
 * grows its used size; the client maps the same section read-only. */
#define SESSION_MAPPING_SIZE (64*1024*1024)
static HANDLE session_section;
static char *session_view;
static size_t session_committed;
HANDLE probe_server_session_section(void) { return session_section; }
struct session_block;
static struct session_block *grow_session_mapping(mem_size_t needed);
#include "out/wine-gui-session.inc"
struct mapping *create_session_mapping(struct object *root,const struct unicode_str *name,unsigned int attr,const struct security_descriptor *sd)
{
    LARGE_INTEGER size={.QuadPart=SESSION_MAPPING_SIZE};
    SIZE_T view_size=0;
    if(NtCreateSection(&session_section,SECTION_ALL_ACCESS,NULL,&size,PAGE_READWRITE,SEC_RESERVE,NULL))return NULL;
    if(NtMapViewOfSection(session_section,GetCurrentProcess(),(void **)&session_view,0,0,NULL,&view_size,ViewUnmap,0,PAGE_READWRITE))
        return NULL;
    return (struct mapping *)native_object(root,name,attr,sd);
}
static BOOL commit_session(size_t size)
{
    if(size>SESSION_MAPPING_SIZE)return FALSE;
    if(size<=session_committed)return TRUE;
    if(!VirtualAlloc(session_view+session_committed,size-session_committed,MEM_COMMIT,PAGE_READWRITE))return FALSE;
    session_committed=size;return TRUE;
}
void set_session_mapping(struct mapping *mapping)
{
    /* Wine's initial session block: sized as in create_session_mapping. */
    size_t size=max(sizeof(*shared_session)+sizeof(object_shm_t)*512,0x10000);
    struct session_block *block;
    (void)mapping;
    size=round_size(size,0xfff);
    if(!session_view||!commit_session(size)||!(block=mem_alloc(sizeof(*block))))return;
    block->data=session_view;block->offset=0;
    block->used_size=sizeof(*shared_session);block->block_size=size;
    shared_session=(session_shm_t *)session_view;
    list_add_tail(&session.blocks,&block->entry);
}
static struct session_block *grow_session_mapping(mem_size_t needed)
{
    /* Same growth policy as Wine; the new range is committed in place. */
    size_t old_size=session_committed,new_size;
    struct session_block *block;
    new_size=max(old_size*3/2,old_size+max(needed,0x10000));
    new_size=round_size(new_size,0xfff);
    if(!commit_session(new_size)){set_error(STATUS_NO_MEMORY);return NULL;}
    if(!(block=mem_alloc(sizeof(*block))))return NULL;
    block->data=session_view+old_size;block->offset=old_size;
    block->used_size=0;block->block_size=new_size-old_size;
    list_add_tail(&session.blocks,&block->entry);
    return block;
}

/* Without tokens, objects carry only explicitly supplied descriptors. */
int check_object_access(struct token *token,struct object *obj,unsigned int *access)
{
    (void)token;
    if(*access&MAXIMUM_ALLOWED)*access=obj->ops->map_access(obj,GENERIC_ALL);
    return TRUE;
}
const struct acl *token_get_default_dacl(struct token *token) { (void)token;return NULL; }
const struct sid *token_get_owner(struct token *token) { (void)token;return NULL; }
const struct sid *token_get_primary_group(struct token *token) { (void)token;return NULL; }
struct token *thread_get_impersonation_token(struct thread *thread) { (void)thread;return NULL; }

/* The X11 driver's queue file descriptor is not used with the null driver. */
struct file *get_file_obj(struct process *process,obj_handle_t handle,unsigned int access)
{ (void)process;(void)handle;(void)access;set_error(STATUS_NOT_SUPPORTED);return NULL; }
int get_file_unix_fd(struct file *file) { (void)file;return -1; }
struct fd *create_anonymous_fd(const struct fd_ops *ops,int unix_fd,struct object *user,unsigned int options)
{ (void)ops;(void)unix_fd;(void)user;(void)options;set_error(STATUS_NOT_SUPPORTED);return NULL; }
void *get_fd_user(struct fd *fd) { (void)fd;return NULL; }
void set_fd_events(struct fd *fd,int events) { (void)fd;(void)events; }
void file_set_error(void) { set_error(STATUS_UNSUCCESSFUL); }

/* One process object represents this host. Threads are the native threads
 * which issue GUI requests; their IDs are the native Windows IDs. */
static void server_thread_dump(struct object *obj,int verbose) { fprintf(stderr,"Thread id=%04x\n",((struct thread *)obj)->id); }
static void server_thread_destroy(struct object *obj)
{
    struct thread *thread=(struct thread *)obj;
    free(thread->req_data);free(thread->reply_data);
    if(thread->process)release_object(thread->process);
}
static void server_process_dump(struct object *obj,int verbose) { fprintf(stderr,"Process id=%04x\n",((struct process *)obj)->id); }
static struct object_ops server_thread_ops,server_process_ops;

static void init_ops(struct object_ops *ops,data_size_t size,void (*dump)(struct object *,int),void (*destroy)(struct object *))
{
    /* Wine's generic defaults for an unnamed, non-waitable object. */
    memset(ops,0,sizeof(*ops));
    ops->size=size;ops->type=&no_type;ops->dump=dump;ops->add_queue=no_add_queue;
    ops->signal=no_signal;ops->get_fd=no_get_fd;ops->get_sync=default_get_sync;
    ops->map_access=default_map_access;ops->get_sd=default_get_sd;ops->set_sd=default_set_sd;
    ops->get_full_name=no_get_full_name;ops->lookup_name=no_lookup_name;ops->link_name=no_link_name;
    ops->open_file=no_open_file;ops->get_kernel_obj_list=no_kernel_obj_list;
    ops->close_handle=no_close_handle;ops->destroy=destroy;
}
static void init_casemap(void)
{
    /* load_intl_file reads Wine's l_intl.nls lowercase table. */
    extern NTSTATUS WINAPI probe_NtGetNlsSectionPtr(ULONG,ULONG,void *,void **,SIZE_T *);
    const unsigned short *data;SIZE_T size;
    if(probe_NtGetNlsSectionPtr(10,0,NULL,(void **)&data,&size)||size<4)ExitProcess(94);
    unsigned int offset=data[0];
    offset+=data[offset];offset++;
    casemap=(unsigned short *)(data+offset);
}
static BOOL CALLBACK server_init(INIT_ONCE *once,void *param,void **context)
{
    (void)once;(void)param;(void)context;
    pQueryInterruptTimePrecise=(void *)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),"QueryInterruptTimePrecise");
    if(!(server_mutex=CreateMutexW(NULL,FALSE,NULL))||!(timer_event=CreateEventW(NULL,FALSE,FALSE,NULL))||
       (thread_slot=TlsAlloc())==TLS_OUT_OF_INDEXES)ExitProcess(94);
    init_ops(&native_sync_ops,sizeof(struct inproc_sync),native_sync_dump,native_sync_destroy);
    native_sync_ops.signal=native_sync_signal;
    init_ops(&native_object_ops,sizeof(struct object),native_object_dump,no_destroy);
    native_object_ops.type=&device_type;
    native_object_ops.link_name=directory_link_name;native_object_ops.unlink_name=default_unlink_name;
    native_object_ops.get_full_name=default_get_full_name;
    init_ops(&server_thread_ops,sizeof(struct thread),server_thread_dump,server_thread_destroy);
    server_thread_ops.type=&thread_type;
    init_ops(&server_process_ops,sizeof(struct process),server_process_dump,no_destroy);
    server_process_ops.type=&process_type;
    init_casemap();
    server_lock();
    set_current_time();
    /* Wine's namespace; native objects stand in for devices and NLS. */
    init_directories((struct fd *)native_object(NULL,NULL,0,NULL));
    struct process *process=alloc_object(&server_process_ops);
    if(!process)ExitProcess(94);
    memset((char *)process+sizeof(process->obj),0,sizeof(*process)-sizeof(process->obj));
    list_init(&process->thread_list);list_init(&process->asyncs);list_init(&process->locks);
    list_init(&process->classes);list_init(&process->views);list_init(&process->kernel_object);
    list_init(&process->job_entry);list_init(&process->rawinput_entry);
    process->id=GetCurrentProcessId();process->session_id=default_session_id;
    process->machine=IMAGE_FILE_MACHINE_I386;process->start_time=current_time;
    process->priority=PROCESS_PRIOCLASS_NORMAL;process->startup_state=STARTUP_DONE;
    if(!(process->handles=alloc_handle_table(process,0)))ExitProcess(94);
    server_process=process;
    server_unlock();
    return TRUE;
}
static DWORD WINAPI timer_thread(void *arg)
{
    (void)arg;
    for(;;)
    {
        server_lock();
        set_current_time();
        int ms=get_next_timeout(NULL);
        server_unlock();
        WaitForSingleObject(timer_event,ms<0?INFINITE:(DWORD)ms);
    }
}
static void ensure_server(void)
{
    static LONG timer_started;
    InitOnceExecuteOnce(&server_once,server_init,NULL,NULL);
    if(!InterlockedCompareExchange(&timer_started,1,0))
    {
        HANDLE thread=CreateThread(NULL,64*1024,timer_thread,NULL,0,NULL);
        if(!thread)ExitProcess(94);
        CloseHandle(thread);
    }
}
/* win32u maps the session before its first request. */
void probe_server_init(void) { ensure_server(); }
static struct thread *current_server_thread(void)
{
    struct thread *thread=TlsGetValue(thread_slot);
    if(thread)return thread;
    if(!(thread=alloc_object(&server_thread_ops)))return NULL;
    memset((char *)thread+sizeof(thread->obj),0,sizeof(*thread)-sizeof(thread->obj));
    list_init(&thread->mutex_list);list_init(&thread->d3dkmt_mutexes);list_init(&thread->system_apc);
    list_init(&thread->user_apc);list_init(&thread->kernel_object);list_init(&thread->desktop_entry);
    thread->process=(struct process *)grab_object(server_process);
    thread->id=GetCurrentThreadId();thread->state=RUNNING;thread->creation_time=current_time;
    thread->priority=0;thread->base_priority=0;
    list_add_tail(&server_threads,&thread->entry);
    list_add_tail(&server_process->thread_list,&thread->proc_entry);
    server_process->running_threads++;
    /* As in Wine create_thread, new threads use the process desktop. */
    if(server_process->desktop)
    {
        struct desktop *desktop=get_desktop_obj(server_process,server_process->desktop,0);
        if(!desktop)clear_error();
        else{set_thread_default_desktop(thread,desktop,server_process->desktop);release_object(desktop);}
    }
    TlsSetValue(thread_slot,thread);
    return thread;
}

typedef void (*req_handler)(const void *req,void *reply);
#include "out/wine-gui-request-table.inc"
unsigned int CDECL wine_server_call(void *req_ptr)
{
    struct __server_request_info *req=req_ptr;
    struct thread *thread;
    union generic_reply reply;
    unsigned int error;
    char *data=NULL;
    ensure_server();
    server_lock();
    set_current_time();
    if(!(thread=current_server_thread())){server_unlock();return STATUS_NO_MEMORY;}
    thread->req=req->u.req;
    data_size_t size=req->u.req.request_header.request_size;
    /* Wine's writev payload, concatenated as read_request receives it. */
    if(size&&!(data=malloc(size))){server_unlock();return STATUS_NO_MEMORY;}
    for(unsigned int i=0,pos=0;i<req->data_count;pos+=req->data[i++].size)
        memcpy(data+pos,req->data[i].ptr,req->data[i].size);
    thread->req_data=data;
    /* call_req_handler, with the reply returned in place of send_reply. */
    enum request code=thread->req.request_header.req;
    current=thread;thread->reply_size=0;thread->reply_data=NULL;clear_error();
    memset(&reply,0,sizeof(reply));
    if(code<REQ_NB_REQUESTS&&req_handlers[code])req_handlers[code](&thread->req,&reply);
    else set_error(STATUS_NOT_IMPLEMENTED);
    reply.reply_header.error=thread->error;
    reply.reply_header.reply_size=thread->reply_size;
    req->u.reply=reply;
    if(thread->reply_size)memcpy(req->reply_data,thread->reply_data,thread->reply_size);
    error=thread->error;
    free(thread->reply_data);thread->reply_data=NULL;thread->reply_size=0;
    free(thread->req_data);thread->req_data=NULL;
    current=NULL;
    BOOL timeouts=!list_empty(&abs_timeout_list)||!list_empty(&rel_timeout_list);
    server_unlock();
    if(timeouts)SetEvent(timer_event);
    return error;
}

/* Remove the server state of a guest thread that has ended, mirroring the
 * GUI part of Wine's cleanup_thread. Runs after the native thread exits. */
void probe_server_thread_exit(DWORD tid)
{
    struct thread *thread;
    if(!server_process)return;
    server_lock();
    set_current_time();
    LIST_FOR_EACH_ENTRY(thread,&server_threads,struct thread,entry)
    {
        if(thread->id!=tid)continue;
        cleanup_clipboard_thread(thread);
        destroy_thread_windows(thread);
        free_msg_queue(thread);
        release_thread_desktop(thread,1);
        thread->desktop=0;thread->state=TERMINATED;
        list_remove(&thread->entry);list_remove(&thread->proc_entry);
        server_process->running_threads--;
        release_object(thread);
        break;
    }
    server_unlock();
}

BOOL probe_server_handle(HANDLE handle)
{
    ULONG_PTR value=(ULONG_PTR)handle;
    return value>=PROBE_SERVER_HANDLE_BASE&&value<PROBE_SERVER_HANDLE_LIMIT;
}
BOOL probe_server_close(HANDLE handle,NTSTATUS *status)
{
    if(!server_process||!probe_server_handle(handle))return FALSE;
    SERVER_START_REQ(close_handle)
    {
        req->handle=wine_server_obj_handle(handle);
        *status=wine_server_call(req);
    }
    SERVER_END_REQ;
    return TRUE;
}

struct thread *get_thread_from_id(thread_id_t id)
{
    struct thread *thread;
    LIST_FOR_EACH_ENTRY(thread,&server_threads,struct thread,entry)
        if(thread->id==id)return (struct thread *)grab_object(thread);
    set_error(STATUS_INVALID_CID);
    return NULL;
}
struct process *get_process_from_id(process_id_t id)
{
    if(server_process&&id==server_process->id)return (struct process *)grab_object(server_process);
    set_error(STATUS_INVALID_CID);
    return NULL;
}
struct process *get_process_from_handle(obj_handle_t handle,unsigned int access)
{
    (void)access;
    /* Process handles are native; only this process has server state. */
    if(handle==(obj_handle_t)~0u||GetProcessId(ULongToHandle(handle))==GetCurrentProcessId())
        return (struct process *)grab_object(server_process);
    set_error(STATUS_INVALID_HANDLE);
    return NULL;
}
void enum_processes(int (*cb)(struct process *,void *),void *user)
{ if(server_process)cb(server_process,user); }
