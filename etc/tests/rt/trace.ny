use std.core

assert(__trace_func("rt.trace.test") == 0, "trace func records current function")
assert(__trace_loc("etc/tests/rt/trace.ny", 7, 3) == 0, "trace loc records source location")
assert(__trace_enter("rt.trace.test.enter", "etc/tests/rt/trace.ny", 8) == 0, "trace enter records call")

assert(__trace_ret_tagged("ok") == "ok", "trace tagged return preserves value")
assert(__trace_ret_i64(42) == 42, "trace i64 return preserves value")
assert(__trace_ret_u64(43) == 43, "trace u64 return preserves value")
assert(__trace_ret_bool(true), "trace bool return preserves true")
assert(__trace_ret_ptr(0) == 0, "trace ptr return preserves null")
assert(__trace_ret_f64_bits(0) == 0, "trace f64-bit return preserves raw bits")
assert(__trace_ret_void() == 0, "trace void return reports success")
assert(__trace_exit() == 0, "trace exit pops call stack")
assert(__trace_dump(1) == 0, "trace dump accepts a bounded count")

print("✓ runtime trace tests passed")
