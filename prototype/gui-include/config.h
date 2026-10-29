/* Native Windows configuration for Wine's GUI Unix-side and server sources.
 * No optional Unix libraries are configured: FreeType, EGL and Vulkan are
 * absent, so Wine uses its built-in null display and font fallbacks. */
#ifndef PROBE_GUI_CONFIG_H
#define PROBE_GUI_CONFIG_H
#define PACKAGE_VERSION "11.0"
typedef unsigned int uid_t;
/* MinGW declares asprintf only for its ANSI stdio, which is not used. */
int asprintf(char **,const char *,...);
#endif
