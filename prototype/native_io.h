#ifndef PROBE_NATIVE_IO_H
#define PROBE_NATIVE_IO_H
void probe_io_drain_thread(DWORD);
struct probe_io_args {
    void *buffer;
    ULONG size;
    LARGE_INTEGER *offset;
    ULONG *key;
    ULONG code;
    void *output;
    ULONG output_size;
    void *opaque;
    void (*complete)(void *,IO_STATUS_BLOCK *,void *);
};
typedef NTSTATUS (*probe_io_submit)(HANDLE,HANDLE,void *,IO_STATUS_BLOCK *,const struct probe_io_args *);
NTSTATUS probe_submit_io(probe_io_submit,HANDLE,HANDLE,PIO_APC_ROUTINE,void *,IO_STATUS_BLOCK *,
                         const struct probe_io_args *,BOOL);
BOOL probe_io_set_information(HANDLE,IO_STATUS_BLOCK *,void *,ULONG,FILE_INFORMATION_CLASS,NTSTATUS *);
NTSTATUS WINAPI probe_NtClose(HANDLE);
NTSTATUS WINAPI probe_NtDuplicateObject(HANDLE,HANDLE,HANDLE,HANDLE *,ACCESS_MASK,ULONG,ULONG);
NTSTATUS WINAPI probe_NtWaitForSingleObject(HANDLE,BOOLEAN,const LARGE_INTEGER *);
NTSTATUS WINAPI probe_NtWaitForMultipleObjects(ULONG,const HANDLE *,WAIT_TYPE,BOOLEAN,const LARGE_INTEGER *);
#endif
