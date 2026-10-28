/* Native storage for Wine's original command-line and WoW64 parameter code.
 * Windows parses the host's Unicode command line; Wine quotes the guest
 * arguments and converts the complete native parameters to low memory. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <shellapi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "native_process.h"
#define ERR(...) fprintf(stderr,__VA_ARGS__)
#define limit_2g ((ULONG_PTR)0x80000000)
#include "out/wine-process-params.inc"

WCHAR *probe_build_host_command_line(WCHAR **arguments)
{
    return build_command_line(arguments);
}

BOOL probe_host_application_path(int argument,WCHAR *path,DWORD capacity)
{
    int count;
    WCHAR **host_argv=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!host_argv)return FALSE;
    const WCHAR *image=count>argument?host_argv[argument]:L"guest/hello32.exe";
    DWORD length=GetFullPathNameW(image,capacity,path,NULL);
    LocalFree(host_argv);
    if(length>=capacity){SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
    return length!=0;
}

RTL_USER_PROCESS_PARAMETERS32 *probe_initial_process_parameters(const WCHAR *image,
                                                               int first_argument,
                                                               const WCHAR *dllpath)
{
    RTL_USER_PROCESS_PARAMETERS *child=probe_child_parameters();
    if(child)
    {
        if(!child->DllPath.Length)RtlInitUnicodeString(&child->DllPath,dllpath);
        RTL_USER_PROCESS_PARAMETERS32 *result=build_wow64_parameters(child);
        HANDLE directory=CreateFileW(child->CurrentDirectory.DosPath.Buffer,FILE_TRAVERSE|SYNCHRONIZE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
        if(directory==INVALID_HANDLE_VALUE){VirtualFree(result,0,MEM_RELEASE);return NULL;}
        result->CurrentDirectory.Handle=HandleToUlong(directory);
        VirtualFree(child,0,MEM_RELEASE);
        return result;
    }
    int count;
    WCHAR **host_argv=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!host_argv)return NULL;
    int arguments=count>first_argument?count-first_argument:0;
    WCHAR **guest_argv=calloc(arguments+2,sizeof(*guest_argv));
    if(!guest_argv){LocalFree(host_argv);return NULL;}
    guest_argv[0]=(WCHAR *)image;
    for(int i=0;i<arguments;i++)guest_argv[i+1]=host_argv[first_argument+i];
    WCHAR *command=build_command_line(guest_argv);
    free(guest_argv);LocalFree(host_argv);
    if(!command)return NULL;
    DWORD length=GetCurrentDirectoryW(0,NULL);
    WCHAR *directory=length?malloc(length*sizeof(WCHAR)):NULL;
    WCHAR *environment=GetEnvironmentStringsW();
    if(!directory||!GetCurrentDirectoryW(length,directory)||!environment)
    {free(command);free(directory);if(environment)FreeEnvironmentStringsW(environment);return NULL;}
    UNICODE_STRING image_string,dll_string,dir_string,command_string;
    RtlInitUnicodeString(&image_string,image);RtlInitUnicodeString(&dll_string,dllpath);
    RtlInitUnicodeString(&dir_string,directory);RtlInitUnicodeString(&command_string,command);
    RTL_USER_PROCESS_PARAMETERS *params=NULL;
    NTSTATUS status=RtlCreateProcessParametersEx(&params,&image_string,&dll_string,&dir_string,
        &command_string,environment,NULL,NULL,NULL,NULL,PROCESS_PARAMS_FLAG_NORMALIZED);
    free(command);
    if(status)
    {FreeEnvironmentStringsW(environment);free(directory);SetLastError(RtlNtStatusToDosError(status));return NULL;}
    const WCHAR *end=environment;while(*end)end+=wcslen(end)+1;
    params->EnvironmentSize=(end-environment+1)*sizeof(WCHAR);
    params->hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    params->hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);
    params->hStdError=GetStdHandle(STD_ERROR_HANDLE);
    const RTL_USER_PROCESS_PARAMETERS *host=NtCurrentTeb()->Peb->ProcessParameters;
    params->ConsoleHandle=host->ConsoleHandle;
    params->ConsoleFlags=host->ConsoleFlags;
    params->dwX=host->dwX;params->dwY=host->dwY;
    params->dwXSize=host->dwXSize;params->dwYSize=host->dwYSize;
    params->dwXCountChars=host->dwXCountChars;params->dwYCountChars=host->dwYCountChars;
    params->dwFillAttribute=host->dwFillAttribute;params->dwFlags=host->dwFlags;
    params->ProcessGroupId=GetCurrentProcessId();
    params->wShowWindow=host->wShowWindow;
    RTL_USER_PROCESS_PARAMETERS32 *result=build_wow64_parameters(params);
    RtlDestroyProcessParameters(params);FreeEnvironmentStringsW(environment);
    HANDLE current=CreateFileW(directory,FILE_TRAVERSE|SYNCHRONIZE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    free(directory);
    if(current==INVALID_HANDLE_VALUE)
    {VirtualFree(result,0,MEM_RELEASE);return NULL;}
    result->CurrentDirectory.Handle=HandleToUlong(current);
    return result;
}
