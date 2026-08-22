/*
 * Expression codegen init: bootstraps the expression-lowering layer
 * by including core.c and f64.c into a single compilation unit.
 */
#include "base/common.h"
#include "base/util.h"

#include "../llvm.h"
#include "../nullnarrow.h"
#include "../priv.h"
#include "../jit.h"
#include "rt/shared.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif

#include "core.c"
#include "f64.c"
