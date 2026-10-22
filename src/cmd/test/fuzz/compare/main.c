#include "../core.h"
#include "api.h"

/*
 * Shared implementations precede consumers; keep these groups ordered.
 */
#include "util.c"

#include "audit.c"
#include "status.c"
#include "tools.c"

/*
 * The selftest runner uses the catalog and report validators from tools.c.
 */
#include "selftest.c"

/*
 * Keep dispatch after the command functions it calls.
 */
#include "init.c"
