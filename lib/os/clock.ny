;; Keywords: clock monotonic ticks timing
;; Clock and monotonic-time operations for elapsed-time measurement.
module std.os.clock(ticks, monotonic_ns)
fn ticks(): int { __ticks_ns() }

fn monotonic_ns(): int { ticks() }
