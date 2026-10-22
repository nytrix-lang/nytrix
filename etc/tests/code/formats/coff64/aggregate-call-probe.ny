#include "etc/tests/lang/interop/c/internal-byvalue-param-import.h"
#include "src/code/lang/c/tests/win64-ny-aggregate-host.h"

fn aggregate_argument_probe(SumPairLayout value) i64 {
   sum_pair(value)
}

fn aggregate_probe() i64 {
   sum_pair(make_sum_pair())
}

assert(aggregate_probe() == 33, "Win64 aggregate argument ABI mismatch")
assert(check_aggregate_argument() == 33,
   "Win64 aggregate parameter ABI mismatch")
