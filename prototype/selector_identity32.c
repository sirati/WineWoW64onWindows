#include "test_support.h"
struct worker_data { volatile DWORD teb,fs;HANDLE ready,finish; };
static volatile LONG failures;
static void descriptor(HANDLE thread,DWORD selector,DWORD expected)
{
    LDT_ENTRY entry;
    SetLastError(0xdeadbeef);
    BOOL success=GetThreadSelectorEntry(thread,selector,&entry);DWORD error=GetLastError();
    MESSAGE("selector=");message_hex(selector);
    MESSAGE("descriptor.success=");message_hex(success);
    MESSAGE("descriptor.error=");message_hex(error);
    if(!success){InterlockedIncrement(&failures);return;}
    DWORD base=entry.BaseLow|((DWORD)entry.HighWord.Bytes.BaseMid<<16)|((DWORD)entry.HighWord.Bytes.BaseHi<<24);
    MESSAGE("descriptor.base=");message_hex(base);
    MESSAGE("expected.base=");message_hex(expected);
    MESSAGE("descriptor.limit=");message_hex(entry.LimitLow|(entry.HighWord.Bits.LimitHi<<16));
    if(base!=expected)InterlockedIncrement(&failures);
}
static void local(DWORD *teb_out,DWORD *fs_out)
{
    DWORD teb;WORD fs,cs,ds;
    __asm__ volatile("movl %%fs:0x18,%0":"=r"(teb));
    __asm__ volatile("movw %%fs,%0":"=r"(fs));
    __asm__ volatile("movw %%cs,%0":"=r"(cs));
    __asm__ volatile("movw %%ds,%0":"=r"(ds));
    *teb_out=teb;*fs_out=fs;
    descriptor(GetCurrentThread(),fs,teb);
    HANDLE duplicate;
    if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))ExitProcess(71);
    descriptor(duplicate,fs,teb);CloseHandle(duplicate);
    descriptor(GetCurrentThread(),cs,0);descriptor(GetCurrentThread(),ds,0);
    LDT_ENTRY entry;
    SetLastError(0xdeadbeef);BOOL success=GetThreadSelectorEntry(GetCurrentThread(),0xdeadbeef,&entry);DWORD error=GetLastError();
    MESSAGE("invalid.success=");message_hex(success);MESSAGE("invalid.error=");message_hex(error);
    if(success)InterlockedIncrement(&failures);
}
static DWORD WINAPI worker(void *opaque)
{
    struct worker_data *data=opaque;DWORD teb,fs;
    MESSAGE("mode=worker-local\r\n");local(&teb,&fs);data->teb=teb;data->fs=fs;
    if(!SetEvent(data->ready)||WaitForSingleObject(data->finish,5000)!=WAIT_OBJECT_0)ExitProcess(72);
    return 0;
}
void WINAPI app_entry(void)
{
    DWORD teb,fs;
    MESSAGE("mode=main-local\r\n");local(&teb,&fs);
    struct worker_data data={0};data.ready=CreateEventA(NULL,FALSE,FALSE,NULL);data.finish=CreateEventA(NULL,FALSE,FALSE,NULL);
    HANDLE thread=CreateThread(NULL,0,worker,&data,0,NULL);
    if(!data.ready||!data.finish||!thread||WaitForSingleObject(data.ready,5000)!=WAIT_OBJECT_0)ExitProcess(73);
    if(SuspendThread(thread)!=0)ExitProcess(74);
    MESSAGE("mode=remote-worker\r\n");descriptor(thread,data.fs,data.teb);
    if(teb==data.teb)InterlockedIncrement(&failures);
    if(ResumeThread(thread)!=1||!SetEvent(data.finish)||WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0)ExitProcess(75);
    DWORD code;if(!GetExitCodeThread(thread,&code)||code)ExitProcess(76);
    CloseHandle(thread);CloseHandle(data.ready);CloseHandle(data.finish);
    MESSAGE("diagnostic.failures=");message_hex(failures);
    if(failures)ExitProcess(78);
    MESSAGE("selector_identity32: current, duplicated and remote FS descriptors identify guest TEBs passed\r\n");
    ExitProcess(0);
}
