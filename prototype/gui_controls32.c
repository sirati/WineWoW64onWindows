/* Headless GUI controls for Wine's win32u with its null display driver.
 * Each mode prints one exact GUI_OK line and exits 0, or prints GUI_FAIL with
 * the failing step and exits with 20+step. The same EXE is the native control.
 * Modes: window, timer, cursor, dc, focus, thread. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static const char *mode;
static DWORD deadline;
static LONG creates,destroys,posted,setfocus,killfocus,sent_value,timer_messages,proc_timers;

static void __attribute__((noreturn)) fail(int step,const char *what)
{
    printf("GUI_FAIL mode=%s step=%d what=%s error=%lu\n",mode,step,what,GetLastError());
    fflush(stdout);
    ExitProcess(20+step);
}
static void __attribute__((noreturn)) pass(const char *details)
{
    printf("GUI_OK mode=%s %s\n",mode,details);
    fflush(stdout);
    ExitProcess(0);
}
static LRESULT CALLBACK window_proc(HWND hwnd,UINT msg,WPARAM wparam,LPARAM lparam)
{
    switch(msg)
    {
    case WM_CREATE:InterlockedIncrement(&creates);return 0;
    case WM_DESTROY:InterlockedIncrement(&destroys);return 0;
    case WM_SETFOCUS:InterlockedIncrement(&setfocus);return 0;
    case WM_KILLFOCUS:InterlockedIncrement(&killfocus);return 0;
    case WM_TIMER:if(wparam==1)InterlockedIncrement(&timer_messages);return 0;
    case WM_APP+1:return wparam+lparam;
    case WM_APP+2:InterlockedExchangeAdd(&posted,(LONG)(wparam+lparam));return 0;
    }
    return DefWindowProcW(hwnd,msg,wparam,lparam);
}
static void CALLBACK timer_proc(HWND hwnd,UINT msg,UINT_PTR id,DWORD time)
{ (void)hwnd;(void)id;(void)time;if(msg==WM_TIMER)InterlockedIncrement(&proc_timers); }

static HWND create_window(int step,DWORD style)
{
    static ATOM atom;
    if(!atom)
    {
        WNDCLASSW wc={0};
        wc.lpfnWndProc=window_proc;wc.hInstance=GetModuleHandleW(NULL);
        wc.lpszClassName=L"ProbeGuiControl";wc.hCursor=NULL;
        if(!(atom=RegisterClassW(&wc)))fail(step,"RegisterClassW");
    }
    HWND hwnd=CreateWindowExW(0,L"ProbeGuiControl",L"probe",style,10,20,200,100,NULL,NULL,GetModuleHandleW(NULL),NULL);
    if(!hwnd)fail(step+1,"CreateWindowExW");
    return hwnd;
}
/* Pump this thread's queue until condition holds or the deadline passes. */
static BOOL pump_until(volatile LONG *value,LONG wanted)
{
    MSG msg;
    while(*value<wanted)
    {
        DWORD now=GetTickCount();
        if((int)(deadline-now)<=0)return FALSE;
        MsgWaitForMultipleObjects(0,NULL,FALSE,deadline-now,QS_ALLINPUT);
        while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    return TRUE;
}

static void window_mode(void)
{
    char details[160];
    HWND hwnd=create_window(1,WS_OVERLAPPEDWINDOW);
    if(creates!=1)fail(3,"WM_CREATE");
    LRESULT result=SendMessageW(hwnd,WM_APP+1,7,9);
    if(result!=16)fail(4,"SendMessageW");
    if(!PostMessageW(hwnd,WM_APP+2,3,4))fail(5,"PostMessageW");
    if(!PostThreadMessageW(GetCurrentThreadId(),WM_APP+3,0,0))fail(6,"PostThreadMessageW");
    MSG msg;BOOL thread_message=FALSE;
    while(posted!=7||!thread_message)
    {
        if((int)(deadline-GetTickCount())<=0)fail(7,"posted messages");
        BOOL got=GetMessageW(&msg,NULL,0,0);
        if(got<=0)fail(8,"GetMessageW");
        if(!msg.hwnd&&msg.message==WM_APP+3)thread_message=TRUE;
        DispatchMessageW(&msg);
    }
    PostQuitMessage(37);
    if(GetMessageW(&msg,NULL,0,0)!=0||msg.wParam!=37)fail(9,"WM_QUIT");
    if(!DestroyWindow(hwnd)||destroys!=1)fail(10,"DestroyWindow");
    if(IsWindow(hwnd))fail(11,"IsWindow");
    snprintf(details,sizeof(details),"create=%ld send=%ld post=%ld thread_post=1 quit=37 destroy=%ld",
             creates,(long)result,posted,destroys);
    pass(details);
}
static void timer_mode(void)
{
    char details[160];
    HWND hwnd=create_window(1,WS_OVERLAPPEDWINDOW);
    DWORD start=GetTickCount();
    if(SetTimer(hwnd,1,50,NULL)!=1)fail(3,"SetTimer window");
    UINT_PTR proc=SetTimer(NULL,0,50,timer_proc);
    if(!proc)fail(4,"SetTimer proc");
    if(!pump_until(&timer_messages,3))fail(5,"WM_TIMER");
    if(!pump_until(&proc_timers,2))fail(6,"TIMERPROC");
    DWORD elapsed=GetTickCount()-start;
    if(!KillTimer(hwnd,1)||!KillTimer(NULL,proc))fail(7,"KillTimer");
    if(elapsed<100)fail(8,"timer interval");
    DestroyWindow(hwnd);
    snprintf(details,sizeof(details),"window_timers=%ld proc_timers=%ld min_elapsed=100",timer_messages,proc_timers);
    pass(details);
}
static void cursor_mode(void)
{
    char details[160];
    HCURSOR arrow=LoadCursorW(NULL,(LPCWSTR)IDC_ARROW);
    if(!arrow)fail(1,"LoadCursorW");
    SetCursor(arrow);
    if(GetCursor()!=arrow)fail(2,"GetCursor");
    if(!SetCursorPos(123,45))fail(3,"SetCursorPos");
    POINT pt={0};
    if(!GetCursorPos(&pt))fail(4,"GetCursorPos");
    if(pt.x!=123||pt.y!=45)
    {
        snprintf(details,sizeof(details),"position %ld,%ld",pt.x,pt.y);
        fail(5,details);
    }
    snprintf(details,sizeof(details),"set_cursor=1 position=%ld,%ld",pt.x,pt.y);
    pass(details);
}
static void dc_mode(void)
{
    char details[200];
    HDC screen=GetDC(NULL);
    if(!screen)fail(1,"GetDC screen");
    int width=GetDeviceCaps(screen,HORZRES),height=GetDeviceCaps(screen,VERTRES);
    if(width<=0||height<=0)fail(2,"GetDeviceCaps");
    if(!PatBlt(screen,0,0,4,4,BLACKNESS))fail(3,"PatBlt screen");
    if(ReleaseDC(NULL,screen)!=1)fail(4,"ReleaseDC screen");
    HWND hwnd=create_window(5,WS_OVERLAPPEDWINDOW);
    HDC window=GetDC(hwnd);
    if(!window)fail(7,"GetDC window");
    if(!PatBlt(window,0,0,8,8,WHITENESS))fail(8,"PatBlt window");
    if(ReleaseDC(hwnd,window)!=1)fail(9,"ReleaseDC window");
    HDC memory=CreateCompatibleDC(NULL);
    if(!memory)fail(10,"CreateCompatibleDC");
    BITMAPINFO info={{sizeof(BITMAPINFOHEADER),16,-16,1,32,BI_RGB}};
    DWORD *bits=NULL;
    HBITMAP dib=CreateDIBSection(memory,&info,DIB_RGB_COLORS,(void **)&bits,NULL,0);
    if(!dib||!bits)fail(11,"CreateDIBSection");
    HGDIOBJ old=SelectObject(memory,dib);
    if(!old)fail(12,"SelectObject");
    if(!PatBlt(memory,0,0,16,16,WHITENESS))fail(13,"PatBlt DIB");
    GdiFlush();
    DWORD white=bits[0]&0xffffff;
    HBRUSH brush=CreateSolidBrush(RGB(0x12,0x34,0x56));
    RECT rect={2,2,6,6};
    if(!brush||!FillRect(memory,&rect,brush))fail(14,"FillRect");
    GdiFlush();
    DWORD filled=bits[3*16+3]&0xffffff;
    COLORREF pixel=GetPixel(memory,3,3);
    if(white!=0xffffff||filled!=0x123456||pixel!=RGB(0x12,0x34,0x56))
    {
        snprintf(details,sizeof(details),"white=%06lx filled=%06lx pixel=%06lx",white,filled,(DWORD)pixel);
        fail(15,details);
    }
    SelectObject(memory,old);DeleteObject(brush);DeleteObject(dib);DeleteDC(memory);
    DestroyWindow(hwnd);
    snprintf(details,sizeof(details),"screen=%dx%d patblt=1 release=1 dib_white=%06lx fill=%06lx pixel=%06lx",
             width,height,white,filled,(DWORD)pixel);
    pass(details);
}
static void focus_mode(void)
{
    char details[160];
    HWND first=create_window(1,WS_OVERLAPPEDWINDOW|WS_VISIBLE);
    HWND second=create_window(3,WS_OVERLAPPEDWINDOW|WS_VISIBLE);
    SetActiveWindow(first);
    if(SetFocus(first),GetFocus()!=first)fail(5,"SetFocus first");
    if(SetFocus(second),GetFocus()!=second)fail(6,"SetFocus second");
    if(GetActiveWindow()!=second)fail(7,"GetActiveWindow");
    if(setfocus<2||killfocus<1)fail(8,"focus messages");
    DestroyWindow(second);DestroyWindow(first);
    snprintf(details,sizeof(details),"focus=1 active=1 setfocus=%ld killfocus=%ld",setfocus,killfocus);
    pass(details);
}
static HWND worker_window;
static HANDLE worker_ready;
static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    worker_window=create_window(1,WS_OVERLAPPEDWINDOW);
    SetEvent(worker_ready);
    MSG msg;
    while(GetMessageW(&msg,NULL,0,0)>0)DispatchMessageW(&msg);
    DestroyWindow(worker_window);
    return 0;
}
static void thread_mode(void)
{
    char details[160];
    worker_ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    DWORD tid;HANDLE thread=CreateThread(NULL,0,worker,NULL,0,&tid);
    if(!thread||!worker_ready)fail(3,"CreateThread");
    if(WaitForSingleObject(worker_ready,5000))fail(4,"worker window");
    if(GetWindowThreadProcessId(worker_window,NULL)!=tid)fail(5,"GetWindowThreadProcessId");
    DWORD_PTR result=0;
    if(!SendMessageTimeoutW(worker_window,WM_APP+1,20,22,SMTO_NORMAL,5000,&result)||result!=42)fail(6,"cross-thread SendMessage");
    if(!PostMessageW(worker_window,WM_APP+2,1,1))fail(7,"cross-thread PostMessage");
    if(!PostThreadMessageW(tid,WM_QUIT,0,0))fail(8,"PostThreadMessage WM_QUIT");
    if(WaitForSingleObject(thread,5000))fail(9,"worker exit");
    if(posted!=2)fail(10,"posted message");
    snprintf(details,sizeof(details),"send=%lu post=%ld worker_exit=1",(unsigned long)result,posted);
    pass(details);
}

int main(int argc,char **argv)
{
    mode=argc>1?argv[1]:"";
    deadline=GetTickCount()+10000;
    if(!strcmp(mode,"window"))window_mode();
    if(!strcmp(mode,"timer"))timer_mode();
    if(!strcmp(mode,"cursor"))cursor_mode();
    if(!strcmp(mode,"dc"))dc_mode();
    if(!strcmp(mode,"focus"))focus_mode();
    if(!strcmp(mode,"thread"))thread_mode();
    printf("GUI_FAIL mode=%s step=0 what=usage\n",mode);
    return 2;
}
