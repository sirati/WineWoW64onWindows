/* The pthread subset used by Wine's win32u, backed by Windows primitives. */
#ifndef PROBE_GUI_PTHREAD_H
#define PROBE_GUI_PTHREAD_H
typedef struct { void *lock; unsigned long owner; long depth; } pthread_mutex_t;
typedef struct { int type; } pthread_mutexattr_t;
typedef struct { void *ptr; } pthread_once_t;
typedef struct { void *lock; unsigned long writer; } pthread_rwlock_t;
#define PTHREAD_MUTEX_INITIALIZER {0,0,0}
#define PTHREAD_ONCE_INIT {0}
#define PTHREAD_RWLOCK_INITIALIZER {0,0}
#define PTHREAD_MUTEX_RECURSIVE 1
int pthread_mutex_init(pthread_mutex_t *,const pthread_mutexattr_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_settype(pthread_mutexattr_t *,int);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_once(pthread_once_t *,void (*)(void));
int pthread_rwlock_init(pthread_rwlock_t *,const void *);
int pthread_rwlock_destroy(pthread_rwlock_t *);
int pthread_rwlock_rdlock(pthread_rwlock_t *);
int pthread_rwlock_wrlock(pthread_rwlock_t *);
int pthread_rwlock_unlock(pthread_rwlock_t *);
#endif
