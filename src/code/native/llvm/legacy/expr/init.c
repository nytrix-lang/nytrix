/*
 * Expression codegen init: bootstraps the expression-lowering layer
 * by including core.c and f64.c into a single compilation unit.
 */
#include "base/common.h"
#include "base/util.h"

#include "code/native/llvm/legacy.h"
#include "code/typing/nullnarrow.h"
#include "code/priv.h"
#include "code/native/llvm/jit.h"
#include "code/runtime/shared.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif

#include "core.c"
#include "f64.c"
