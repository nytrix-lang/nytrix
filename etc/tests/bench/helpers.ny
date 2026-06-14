;; Benchmark Helpers - Shared utilities for benchmarks
use std.core
use std.os.prim (env)

fn _bench_scale_percent() {
   def raw = env("NYTRIX_BENCH_SCALE")
   if is_str(raw) && len(raw) > 0 {
      def v = atoi(raw)
      if v > 0 { return v }
   }
   100
}

fn _bench_scale(val, minv) {
   mut out = (val * _bench_scale_percent()) / 100
   if out < minv { out = minv }
   out
}
