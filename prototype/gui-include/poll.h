/* Wine's server headers declare poll event masks. The in-process server
 * has no file descriptors, so only the declarations are required. */
#ifndef PROBE_GUI_POLL_H
#define PROBE_GUI_POLL_H
struct pollfd { int fd; short events; short revents; };
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#endif
