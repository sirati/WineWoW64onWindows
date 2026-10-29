/* Optional EGL/Vulkan loading in win32u. No Unix library is present. */
#ifndef PROBE_GUI_DLFCN_H
#define PROBE_GUI_DLFCN_H
#define RTLD_NOW 0
#define RTLD_GLOBAL 0
void *dlopen(const char *,int);
void *dlsym(void *,const char *);
int dlclose(void *);
char *dlerror(void);
#endif
