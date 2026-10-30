#ifndef PROBE_NATIVE_LOADER_H
#define PROBE_NATIVE_LOADER_H
void probe_loader_init(void);
BOOL probe_runtime_path(WCHAR *path,SIZE_T capacity,const WCHAR *relative);
BOOL probe_loader_append_paths(WCHAR *path,SIZE_T capacity);
NTSTATUS probe_builtin_unixlib(void *module,ULONGLONG *handle);
void WINAPI probe_unixlib_unmap(void *module,BOOL post,NTSTATUS status);
HMODULE probe_load_native_library(const WCHAR *path);
#endif
