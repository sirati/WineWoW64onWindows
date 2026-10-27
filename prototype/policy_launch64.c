/* Test helper: create the same native host under explicit OS mitigations.
 * It never weakens or changes the host's requested policy. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    if(argc!=3){fputs("usage: policy-launch64 MODE \"COMMAND LINE\"\n",stderr);return 2;}
    DWORD64 policy[2]={0};
    if(!strcmp(argv[1],"aslr"))policy[0]=PROCESS_CREATION_MITIGATION_POLICY_FORCE_RELOCATE_IMAGES_ALWAYS_ON_REQ_RELOCS|
        PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON|PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON;
    else if(!strcmp(argv[1],"cfg"))policy[0]=PROCESS_CREATION_MITIGATION_POLICY_CONTROL_FLOW_GUARD_ALWAYS_ON;
    else if(!strcmp(argv[1],"dynamic"))policy[0]=PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON;
    else if(!strcmp(argv[1],"cet"))policy[1]=PROCESS_CREATION_MITIGATION_POLICY2_CET_USER_SHADOW_STACKS_ALWAYS_ON;
    else if(!strcmp(argv[1],"cet-strict"))policy[1]=PROCESS_CREATION_MITIGATION_POLICY2_CET_USER_SHADOW_STACKS_STRICT_MODE|
        PROCESS_CREATION_MITIGATION_POLICY2_USER_CET_SET_CONTEXT_IP_VALIDATION_ALWAYS_ON;
    else return 2;
    SIZE_T size=0;InitializeProcThreadAttributeList(NULL,1,0,&size);
    STARTUPINFOEXA startup={.StartupInfo.cb=sizeof(startup)};PROCESS_INFORMATION process={0};
    startup.lpAttributeList=HeapAlloc(GetProcessHeap(),0,size);if(!startup.lpAttributeList)return 80;
    if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&size)||
       !UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,policy,sizeof(policy),NULL,NULL))
    {fprintf(stderr,"POLICY_ATTRIBUTE error=%lu\n",GetLastError());return 81;}
    char *command=_strdup(argv[2]);
    BOOL created=command&&CreateProcessA(NULL,command,NULL,NULL,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_SUSPENDED,NULL,NULL,&startup.StartupInfo,&process);
    DWORD error=GetLastError();DeleteProcThreadAttributeList(startup.lpAttributeList);HeapFree(GetProcessHeap(),0,startup.lpAttributeList);free(command);
    if(!created){fprintf(stderr,"POLICY_CREATE mode=%s error=%lu\n",argv[1],error);return 82;}
    DWORD cet=0,cfg=0,aslr=0,dynamic=0;
    GetProcessMitigationPolicy(process.hProcess,ProcessUserShadowStackPolicy,&cet,sizeof(cet));
    GetProcessMitigationPolicy(process.hProcess,ProcessControlFlowGuardPolicy,&cfg,sizeof(cfg));
    GetProcessMitigationPolicy(process.hProcess,ProcessASLRPolicy,&aslr,sizeof(aslr));
    GetProcessMitigationPolicy(process.hProcess,ProcessDynamicCodePolicy,&dynamic,sizeof(dynamic));
    printf("POLICY_CHILD mode=%s cet=%lx cfg=%lx aslr=%lx dynamic=%lx\n",argv[1],cet,cfg,aslr,dynamic);fflush(stdout);
    ResumeThread(process.hThread);DWORD code=98;
    if(WaitForSingleObject(process.hProcess,15000)!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,98);WaitForSingleObject(process.hProcess,1000);}
    GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
    printf("POLICY_EXIT mode=%s code=%08lx\n",argv[1],code);
    if((!strcmp(argv[1],"cet")||!strcmp(argv[1],"cet-strict"))&&!(cet&1))return 83;
    return code;
}
