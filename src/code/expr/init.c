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
