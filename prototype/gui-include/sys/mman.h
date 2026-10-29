/* Read-only private file mappings, as win32u maps font files for FreeType. */
#ifndef PROBE_GUI_MMAN_H
#define PROBE_GUI_MMAN_H
#include <stddef.h>
#define PROT_READ 1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr,size_t length,int protection,int flags,int fd,long long offset);
int munmap(void *addr,size_t length);
#endif
