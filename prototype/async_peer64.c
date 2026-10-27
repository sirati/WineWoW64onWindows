/* Native Windows test peer. It deliberately withholds input until asked. */
#include <stdio.h>
#include <windows.h>
static HANDLE write_release;
static DWORD WINAPI serve(void *opaque)
{
    HANDLE pipe=opaque;char command;DWORD count;
    if(!ConnectNamedPipe(pipe,NULL)&&GetLastError()!=ERROR_PIPE_CONNECTED)return 1;
    while(ReadFile(pipe,&command,1,&count,NULL)&&count==1)
    {
        if(command=='Q')break;
        if(command=='S') { SetEvent(write_release);continue; }
        if(command=='W')
        {
            char block[512];DWORD total=0;
            if(WaitForSingleObject(write_release,3000)!=WAIT_OBJECT_0)return 4;
            while(total<65536)
            {
                if(!ReadFile(pipe,block,sizeof(block),&count,NULL)||!count)return 5;
                for(DWORD i=0;i<count;i++)if((unsigned char)block[i]!=(unsigned char)(total+i))return 6;
                total+=count;
            }
            continue;
        }
        if(command!='R'&&command!='r')return 2;
        if(command=='R')Sleep(100);
        if(!WriteFile(pipe,"Wine",4,&count,NULL)||count!=4)return 3;
    }
    CloseHandle(pipe);return 0;
}
static int pipe_control_peer(void)
{
    HANDLE target=INVALID_HANDLE_VALUE;
    HANDLE control=CreateNamedPipeA("\\\\.\\pipe\\wine-fs-control-command",PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE|PIPE_WAIT,1,4096,4096,0,NULL);
    if(control==INVALID_HANDLE_VALUE)return 20;
    puts("peer ready");fflush(stdout);
    if(!ConnectNamedPipe(control,NULL)&&GetLastError()!=ERROR_PIPE_CONNECTED)return 21;
    char command,request[4];DWORD n;
    while(ReadFile(control,&command,1,&n,NULL)&&n==1)
    {
        if(command=='C')
        {
            target=CreateFileA("\\\\.\\pipe\\wine-fs-control-target",GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
            DWORD mode=PIPE_READMODE_MESSAGE;
            if(target==INVALID_HANDLE_VALUE)
            { fprintf(stderr,"pipe peer CreateFile error=%lu\n",GetLastError());return 22; }
            if(!SetNamedPipeHandleState(target,&mode,NULL,NULL))
            { fprintf(stderr,"pipe peer SetNamedPipeHandleState error=%lu\n",GetLastError());return 22; }
        }
        else if(command=='T'||command=='D')
        {
            if(!ReadFile(target,request,4,&n,NULL)||n!=4||memcmp(request,"ping",4))return 23;
            if(command=='T'&&(!WriteFile(target,"Wine",4,&n,NULL)||n!=4))return 24;
        }
        else if(command=='P')
        { if(!WriteFile(target,"peek",4,&n,NULL)||n!=4)return 25; }
        else if(command=='X')
        { if(!CloseHandle(target))return 26;target=INVALID_HANDLE_VALUE; }
        else if(command!='Q')return 27;
        if(!WriteFile(control,"A",1,&n,NULL)||n!=1)return 28;
        if(command=='Q')break;
    }
    if(target!=INVALID_HANDLE_VALUE)CloseHandle(target);
    CloseHandle(control);return 0;
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--pipe-control"))return pipe_control_peer();
    HANDLE pipes[2],threads[2];char name[80];DWORD code;
    if(!(write_release=CreateEventA(NULL,TRUE,FALSE,NULL)))return 9;
    for(unsigned i=0;i<2;i++)
    {
        sprintf(name,"\\\\.\\pipe\\wine-native-async-event-%u",i);
        pipes[i]=CreateNamedPipeA(name,PIPE_ACCESS_DUPLEX,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,1,4096,4096,0,NULL);
        if(pipes[i]==INVALID_HANDLE_VALUE)return 10;
        if(!(threads[i]=CreateThread(NULL,0,serve,pipes[i],0,NULL)))return 11;
    }
    puts("peer ready");fflush(stdout);
    for(unsigned i=0;i<2;i++)
    {
        if(WaitForSingleObject(threads[i],8000)!=WAIT_OBJECT_0||!GetExitCodeThread(threads[i],&code)||code)return 12;
        CloseHandle(threads[i]);
    }
    CloseHandle(write_release);return 0;
}
