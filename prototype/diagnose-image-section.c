#include <stdio.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
int main(int argc,char **argv) {
 for(unsigned i=0;i<2;i++) {
  HANDLE file=CreateFileA(argv[1],GENERIC_READ|(i?GENERIC_EXECUTE:0),FILE_SHARE_READ|FILE_SHARE_DELETE,0,OPEN_EXISTING,0,0),section=0;
  LARGE_INTEGER size={0};
  NTSTATUS s=NtCreateSection(&section,STANDARD_RIGHTS_REQUIRED|SECTION_QUERY|SECTION_MAP_READ|SECTION_MAP_EXECUTE,NULL,&size,PAGE_EXECUTE_READ,SEC_IMAGE,file);
  printf("execute=%u handle=%p status=%08lx\n",i,file,s);if(section)NtClose(section);CloseHandle(file);
 }
 return 0;
}
