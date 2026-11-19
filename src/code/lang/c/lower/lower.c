/*
 * C lowering implementation fragments.
 *
 * This file is included by code/lang/c/parser/parse.c after parser state and
 * helper declarations are defined. Keeping it in the lowering layer gives
 * the C lowering fragments a single entry point while preserving their
 * existing private access to parser state and static helpers.
 */
#include "code/lang/c/lower/lower.h"

#include "code/lang/c/lower/parts/asm.h"
#include "code/lang/c/lower/parts/emit.h"
#include "code/lang/c/lower/parts/expr.h"
#include "code/lang/c/lower/parts/initializer.h"
#include "code/lang/c/lower/parts/statement.h"
