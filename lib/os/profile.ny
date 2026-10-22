;; Keywords: profile profiler performance diagnostics
;; In-process function call and timing counters.
;; References:
;; - core.os
module core.os.profile(reset, snapshot, calls, inclusive_ns, exclusive_ns, allocations, cpu_ns, exclusive_cpu_ns, sampled_rss_delta_bytes, sampled_rss_peak_bytes, runtime_stats, bytes_by_tag, request_native_oracle, request_tv_checks)
use core
use core.dict_mod

fn reset() int {
   "Clear recorded function calls and profiling counters."
   __trace_reset()
}

fn calls(str function_name) int {
   "Return the number of recorded calls for `function_name`."
   __trace_count(function_name)
}

fn inclusive_ns(str function_name) int {
   "Return elapsed nanoseconds in `function_name`, including child calls."
   __trace_inclusive_ns(function_name)
}

fn exclusive_ns(str function_name) int {
   "Return elapsed nanoseconds in `function_name`, excluding child calls."
   __trace_exclusive_ns(function_name)
}

fn cpu_ns(str function_name) int {
   "Return thread CPU nanoseconds in `function_name`, including child calls."
   __trace_cpu_ns(function_name)
}

fn exclusive_cpu_ns(str function_name) int {
   "Return thread CPU nanoseconds in `function_name`, excluding child calls."
   __trace_exclusive_cpu_ns(function_name)
}

fn sampled_rss_delta_bytes(str function_name) int {
   "Return positive process RSS growth sampled from calls of this function."
   __trace_sampled_rss_delta(function_name)
}

fn sampled_rss_peak_bytes(str function_name) int {
   "Return the largest positive process RSS growth sample for this function."
   __trace_sampled_rss_peak(function_name)
}

fn allocations(str function_name) int {
   "Return allocator calls observed in `function_name` and its children."
   __trace_allocations(function_name)
}

fn snapshot(str function_name) dict {
   "Return calls, inclusive/exclusive nanoseconds, and allocation calls."
   mut out = dict(8)
   out = dict_set(out, "calls", calls(function_name))
   out = dict_set(out, "inclusive_ns", inclusive_ns(function_name))
   out = dict_set(out, "exclusive_ns", exclusive_ns(function_name))
   out = dict_set(out, "cpu_ns", cpu_ns(function_name))
   out = dict_set(out, "exclusive_cpu_ns", exclusive_cpu_ns(function_name))
   out = dict_set(out, "sampled_rss_delta_bytes", sampled_rss_delta_bytes(function_name))
   out = dict_set(out, "sampled_rss_peak_bytes", sampled_rss_peak_bytes(function_name))
   out = dict_set(out, "allocations", allocations(function_name))
   out
}

fn bytes_by_tag(str tag_name) int {
   "Return managed GC heap payload bytes currently allocated for `tag_name`."
   __gc_tag_bytes(__runtime_tag(tag_name))
}

fn runtime_stats() dict {
   "Return allocator and GC counters. Tag byte totals are meaningful when GC is enabled."
   mut out = dict(16)
   out = dict_set(out, "allocation_calls", __runtime_alloc_count())
   out = dict_set(out, "reallocation_calls", __runtime_realloc_count())
   out = dict_set(out, "heap_bytes", __gc_stat(0))
   out = dict_set(out, "nursery_allocated_bytes", __gc_stat(1))
   out = dict_set(out, "tenured_allocated_bytes", __gc_stat(2))
   out = dict_set(out, "large_allocated_bytes", __gc_stat(3))
   out = dict_set(out, "minor_collections", __gc_stat(4))
   out = dict_set(out, "major_collections", __gc_stat(5))
   out = dict_set(out, "objects_promoted", __gc_stat(6))
   out = dict_set(out, "objects_swept", __gc_stat(7))
   out = dict_set(out, "bytes_freed", __gc_stat(8))
   out = dict_set(out, "total_pause_ns", __gc_stat(9))
   out = dict_set(out, "last_pause_ns", __gc_stat(10))
   mut by_tag = dict(8)
   by_tag = dict_set(by_tag, "list", bytes_by_tag("list"))
   by_tag = dict_set(by_tag, "tuple", bytes_by_tag("tuple"))
   by_tag = dict_set(by_tag, "dict", bytes_by_tag("dict"))
   by_tag = dict_set(by_tag, "str", bytes_by_tag("str"))
   by_tag = dict_set(by_tag, "closure", bytes_by_tag("closure"))
   by_tag = dict_set(by_tag, "bigint", bytes_by_tag("bigint"))
   by_tag = dict_set(by_tag, "bigfloat", bytes_by_tag("bigfloat"))
   out = dict_set(out, "bytes_by_tag", by_tag)
   out
}

fn request_native_oracle() int {
   "In a #main block, opt this program into the native-versus-VM result oracle."
   __request_native_oracle()
}

fn request_tv_checks(int trials=16) int {
   "In a #main block, request `trials` typed-value checks across NYIR passes."
   __request_tv_checks(trials)
}
