#include "code/typepipeline.h"
#include "base/common.h"
#include "base/util.h"
#include "code/nullnarrow.h"
#include "parse/ast.h"
#include "../priv.h"
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
