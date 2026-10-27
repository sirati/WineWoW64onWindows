/* Load MinGW's intrinsic declarations before isolating Wine's local helper.
 * Both header sets define __fastfail; renaming only Wine's definition avoids
 * turning its static inline into a duplicate external definition in Clang.
 */
#include <_mingw.h>
#define __fastfail wine_probe_fastfail
