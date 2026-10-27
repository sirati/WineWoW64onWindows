#include "test_support.h"
void WINAPI app_entry(void)
{
    const char *name="wine-file-semantics.txt";const char bytes[]="Wine file test";
    char readback[sizeof(bytes)];DWORD n;HANDLE h;
    DeleteFileA(name);
    h=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
    if(h==INVALID_HANDLE_VALUE) { MESSAGE("CreateFile error=");message_hex(GetLastError());ExitProcess(60); }
    if(!WriteFile(h,bytes,sizeof(bytes),&n,0)||n!=sizeof(bytes)||!CloseHandle(h)) ExitProcess(61);
    h=CreateFileA(name,GENERIC_READ,FILE_SHARE_READ,0,CREATE_NEW,0,0);
    if(h!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_FILE_EXISTS) ExitProcess(62);
    h=CreateFileA(name,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);
    if(h==INVALID_HANDLE_VALUE||!ReadFile(h,readback,sizeof(readback),&n,0)||n!=sizeof(bytes)) ExitProcess(63);
    for(unsigned i=0;i<sizeof(bytes);i++) if(readback[i]!=bytes[i]) ExitProcess(64);
    if(!CloseHandle(h)) ExitProcess(65);
    h=CreateFileA(name,GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0);
    if(h==INVALID_HANDLE_VALUE||!CloseHandle(h)) ExitProcess(66);
    h=CreateFileA(name,GENERIC_READ,0,0,OPEN_EXISTING,0,0);
    if(h==INVALID_HANDLE_VALUE||GetFileSize(h,0)!=0||!CloseHandle(h)) ExitProcess(67);
    if(!DeleteFileA(name)) ExitProcess(68);
    h=CreateFileA(name,GENERIC_READ,0,0,OPEN_EXISTING,0,0);
    if(h!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_FILE_NOT_FOUND) ExitProcess(69);
    MESSAGE("file_semantics32: create, duplicate error, read/write, truncate, delete and missing-file error passed\r\n");
    ExitProcess(0);
}
