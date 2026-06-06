;; Keywords: clock monotonic ticks timing os
;; Clock and monotonic-time operations for elapsed-time measurement.
;; References:
;; - std.os
module std.os.clock(ticks, monotonic_ns)
fn ticks() int { __ticks_ns() }

fn monotonic_ns() int { ticks() }
