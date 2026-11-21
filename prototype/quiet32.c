/* No console output: success is recorded outside stdout and stderr.
 * The OpenGL variant imports its Wine builtin before the entry point runs. */
#include <windows.h>
#ifdef TEST_OPENGL_IMPORT
__declspec(dllimport) HGLRC WINAPI wglGetCurrentContext(void);
#define RESULT_FILE "opengl_import32.ok"
#define RESULT_MARKER "OPENGL_IMPORT_OK\n"
#else
#define RESULT_FILE "quiet32.ok"
#define RESULT_MARKER "QUIET_OK\n"
#endif
void WINAPI app_entry(void)
{
#ifdef TEST_OPENGL_IMPORT
    if(wglGetCurrentContext())ExitProcess(31);
#endif
    static const char marker[]=RESULT_MARKER;
    DWORD written=0;
    HANDLE file=CreateFileA(RESULT_FILE,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)ExitProcess(32);
    if(!WriteFile(file,marker,sizeof(marker)-1,&written,NULL)||written!=sizeof(marker)-1||!CloseHandle(file))ExitProcess(33);
    ExitProcess(0);
}
