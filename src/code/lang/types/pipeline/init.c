/*
 * Type pipeline entry: initializes the typed-pipeline subsystem by
 * amalgamating hm.c, core.c, and emit.c into one compilation unit.
 */
#include "base/common.h"
#include "base/util.h"
#include "code/lang/parse/ast.h"
#include "code/lang/parse/frontend/visitor.h"
#include "code/lang/parse/json.h"
#include "code/lang/parse/proof.h"
#include "code/lang/priv.h"
#include "code/lang/types/nullnarrow.h"
#include "code/lang/types/ownership.h"
#include "code/lang/types/pipeline.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NYTRIX_HAS_Z3
#include <z3.h>
#endif

#include "core.c"
#include "emit.c"
#include "hm.c"
