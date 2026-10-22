#ifndef NY_RUNTIME_H
#define NY_RUNTIME_H

#include <stdint.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"

#ifdef _WIN32
#ifdef rt_argc
#undef rt_argc
#endif
#ifdef rt_argv
#undef rt_argv
#endif
#endif

#define RT_DEF(name, p, args, sig, doc) extern int64_t p();
#define RT_DEF_F64(name, p, args, sig, doc) extern int64_t p(double);
#define RT_GV(name, p, t, doc) extern t p;

#include "defs.h"

#undef RT_DEF
#undef RT_DEF_F64
#undef RT_GV

/* Backend-emitted trap entry points are implemented in the runtime but are
 * not source-callable builtins, so they are declared outside the builtin
 * signature table. */
extern void rt_bounds_trap(void);
extern void rt_rc_set_enabled(int enabled);

#pragma GCC diagnostic pop

#endif
