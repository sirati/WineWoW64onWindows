/* Wine's original WINEDEBUG option parser and its PEB option-table layout.
 * The native host supplies the storage normally provided by dbg_init(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "wine/debug.h"
static unsigned char default_flags;
static int nb_debug_options,options_size;
static struct __wine_debug_channel *debug_options;
static const char *const debug_classes[]={"fixme","err","warn","trace"};
#define strcasecmp _stricmp
#include "out/wine-debug-options.inc"
void probe_debug_init(void *peb,const char *app_name)
{
    const char *options=getenv("WINEDEBUG");
    if(!options)return;
    default_flags=(1<<__WINE_DBCL_ERR)|(1<<__WINE_DBCL_FIXME);
    parse_options(options,app_name);
    if((nb_debug_options+1)*sizeof(*debug_options)>4096)
    {fputs("Wine debug option table exceeds its PEB page\n",stderr);ExitProcess(94);}
    struct __wine_debug_channel *table=(void *)((BYTE *)peb+4096);
    memcpy(table,debug_options,nb_debug_options*sizeof(*table));
    table[nb_debug_options]=(struct __wine_debug_channel){default_flags};
    free(debug_options);debug_options=NULL;nb_debug_options=options_size=0;
}
