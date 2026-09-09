/*
 * Type pipeline entry: initializes the typed-pipeline subsystem by
 * amalgamating hm.c, core.c, and emit.c into one compilation unit.
 */
#include "code/typing/pipeline.h"
#include "base/common.h"
#include "base/util.h"
#include "code/typing/nullnarrow.h"
#include "code/frontend/visitor.h"
#include "code/parse/ast.h"
#include "code/parse/proof.h"
#include "code/priv.h"
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NYTRIX_HAS_Z3
#include <z3.h>
#endif

#include "core.c"
#include "hm.c"
#include "emit.c"
