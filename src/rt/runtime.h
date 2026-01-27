#ifndef NY_RUNTIME_H
#define NY_RUNTIME_H

#include <stdint.h>

/*
 * Runtime function declarations.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"

#define RT_DEF(name, p, args, sig, doc) extern int64_t p();
#define RT_GV(name, p, t, doc) extern t p;

#include "defs.h"

#undef RT_DEF
#undef RT_GV

#pragma GCC diagnostic pop

#endif
