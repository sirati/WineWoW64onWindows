#include <winsock2.h>
__declspec(dllexport) int WINAPI SocketSend32(SOCKET socket)
{ return send(socket,"Wine",4,0); }
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,void *reserved)
{ (void)instance;(void)reason;(void)reserved;return TRUE; }
