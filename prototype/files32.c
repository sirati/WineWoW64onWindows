#include "test_support.h"
void WINAPI app_entry(void)
{
    HANDLE h=CreateFileA("wine-probe-test.txt",GENERIC_READ|GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    if(h==INVALID_HANDLE_VALUE) ExitProcess(52);
    CloseHandle(h);DeleteFileA("wine-probe-test.txt");MESSAGE("files32: file create/close/delete passed\r\n");ExitProcess(0);
}
