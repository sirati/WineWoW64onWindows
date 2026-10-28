#include "test_support.h"
#include <winternl.h>
typedef LONG (WINAPI *query_fn)(HANDLE,IO_STATUS_BLOCK *,void *,ULONG,ULONG);
static const char minimal[]="<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><assemblyIdentity version=\"1.2.3.4\" name=\"Wine.Diagnostic\" type=\"win32\" processorArchitecture=\"x86\"/></assembly>";
static const char dependency[]="<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><assemblyIdentity version=\"1.2.3.4\"  name=\"testdep1\" type=\"win32\" processorArchitecture=\"x86\"/>    <file name=\"testdep.dll\" /></assembly>";
static const char main_manifest[]="<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><assemblyIdentity version=\"1.2.3.4\" name=\"Wine.Test\" type=\"win32\" /><dependency> <dependentAssembly>  <assemblyIdentity type=\"win32\" name=\"testdep1\" version=\"1.2.3.4\" processorArchitecture=\"x86\" /> </dependentAssembly></dependency></assembly>";
static unsigned failures;
static void verify_bytes(const void *memory,const char *expected,DWORD length)
{
    const BYTE *bytes=memory;
    for(DWORD i=0;i<length;i++)if(bytes[i]!=(BYTE)expected[i])ExitProcess(73);
}
static void prepare(const char *name,const char *bytes,DWORD length,BOOL inspect)
{
    HANDLE file=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    DWORD written=0,read=0;
    char buffer[1024];
    if(file==INVALID_HANDLE_VALUE||!WriteFile(file,bytes,length,&written,NULL)||written!=length||GetFileSize(file,NULL)!=length)ExitProcess(71);
    if(SetFilePointer(file,0,NULL,FILE_BEGIN)!=0||!ReadFile(file,buffer,length,&read,NULL)||read!=length)ExitProcess(72);
    verify_bytes(buffer,bytes,length);
    if(inspect)
    {
        query_fn query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationFile");
        if(!query)ExitProcess(74);
        struct {LARGE_INTEGER allocation,eof;ULONG links;BOOLEAN deleted,directory;} standard={0};
        LARGE_INTEGER eof={0};IO_STATUS_BLOCK iosb;
        iosb.Status=0xdeadbeef;iosb.Information=0xdeadbeef;
        LONG status=query(file,&iosb,&standard,sizeof(standard),5);
        MESSAGE("query.standard.status=");message_hex(status);
        MESSAGE("query.standard.iosb.status=");message_hex(iosb.Status);
        MESSAGE("query.standard.iosb.information=");message_hex(iosb.Information);
        MESSAGE("query.standard.eof=");message_hex(standard.eof.LowPart);
        if(status||standard.eof.QuadPart!=length)++failures;
        iosb.Status=0xdeadbeef;iosb.Information=0xdeadbeef;
        status=query(file,&iosb,&eof,sizeof(eof),20);
        MESSAGE("query.eof.status=");message_hex(status);
        MESSAGE("query.eof.iosb.status=");message_hex(iosb.Status);
        MESSAGE("query.eof.iosb.information=");message_hex(iosb.Information);
        MESSAGE("query.eof.value=");message_hex(eof.LowPart);
        if(!status)
        {
            if(eof.QuadPart!=length||iosb.Status||iosb.Information!=sizeof(eof))++failures;
            eof.QuadPart=0x1122334455667788ULL;
            status=query(file,&iosb,&eof,sizeof(eof)-1,20);
            MESSAGE("query.eof.short.status=");message_hex(status);
            if(status!=(LONG)0xc0000004||eof.QuadPart!=0x1122334455667788ULL)++failures;
            status=query(NULL,&iosb,&eof,sizeof(eof),20);
            MESSAGE("query.eof.invalid.status=");message_hex(status);
            if(status!=(LONG)0xc0000008||eof.QuadPart!=0x1122334455667788ULL)++failures;
        }
        HANDLE mapping=CreateFileMappingW(file,NULL,PAGE_READONLY,0,0,NULL);
        if(!mapping)ExitProcess(75);
        void *view=MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);
        if(!view)ExitProcess(76);
        verify_bytes(view,bytes,length);UnmapViewOfFile(view);CloseHandle(mapping);
        MESSAGE("manifest.readback-and-mapping=verified\r\n");
    }
    CloseHandle(file);
}
static void activate(const WCHAR *source)
{
    ACTCTXW input={.cbSize=sizeof(input),.lpSource=source};
    SetLastError(0xdeadbeef);HANDLE context=CreateActCtxW(&input);DWORD error=GetLastError();
    MESSAGE("create.valid=");message_hex(context!=INVALID_HANDLE_VALUE);
    MESSAGE("create.error=");message_hex(error);
    if(context==INVALID_HANDLE_VALUE){++failures;return;}
    ULONG_PTR cookie=0;HANDLE current=NULL;
    BOOL activated=ActivateActCtx(context,&cookie);
    MESSAGE("activate.success=");message_hex(activated);
    if(!activated||!GetCurrentActCtx(&current)||current!=context){++failures;}
    if(current)ReleaseActCtx(current);
    if(activated&&!DeactivateActCtx(0,cookie))++failures;
    ReleaseActCtx(context);
}
void WINAPI app_entry(void)
{
    prepare("diag-minimal.manifest",minimal,sizeof(minimal)-1,TRUE);
    prepare("testdep1.manifest",dependency,sizeof(dependency)-1,FALSE);
    prepare("diag-main.manifest",main_manifest,sizeof(main_manifest)-1,TRUE);
    MESSAGE("case=minimal\r\n");activate(L"diag-minimal.manifest");
    MESSAGE("case=upstream-dependency\r\n");activate(L"diag-main.manifest");
    DeleteFileA("diag-minimal.manifest");DeleteFileA("diag-main.manifest");DeleteFileA("testdep1.manifest");
    MESSAGE("diagnostic.failures=");message_hex(failures);
    if(failures)ExitProcess(78);
    MESSAGE("activation_context32: verified manifest files, mapping and simple/dependent activation contexts passed\r\n");
    ExitProcess(0);
}
