;; Keywords: clock monotonic ticks timing os
;; Clock and monotonic-time operations for elapsed-time measurement.
;; References:
;; - core.os
module core.os.clock(ticks, monotonic_ns)
fn ticks() int {
   ;; Raw untagged nanoseconds; see core.os.time.ticks.
   __monotonic_ns_raw()
}

fn monotonic_ns() int { ticks() }
