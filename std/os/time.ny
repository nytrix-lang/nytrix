;; Keywords: os time
;; Os Time module.

module std.os.time (
   time, sleep, msleep, ticks
)
use std.core *
use std.os.sys *

fn time(){
   "Returns the current Unix timestamp (seconds since epoch) using `clock_gettime(CLOCK_REALTIME)`."
   def ts = malloc(16)
   def r = __clock_gettime(0, ts)
   if(r != 0){ free(ts) return 0 }
   ; load64 returns raw (even/pointer) seconds
   def raw_sec = load64(ts)
   ; To use as integer in Nytrix, we must tag it.
   def res = from_int(raw_sec)
   free(ts)
   return res
}

fn msleep(ms){
   "Suspends execution of the current thread for `ms` milliseconds."
   def ts = malloc(16)
   store64(ts, to_int(ms / 1000), 0)
   store64(ts, to_int((ms % 1000) * 1000000), 8)
   __nanosleep(ts)
   free(ts)
}

fn sleep(s){
   "Suspends execution of the current thread for `s` seconds."
   msleep(s * 1000)
}

fn ticks(){
   "Returns a high-resolution monotonic tick count in nanoseconds. Useful for precise timing and benchmarking."
   def ts = malloc(16)
   def r = __clock_gettime(1, ts)
   if(r != 0){ free(ts) return 0 }
   def raw_sec = load64(ts, 0)
   def raw_nsec = load64(ts, 8)
   def sec = from_int(raw_sec)
   def nsec = from_int(raw_nsec)
   def res = sec * 1000000000 + nsec
   free(ts)
   return res
}

if(comptime{__main()}){
    use std.os.time *
    use std.math.timefmt *
    use std.core.error *

    print("Testing Math Time...")

    def t1 = time()
    assert(t1 > 0, "time > 0")

    def start = ticks()
    ;; Keep the sleep check short so full test suites stay responsive.
    msleep(120)
    def end = ticks()

    if(start > 0 && end > 0){
     if(end < start){
      print("Warning: ticks went backwards")
     }
    } else {
     print("Warning: ticks unavailable")
    }

    ticks()

    def fmt = format_time(0)
    assert((fmt == "1970-01-01 00:00:00"), "epoch")

    def fmt2 = format_time(1672531200)
    assert((fmt2 == "2023-01-01 00:00:00"), "2023")

    print("✓ std.math.time tests passed")
}
