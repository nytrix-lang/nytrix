/*
 * Expression codegen init: bootstraps the expression-lowering layer
 * by including core.c and f64.c into a single compilation unit.
 */
#include "base/common.h"
#include "base/loader.h"
#include "base/util.h"

#include "code/backend/llvm/bridge.h"
#include "code/lang/llvm/jit.h"
#include "code/lang/priv.h"
#include "code/lang/runtime/shared.h"
#include "code/lang/types/nullnarrow.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif

#include "core.c"
#include "f64.c"
