/* Wine's loader must recognize a renamed or hard-linked DLL by file identity,
 * while a separate byte-identical file remains a separate loaded module. */
#include "test_support.h"
static char directory[MAX_PATH],original[MAX_PATH],moved[MAX_PATH],alias[MAX_PATH],distinct[MAX_PATH];
static HMODULE first,linked,copy;
static void cleanup(void)
{
    if(copy)FreeLibrary(copy);
    if(linked)FreeLibrary(linked);
    if(first)FreeLibrary(first);
    DeleteFileA(original);DeleteFileA(moved);DeleteFileA(alias);DeleteFileA(distinct);
    RemoveDirectoryA(directory);
}
static void fail_at(DWORD stage)
{
    DWORD error=GetLastError();
    MESSAGE("file_identity32 failure stage/error: ");message_hex(stage);message_hex(error);
    cleanup();ExitProcess(stage);
}
#define REQUIRE(c,s) do { if(!(c))fail_at(s); } while(0)
static void path(char *destination,const char *name)
{ lstrcpyA(destination,directory);lstrcatA(destination,"\\");lstrcatA(destination,name); }
void WINAPI app_entry(void)
{
    char temporary[MAX_PATH];HMODULE found=NULL;
    DWORD length=GetTempPathA(sizeof(temporary),temporary);
    REQUIRE(length&&length<sizeof(temporary),71);
    REQUIRE(GetTempFileNameA(temporary,"wid",0,directory),72);
    REQUIRE(DeleteFileA(directory)&&CreateDirectoryA(directory,NULL),73);
    path(original,"original.dll");path(moved,"moved.dll");
    path(alias,"alias.dll");path(distinct,"distinct.dll");
    REQUIRE(CopyFileA("guest/dynamic_tls_library32.dll",original,TRUE),74);
    REQUIRE((first=LoadLibraryA(original)),75);
    REQUIRE(MoveFileA(original,moved),76);
    REQUIRE(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,moved,&found)&&found==first,77);
    MESSAGE("phase=renamed-module-found\r\n");
    REQUIRE(CreateHardLinkA(alias,moved,NULL),78);
    REQUIRE((linked=LoadLibraryA(alias))&&linked==first,79);
    MESSAGE("phase=hardlink-module-shared\r\n");
    REQUIRE(CopyFileA(moved,distinct,TRUE),80);
    REQUIRE((copy=LoadLibraryA(distinct))&&copy!=first,81);
    typedef DWORD (WINAPI *exchange_t)(DWORD);
    exchange_t exchange_first=(void *)GetProcAddress(first,"TlsExchange32");
    exchange_t exchange_copy=(void *)GetProcAddress(copy,"TlsExchange32");
    REQUIRE(exchange_first&&exchange_copy,82);
    REQUIRE(exchange_first(23)==0&&exchange_copy(29)==0&&exchange_first(0)==23&&exchange_copy(0)==29,83);
    MESSAGE("phase=distinct-file-independent\r\n");
    cleanup();
    MESSAGE("file_identity32: renamed and hardlink DLLs share a module, distinct copies retain separate TLS passed\r\n");
    ExitProcess(0);
}
