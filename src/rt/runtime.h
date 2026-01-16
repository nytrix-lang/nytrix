#ifndef NY_RUNTIME_H
#define NY_RUNTIME_H

#include <stdint.h>

/*
 * Runtime function declarations.
 * We use an unspecified argument list () to avoid -Wstrict-prototypes issues
 * with varying arity, although strictly speaking () means "fixed but
 * unspecified" in C11 or "unspecified" in K&R. Ideally we would have exact
 * prototypes, but for the purpose of the REPL init where we only need the
 * symbol address, this is sufficient.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#define RT_DEF(n, p, a) extern int64_t p();
#include "defs.h"
#undef RT_DEF
#pragma GCC diagnostic pop

#endif
