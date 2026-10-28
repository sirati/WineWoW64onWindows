/* Public startup observations, identical under native WoW64 and Wine. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include "test_support.h"
static BOOL same(const WCHAR *a,const WCHAR *b)
{
    while(*a&&*a==*b){a++;b++;}return *a==*b;
}
static BOOL arguments_match(void)
{
    static const WCHAR *expected[]={L"arg-\x03a9-\xd83d\xde00",L"",L"two words",
                                    L"quote\"inside",L"space slash\\"};
    /* Wine reconstructs a Windows command line from argv.  Equivalent quote
     * spellings must retain the same arguments, including an empty argument,
     * embedded quotes and a backslash before the closing quote. */
    typedef int (__cdecl *getargs_t)(int *,WCHAR ***,WCHAR ***,int,int *);
    HMODULE crt=LoadLibraryW(L"msvcrt.dll");
    getargs_t getargs=crt?(getargs_t)GetProcAddress(crt,"__wgetmainargs"):NULL;
    int count=0,newmode=0;WCHAR **arguments,**environment;
    if(!getargs||getargs(&count,&arguments,&environment,0,&newmode)||count!=ARRAY_SIZE(expected)+1)
        return FALSE;
    for(unsigned i=0;i<ARRAY_SIZE(expected);i++)if(!same(arguments[i+1],expected[i]))return FALSE;
    return TRUE;
}
static void fail(DWORD code)
{
    MESSAGE("PROCESS_PARAMETERS_FAILED code=");message_hex(code);ExitProcess(code);
}
void WINAPI app_entry(void)
{
    RTL_USER_PROCESS_PARAMETERS *params=NtCurrentTeb()->Peb->ProcessParameters;
    static WCHAR image[1024],environment[128],cwd[1024];STARTUPINFOW startup={0};
    if(!params||!(params->Flags&PROCESS_PARAMS_FLAG_NORMALIZED))fail(71);
    if(!GetModuleFileNameW(NULL,image,ARRAY_SIZE(image))||
       !same(image,params->ImagePathName.Buffer))fail(72);
    if(!same(GetCommandLineW(),params->CommandLine.Buffer)||!arguments_match())fail(73);
    if(!GetEnvironmentVariableW(L"WINE_PROCESS_PARAMS_TEST",environment,ARRAY_SIZE(environment))||
       !same(environment,L"params-\x03a9-\xd83d\xde00"))fail(74);
    if(!params->Environment||!params->EnvironmentSize)fail(75);
    BOOL found=FALSE;
    for(const WCHAR *p=params->Environment;*p;)
    {
        if(same(p,L"WINE_PROCESS_PARAMS_TEST=params-\x03a9-\xd83d\xde00"))found=TRUE;
        while(*p)p++;p++;
    }
    if(!found)fail(76);
    if(!GetCurrentDirectoryW(ARRAY_SIZE(cwd),cwd))fail(77);
    unsigned length=0;while(cwd[length])length++;
    const UNICODE_STRING *dir=&params->CurrentDirectory.DosPath;
    if(dir->Length/2<length)fail(78);
    for(unsigned i=0;i<length;i++)if(cwd[i]!=dir->Buffer[i])fail(78);
    GetStartupInfoW(&startup);
    if(startup.hStdInput!=params->hStdInput||startup.hStdOutput!=params->hStdOutput||
       startup.hStdError!=params->hStdError||GetStdHandle(STD_OUTPUT_HANDLE)!=params->hStdOutput)
        fail(79);
    MESSAGE("process_parameters32: Unicode command line, environment, image, directory and standard handles passed\r\n");
    ExitProcess(0);
}
