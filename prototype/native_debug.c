/* Wine's original WINEDEBUG option parser and its PEB option-table layout.
 * The native host supplies the storage normally provided by dbg_init(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "wine/debug.h"
static unsigned char default_flags;
static int nb_debug_options=-1,options_size;
static struct __wine_debug_channel *debug_options;
static const char *const debug_classes[]={"fixme","err","warn","trace"};
static const char *debug_app_name="";
#define strcasecmp _stricmp
#include "out/wine-debug-options.inc"
static void init_options(void)
{
    const char *options=getenv("WINEDEBUG");
    /* Keep the port's quiet launch default. Explicit WINEDEBUG options use
     * Wine's normal initial classes and its unchanged option parser. */
    default_flags=options?(1<<__WINE_DBCL_ERR)|(1<<__WINE_DBCL_FIXME):0;
    nb_debug_options=0;
    if(options)parse_options(options,debug_app_name);
}
#include "out/wine-debug-channel.inc"
void probe_debug_options_init(const char *app_name)
{
    debug_app_name=app_name;
    init_options();
}
void probe_debug_init(void *peb,const char *app_name)
{
    (void)app_name;
    if(nb_debug_options==-1)init_options();
    if((nb_debug_options+1)*sizeof(*debug_options)>4096)
    {fputs("Wine debug option table exceeds its PEB page\n",stderr);ExitProcess(94);}
    struct __wine_debug_channel *table=(void *)((BYTE *)peb+4096);
    if(nb_debug_options)memcpy(table,debug_options,nb_debug_options*sizeof(*table));
    table[nb_debug_options]=(struct __wine_debug_channel){default_flags};
    /* Native Wine code uses the same immutable options for this process. */
}
