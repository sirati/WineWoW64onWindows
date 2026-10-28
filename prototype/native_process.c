/* Native transport for Wine's process startup data.  The child is another
 * native host; Wine still maps and starts the x86 application and its DLLs. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include "wine/server_protocol.h"
#include "native_process.h"

#define wine_server_obj_handle(handle) HandleToULong(handle)
#define wine_server_ptr_handle(handle) ULongToHandle(handle)
static BOOL is_console_handle(HANDLE handle)
{ DWORD mode;return GetConsoleMode(handle,&mode); }
#include "out/wine-startup-info.inc"
#include "out/wine-child-context.inc"

#define STARTUP_MAGIC UINT64_C(0x57494e4550524331)
struct native_startup {
    ULONGLONG magic;
    ULONG size,info_size,env_size;
    NTSTATUS status;
    ULONG_PTR peb,teb,parameters,context;
    DWORD pid,tid;
    SECTION_IMAGE_INFORMATION image;
};
struct child_process {
    struct child_process *next;
    HANDLE process;
    struct native_startup startup;
};
static SRWLOCK children_lock=SRWLOCK_INIT;
static SRWLOCK creation_lock=SRWLOCK_INIT;
static struct child_process *children;
static struct native_startup *child_startup;
static HANDLE child_section,child_ready,child_continue;

static NTSTATUS last_status(void)
{
    NTSTATUS status=RtlGetLastNtStatus();
    return status<0?status:STATUS_UNSUCCESSFUL;
}
static HANDLE parse_handle(const WCHAR *text)
{
    WCHAR *end;
    ULONG value=wcstoul(text,&end,16);
    return text[0]&&!*end?(HANDLE)(ULONG_PTR)value:NULL;
}
BOOL probe_child_bootstrap_init(void)
{
    int count;
    WCHAR **arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    BOOL child=arguments&&count>1&&!wcscmp(arguments[1],L"--wine-child");
    if(!child){if(arguments)LocalFree(arguments);return TRUE;}
    if(count!=6){LocalFree(arguments);return FALSE;}
    child_section=parse_handle(arguments[3]);
    child_ready=parse_handle(arguments[4]);
    child_continue=parse_handle(arguments[5]);
    LocalFree(arguments);
    if(!child_section||!child_ready||!child_continue)return FALSE;
    child_startup=MapViewOfFile(child_section,FILE_MAP_READ|FILE_MAP_WRITE,0,0,0);
    if(!child_startup)return FALSE;
    MEMORY_BASIC_INFORMATION region;
    if(!VirtualQuery(child_startup,&region,sizeof(region))||
       region.RegionSize<sizeof(*child_startup)||child_startup->magic!=STARTUP_MAGIC||
       child_startup->size>region.RegionSize||
       child_startup->size<sizeof(*child_startup)||
       child_startup->info_size<sizeof(struct startup_info_data)||
       child_startup->info_size>child_startup->size-sizeof(*child_startup)||
       child_startup->env_size!=child_startup->size-sizeof(*child_startup)-child_startup->info_size)
        return FALSE;
    /* Bootstrap handles are private transport, never inherited by grandchildren. */
    SetHandleInformation(child_section,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(child_ready,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(child_continue,HANDLE_FLAG_INHERIT,0);
    return TRUE;
}
RTL_USER_PROCESS_PARAMETERS *probe_child_parameters(void)
{
    if(!child_startup)return NULL;
    const struct startup_info_data *info=(void *)(child_startup+1);
    const void *environment=(const BYTE *)info+child_startup->info_size;
    return rebuild_startup_info(info,child_startup->info_size,environment,child_startup->env_size);
}
NTSTATUS probe_publish_guest_process(PEB32 *peb,TEB32 *teb,I386_CONTEXT *context,
                                    SECTION_IMAGE_INFORMATION *image)
{
    if(!child_startup)return STATUS_SUCCESS;
    child_startup->peb=(ULONG_PTR)peb;
    child_startup->teb=(ULONG_PTR)teb;
    child_startup->parameters=peb->ProcessParameters;
    child_startup->context=(ULONG_PTR)context;
    child_startup->pid=GetCurrentProcessId();child_startup->tid=GetCurrentThreadId();
    if(image)child_startup->image=*image;
    child_startup->image.TransferAddress=ULongToPtr(context->Eax);
    child_startup->status=STATUS_SUCCESS;
    if(!SetEvent(child_ready))return last_status();
    NTSTATUS status=NtWaitForSingleObject(child_continue,FALSE,NULL);
    UnmapViewOfFile(child_startup);child_startup=NULL;
    CloseHandle(child_section);CloseHandle(child_ready);CloseHandle(child_continue);
    child_section=child_ready=child_continue=NULL;
    return status;
}
static void remember_child(struct child_process *record,HANDLE process,const struct native_startup *startup)
{
    record->process=process;record->startup=*startup;
    AcquireSRWLockExclusive(&children_lock);
    /* Reap completed children on later launches.  Live records retain a native
     * process handle so PID reuse cannot select a different application. */
    struct child_process **previous=&children;
    while(*previous)
    {
        struct child_process *old=*previous;
        if(WaitForSingleObject(old->process,0)==WAIT_OBJECT_0)
        {*previous=old->next;CloseHandle(old->process);free(old);}
        else previous=&old->next;
    }
    record->next=children;children=record;
    ReleaseSRWLockExclusive(&children_lock);
}
BOOL probe_remote_guest_process(HANDLE process,ULONG_PTR *peb,ULONG_PTR *parameters,DWORD *tid)
{
    DWORD pid=GetProcessId(process);BOOL found=FALSE;
    AcquireSRWLockShared(&children_lock);
    for(struct child_process *record=children;record;record=record->next)
        if(pid==record->startup.pid)
        {
            if(peb)*peb=record->startup.peb;
            if(parameters)*parameters=record->startup.parameters;
            if(tid)*tid=record->startup.tid;
            found=TRUE;break;
        }
    ReleaseSRWLockShared(&children_lock);return found;
}
BOOL probe_remote_guest_thread(HANDLE thread,HANDLE *process,ULONG_PTR *teb,ULONG_PTR *context)
{
    DWORD tid=GetThreadId(thread),pid=GetProcessIdOfThread(thread);BOOL found=FALSE;
    AcquireSRWLockShared(&children_lock);
    for(struct child_process *record=children;record;record=record->next)
        if(pid==record->startup.pid&&tid==record->startup.tid)
        {
            if(teb)*teb=record->startup.teb;
            if(context)*context=record->startup.context;
            if(process&&!DuplicateHandle(GetCurrentProcess(),record->process,GetCurrentProcess(),process,
                                         0,FALSE,DUPLICATE_SAME_ACCESS))break;
            found=TRUE;break;
        }
    ReleaseSRWLockShared(&children_lock);return found;
}
static NTSTATUS restrict_handle(HANDLE source,ACCESS_MASK access,OBJECT_ATTRIBUTES *attributes,HANDLE *result)
{
    return NtDuplicateObject(GetCurrentProcess(),source,GetCurrentProcess(),result,access,
        attributes?attributes->Attributes&OBJ_INHERIT:0,0);
}
NTSTATUS WINAPI probe_NtCreateUserProcess(HANDLE *process_result,HANDLE *thread_result,
    ACCESS_MASK process_access,ACCESS_MASK thread_access,OBJECT_ATTRIBUTES *process_attributes,
    OBJECT_ATTRIBUTES *thread_attributes,ULONG process_flags,ULONG thread_flags,
    RTL_USER_PROCESS_PARAMETERS *params,PS_CREATE_INFO *create_info,PS_ATTRIBUTE_LIST *attributes)
{
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    HANDLE file=NULL,image_section=NULL,section=NULL,ready=NULL,continuation=NULL,record_handle=NULL;
    struct native_startup *startup=NULL;
    void *serialized=NULL;DWORD serialized_size=0;
    WCHAR *image_path=NULL,*command=NULL;
    HANDLE *inherit_handles=NULL;
    HANDLE diagnostic_handles[3]={0};
    struct child_process *record=NULL;
    PROCESS_INFORMATION process={0};STARTUPINFOEXW si={0};
    SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE},process_security={sizeof(security)},thread_security={sizeof(security)};
    UNICODE_STRING image_name={0};
    const PS_ATTRIBUTE *handle_list=NULL,*jobs=NULL;
    HANDLE token=NULL;
    BOOL creation_locked=FALSE,attributes_initialized=FALSE;
    SIZE_T count=attributes?(attributes->TotalLength-sizeof(attributes->TotalLength))/sizeof(PS_ATTRIBUTE):0;
    SECTION_IMAGE_INFORMATION image_info;
    *process_result=*thread_result=NULL;
    /* Wine's original WoW64 thunk supplies an output-only native create_info. */
    memset(create_info,0,sizeof(*create_info));create_info->Size=sizeof(*create_info);
    if(!params||(thread_flags&~THREAD_CREATE_FLAGS_CREATE_SUSPENDED))goto done;
    if(process_flags&~(PROCESS_CREATE_FLAGS_INHERIT_HANDLES|PROCESS_CREATE_FLAGS_SUSPENDED|
                        PROCESS_CREATE_FLAGS_BREAKAWAY|PROCESS_CREATE_FLAGS_NO_DEBUG_INHERIT))
    {status=STATUS_NOT_SUPPORTED;goto done;}
    for(SIZE_T i=0;i<count;i++)
    {
        const PS_ATTRIBUTE *attribute=&attributes->Attributes[i];
        switch(attribute->Attribute)
        {
        case PS_ATTRIBUTE_IMAGE_NAME:
            image_name.Buffer=attribute->ValuePtr;image_name.Length=attribute->Size;break;
        case PS_ATTRIBUTE_HANDLE_LIST:handle_list=attribute;break;
        case PS_ATTRIBUTE_JOB_LIST:jobs=attribute;break;
        case PS_ATTRIBUTE_TOKEN:token=attribute->ValuePtr;break;
        case PS_ATTRIBUTE_MACHINE_TYPE:
            if(attribute->Value&&attribute->Value!=IMAGE_FILE_MACHINE_I386)
            {status=STATUS_INVALID_IMAGE_FORMAT;goto done;}break;
        case PS_ATTRIBUTE_CLIENT_ID:case PS_ATTRIBUTE_IMAGE_INFO:case PS_ATTRIBUTE_TEB_ADDRESS:break;
        default:
            if(attribute->Attribute&PS_ATTRIBUTE_INPUT){status=STATUS_NOT_SUPPORTED;goto done;}
            break;
        }
    }
    if(!image_name.Buffer||!image_name.Length)goto done;
    image_name.MaximumLength=image_name.Length;
    OBJECT_ATTRIBUTES file_attributes;
    IO_STATUS_BLOCK io;
    InitializeObjectAttributes(&file_attributes,&image_name,OBJ_CASE_INSENSITIVE,NULL,NULL);
    status=NtOpenFile(&file,FILE_EXECUTE|FILE_READ_DATA|SYNCHRONIZE,&file_attributes,&io,
                      FILE_SHARE_READ|FILE_SHARE_DELETE,FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE);
    if(status)goto done;
    status=NtCreateSection(&image_section,SECTION_QUERY,NULL,NULL,PAGE_READONLY,SEC_IMAGE,file);
    if(status){create_info->State=PsCreateFailOnSectionCreate;goto done;}
    status=NtQuerySection(image_section,SectionImageInformation,&image_info,sizeof(image_info),NULL);
    if(status)goto done;
    if(image_info.Machine!=IMAGE_FILE_MACHINE_I386){status=STATUS_INVALID_IMAGE_FORMAT;goto done;}
    struct pe_image_info pe_info={.subsystem=image_info.SubSystemType};
    serialized=create_startup_info(&image_name,process_flags,params,&pe_info,&serialized_size);
    if(!serialized){status=STATUS_NO_MEMORY;goto done;}
    const WCHAR *environment=params->Environment,*environment_end=environment;
    if(environment)while(*environment_end)environment_end+=wcslen(environment_end)+1;
    SIZE_T environment_size=environment?(environment_end-environment+1)*sizeof(WCHAR):2*sizeof(WCHAR);
    SIZE_T total_size=sizeof(*startup)+serialized_size+environment_size;
    if(total_size>MAXDWORD){status=STATUS_INVALID_PARAMETER;goto done;}
    /* Do not let concurrent launches inherit another launch's transport. */
    AcquireSRWLockExclusive(&creation_lock);creation_locked=TRUE;
    section=CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,total_size,NULL);
    ready=CreateEventW(&security,TRUE,FALSE,NULL);continuation=CreateEventW(&security,TRUE,FALSE,NULL);
    if(!section||!ready||!continuation){status=last_status();goto done;}
    startup=MapViewOfFile(section,FILE_MAP_READ|FILE_MAP_WRITE,0,0,total_size);
    if(!startup){status=last_status();goto done;}
    startup->magic=STARTUP_MAGIC;startup->size=total_size;
    startup->info_size=serialized_size;startup->env_size=environment_size;
    startup->status=STATUS_PENDING;startup->image=image_info;
    memcpy(startup+1,serialized,serialized_size);
    if(environment)memcpy((BYTE *)(startup+1)+serialized_size,environment,environment_size);
    image_path=calloc(image_name.Length/sizeof(WCHAR)+1,sizeof(WCHAR));
    record=calloc(1,sizeof(*record));
    if(!image_path||!record){status=STATUS_NO_MEMORY;goto done;}
    memcpy(image_path,image_name.Buffer,image_name.Length);
    WCHAR *dos_image=image_path;
    if(!wcsncmp(dos_image,L"\\??\\",4))
    {if(dos_image[5]==L':')dos_image+=4;else dos_image[1]=L'\\';}
    WCHAR host_path[32768],section_arg[32],ready_arg[32],continue_arg[32];
    if(!GetModuleFileNameW(NULL,host_path,ARRAY_SIZE(host_path))){status=last_status();goto done;}
    swprintf(section_arg,ARRAY_SIZE(section_arg),L"%llx",(ULONGLONG)(ULONG_PTR)section);
    swprintf(ready_arg,ARRAY_SIZE(ready_arg),L"%llx",(ULONGLONG)(ULONG_PTR)ready);
    swprintf(continue_arg,ARRAY_SIZE(continue_arg),L"%llx",(ULONGLONG)(ULONG_PTR)continuation);
    WCHAR *host_arguments[]={host_path,L"--wine-child",dos_image,section_arg,ready_arg,continue_arg,NULL};
    command=probe_build_host_command_line(host_arguments);
    if(!command){status=STATUS_NO_MEMORY;goto done;}
    si.StartupInfo.cb=sizeof(si);
    si.StartupInfo.lpDesktop=params->Desktop.Buffer;
    si.StartupInfo.lpTitle=params->WindowTitle.Buffer;
    /* Guest standard handles stay in Wine's startup data.  Native bootstrap
     * diagnostics use the parent's host streams, outside redirected guest IO. */
    const DWORD standard_ids[]={STD_INPUT_HANDLE,STD_OUTPUT_HANDLE,STD_ERROR_HANDLE};
    for(UINT i=0;i<ARRAY_SIZE(standard_ids);i++)
    {
        HANDLE source=GetStdHandle(standard_ids[i]);
        if(source&&source!=INVALID_HANDLE_VALUE&&
           !DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&diagnostic_handles[i],
                            0,TRUE,DUPLICATE_SAME_ACCESS))
        {status=last_status();goto done;}
    }
    si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput=diagnostic_handles[0];
    si.StartupInfo.hStdOutput=diagnostic_handles[1];
    si.StartupInfo.hStdError=diagnostic_handles[2];
    if(process_attributes)process_security.lpSecurityDescriptor=process_attributes->SecurityDescriptor;
    if(thread_attributes)thread_security.lpSecurityDescriptor=thread_attributes->SecurityDescriptor;
    DWORD flags=CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT;
    if(process_flags&PROCESS_CREATE_FLAGS_BREAKAWAY)flags|=CREATE_BREAKAWAY_FROM_JOB;
    /* A handle list limits guest inheritance while allowing the private
     * bootstrap section/events even when guest bInheritHandles is FALSE. */
    BOOL limited=!(process_flags&PROCESS_CREATE_FLAGS_INHERIT_HANDLES)||handle_list;
    SIZE_T attribute_bytes=0,attribute_count=(limited?1:0)+(jobs?1:0);
    if(attribute_count)
    {
        InitializeProcThreadAttributeList(NULL,attribute_count,0,&attribute_bytes);
        si.lpAttributeList=malloc(attribute_bytes);
        if(!si.lpAttributeList){status=STATUS_NO_MEMORY;goto done;}
        if(!InitializeProcThreadAttributeList(si.lpAttributeList,attribute_count,0,&attribute_bytes))
        {status=last_status();goto done;}
        attributes_initialized=TRUE;
    }
    if(limited)
    {
        SIZE_T guest_count=handle_list&&(process_flags&PROCESS_CREATE_FLAGS_INHERIT_HANDLES)?
                          handle_list->Size/sizeof(HANDLE):0;
        inherit_handles=calloc(guest_count+6,sizeof(HANDLE));
        if(!inherit_handles){status=STATUS_NO_MEMORY;goto done;}
        if(guest_count)memcpy(inherit_handles,handle_list->ValuePtr,guest_count*sizeof(HANDLE));
        inherit_handles[guest_count]=section;inherit_handles[guest_count+1]=ready;inherit_handles[guest_count+2]=continuation;
        SIZE_T inherited_count=guest_count+3;
        for(UINT i=0;i<ARRAY_SIZE(diagnostic_handles);i++)
            if(diagnostic_handles[i])inherit_handles[inherited_count++]=diagnostic_handles[i];
        if(!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
             inherit_handles,inherited_count*sizeof(HANDLE),NULL,NULL))
        {status=last_status();goto done;}
    }
    if(jobs&&!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                      jobs->ValuePtr,jobs->Size,NULL,NULL))
    {status=last_status();goto done;}
    BOOL created=token?CreateProcessAsUserW(token,host_path,command,&process_security,&thread_security,TRUE,
        flags,params->Environment,params->CurrentDirectory.DosPath.Buffer,&si.StartupInfo,&process):
        CreateProcessW(host_path,command,&process_security,&thread_security,TRUE,flags,params->Environment,
            params->CurrentDirectory.DosPath.Buffer,&si.StartupInfo,&process);
    if(!created){status=last_status();goto done;}
    SetHandleInformation(section,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(ready,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(continuation,HANDLE_FLAG_INHERIT,0);
    for(UINT i=0;i<ARRAY_SIZE(diagnostic_handles);i++)
        if(diagnostic_handles[i])SetHandleInformation(diagnostic_handles[i],HANDLE_FLAG_INHERIT,0);
    ReleaseSRWLockExclusive(&creation_lock);creation_locked=FALSE;
    if(ResumeThread(process.hThread)==(DWORD)-1){status=last_status();goto done;}
    HANDLE wait_handles[]={ready,process.hProcess};
    DWORD wait=WaitForMultipleObjects(ARRAY_SIZE(wait_handles),wait_handles,FALSE,60000);
    if(wait!=WAIT_OBJECT_0)
    {
        DWORD code=STATUS_UNSUCCESSFUL;
        if(wait==WAIT_OBJECT_0+1)GetExitCodeProcess(process.hProcess,&code);
        status=wait==WAIT_TIMEOUT?STATUS_TIMEOUT:code;
        if(status>=0)status=STATUS_UNSUCCESSFUL;
        goto done;
    }
    status=startup->status;
    if(status)goto done;
    if(thread_flags&THREAD_CREATE_FLAGS_CREATE_SUSPENDED)
    {status=NtSuspendThread(process.hThread,NULL);if(status)goto done;}
    if(!DuplicateHandle(GetCurrentProcess(),process.hProcess,GetCurrentProcess(),&record_handle,
                        0,FALSE,DUPLICATE_SAME_ACCESS)){status=last_status();goto done;}
    status=restrict_handle(process.hProcess,process_access,process_attributes,process_result);
    if(status)goto done;
    status=restrict_handle(process.hThread,thread_access,thread_attributes,thread_result);
    if(status)goto done;
    create_info->State=PsCreateSuccess;
    create_info->SuccessState.PebAddressWow64=startup->peb;
    create_info->SuccessState.UserProcessParametersWow64=startup->parameters;
    create_info->SuccessState.CurrentParameterFlags=PROCESS_PARAMS_FLAG_NORMALIZED;
    CLIENT_ID id={ULongToHandle(process.dwProcessId),ULongToHandle(process.dwThreadId)};
    for(SIZE_T i=0;i<count;i++)
    {
        PS_ATTRIBUTE *attribute=&attributes->Attributes[i];
        const void *value=NULL;SIZE_T size=0;
        ULONG_PTR teb=startup->teb-0x2000;
        if(attribute->Attribute==PS_ATTRIBUTE_CLIENT_ID){value=&id;size=sizeof(id);}
        else if(attribute->Attribute==PS_ATTRIBUTE_IMAGE_INFO){value=&startup->image;size=sizeof(startup->image);}
        else if(attribute->Attribute==PS_ATTRIBUTE_TEB_ADDRESS){value=&teb;size=sizeof(teb);}
        if(value)
        {size=min(size,attribute->Size);memcpy(attribute->ValuePtr,value,size);if(attribute->ReturnLength)*attribute->ReturnLength=size;}
    }
    if(!SetEvent(continuation)){status=last_status();goto done;}
    remember_child(record,record_handle,startup);record=NULL;record_handle=NULL;
    status=STATUS_SUCCESS;
done:
    if(status)
    {
        if(process.hProcess)TerminateProcess(process.hProcess,status);
        if(*process_result){CloseHandle(*process_result);*process_result=NULL;}
        if(*thread_result){CloseHandle(*thread_result);*thread_result=NULL;}
    }
    if(record_handle)CloseHandle(record_handle);
    if(process.hProcess)CloseHandle(process.hProcess);
    if(process.hThread)CloseHandle(process.hThread);
    if(startup)UnmapViewOfFile(startup);
    if(section)CloseHandle(section);if(ready)CloseHandle(ready);if(continuation)CloseHandle(continuation);
    if(image_section)NtClose(image_section);if(file)NtClose(file);
    for(UINT i=0;i<ARRAY_SIZE(diagnostic_handles);i++)if(diagnostic_handles[i])CloseHandle(diagnostic_handles[i]);
    if(attributes_initialized)DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    if(creation_locked)ReleaseSRWLockExclusive(&creation_lock);
    free(inherit_handles);free(serialized);free(image_path);free(command);free(record);
    return status;
}
NTSTATUS (WINAPI *__imp_probe_NtCreateUserProcess)(HANDLE *,HANDLE *,ACCESS_MASK,ACCESS_MASK,
    OBJECT_ATTRIBUTES *,OBJECT_ATTRIBUTES *,ULONG,ULONG,RTL_USER_PROCESS_PARAMETERS *,PS_CREATE_INFO *,PS_ATTRIBUTE_LIST *)=probe_NtCreateUserProcess;
