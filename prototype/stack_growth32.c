#include "test_support.h"
#include "stack_test.h"
static DWORD WINAPI worker(void *unused)
{ (void)unused;return check_stack_growth()?0:11; }
void WINAPI app_entry(void)
{
    if(!check_stack_growth())ExitProcess(10);
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);DWORD code;
    if(!thread||WaitForSingleObject(thread,3000)!=WAIT_OBJECT_0||!GetExitCodeThread(thread,&code)||code||!CloseHandle(thread))ExitProcess(12);
    MESSAGE("stack_growth32: main and worker stacks crossed guard pages, updated TEB limits and retained reserved pages successfully\r\n");ExitProcess(0);
}
