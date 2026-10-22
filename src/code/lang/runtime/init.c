/*
 * The runtime is a single translation unit: every rt_*.c below is #included
 * here. Its public surface is the RT_DEF dispatch table
 * (src/code/lang/runtime/defs.h), which declares each host function as `extern
 * int64_t p();` and exports it by address — these functions are not called by
 * name from C, so they have no call-site prototype. That is intentional, so
 * silence -Wmissing-prototypes for the whole amalgamation rather than
 * annotating hundreds of functions.
 */
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

#include "ast.c"

/*
 * Runtime AST inspection is available to compiled programs as well.
 */
#ifdef NYTRIX_RUNTIME_ONLY
#include "../parse/ast.c"
#include "../parse/lexer.c"
#include "../parse/shared.c"
#endif

#include "bigfloat.c"
#include "bigint.c"
#include "core.c"
#include "ffi.c"
#include "ffigates.c"
#include "gc.c"
#include "math.c"
#include "memory.c"
#include "os.c"
#include "proof.c"
#include "simmd.c"
#include "string.c"
