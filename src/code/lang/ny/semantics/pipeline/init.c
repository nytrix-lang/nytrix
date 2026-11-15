/*
 * Type pipeline entry: initializes the typed-pipeline subsystem by
 * amalgamating hm.c, core.c, and emit.c into one compilation unit.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/util.h"
#include "code/lang/ny/parser/ast.h"
#include "code/lang/ny/parser/visitor.h"
#include "code/lang/ny/parser/json.h"
#include "code/lang/ny/parser/proof.h"
#include "code/lang/ny/gen/internal.h"
#include "code/lang/ny/semantics/null_narrow.h"
#include "code/lang/ny/semantics/ownership.h"
#include "code/lang/ny/semantics/pipeline.h"
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
