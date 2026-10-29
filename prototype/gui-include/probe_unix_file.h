/* Forced include for win32u's freetype.c. Wine caches font mappings by
 * st_dev/st_ino, which the Windows CRT does not provide; use the native
 * volume serial number and file index for that identity. */
#ifndef PROBE_UNIX_FILE_H
#define PROBE_UNIX_FILE_H
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
struct probe_stat { unsigned long long st_dev,st_ino; long long st_size; unsigned int st_mode; };
int probe_stat(const char *path,struct probe_stat *st);
int probe_fstat(int fd,struct probe_stat *st);
#define stat probe_stat
#define fstat probe_fstat
#endif
