#ifndef PROBE_NATIVE_PROCESS_H
#define PROBE_NATIVE_PROCESS_H
BOOL probe_child_bootstrap_init(void);
RTL_USER_PROCESS_PARAMETERS *probe_child_parameters(void);
NTSTATUS probe_publish_guest_process(PEB32 *,TEB32 *,I386_CONTEXT *,SECTION_IMAGE_INFORMATION *);
BOOL probe_remote_guest_process(HANDLE,ULONG_PTR *,ULONG_PTR *,DWORD *);
BOOL probe_remote_guest_thread(HANDLE,HANDLE *,ULONG_PTR *,ULONG_PTR *);
NTSTATUS WINAPI probe_NtCreateUserProcess(HANDLE *,HANDLE *,ACCESS_MASK,ACCESS_MASK,
    OBJECT_ATTRIBUTES *,OBJECT_ATTRIBUTES *,ULONG,ULONG,RTL_USER_PROCESS_PARAMETERS *,
    PS_CREATE_INFO *,PS_ATTRIBUTE_LIST *);
WCHAR *probe_build_host_command_line(WCHAR **);
void probe_init_child_context(I386_CONTEXT *,void *,void *,TEB32 *,ULONG,WORD,WORD,WORD);
#endif
