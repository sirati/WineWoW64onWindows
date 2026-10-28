#ifndef PROBE_NATIVE_LOADER_H
#define PROBE_NATIVE_LOADER_H
void probe_loader_init(void);
BOOL probe_loader_append_paths(WCHAR *path,SIZE_T capacity);
NTSTATUS probe_builtin_unixlib(void *module,ULONGLONG *handle);
void WINAPI probe_unixlib_unmap(void *module,BOOL post,NTSTATUS status);
#endif
