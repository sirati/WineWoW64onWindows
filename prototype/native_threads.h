#ifndef PROBE_NATIVE_THREADS_H
#define PROBE_NATIVE_THREADS_H
struct guest_thread;
void probe_threads_init(void);
struct guest_thread *probe_thread_register(HANDLE,TEB32 *,struct guest_cpu *,void (*)(void *),void *);
void probe_thread_bind(struct guest_thread *);
void probe_thread_remove(struct guest_thread *);
void probe_thread_exception_state(void *);
void *probe_thread_take_exception_state(struct guest_thread *);
void probe_thread_ready(void);
struct guest_thread *probe_thread_acquire(HANDLE);
void probe_thread_release(struct guest_thread *);
struct guest_cpu *probe_thread_cpu(struct guest_thread *);
TEB32 *probe_thread_teb(struct guest_thread *);
void probe_thread_enter(struct guest_cpu *,struct guest_cpu **);
void probe_thread_leave(struct guest_cpu *);
void probe_thread_unregister(void);
NTSTATUS WINAPI probe_NtQueryInformationThread(HANDLE,THREADINFOCLASS,void *,ULONG,ULONG *);
NTSTATUS WINAPI probe_NtSetInformationThread(HANDLE,THREADINFOCLASS,void *,ULONG);
NTSTATUS WINAPI probe_NtGetNextThread(HANDLE,HANDLE,ACCESS_MASK,ULONG,ULONG,HANDLE *);
#endif
