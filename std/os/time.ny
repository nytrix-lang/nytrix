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
   def r = syscall(228, 0, ts, 0,0,0,0) ; clock_gettime(CLOCK_REALTIME)
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
   syscall(35, ts, 0, 0, 0, 0, 0) ; nanosleep(ts, NULL)
   free(ts)
}

fn sleep(s){
   "Suspends execution of the current thread for `s` seconds."
   msleep(s * 1000)
}

fn ticks(){
   "Returns a high-resolution monotonic tick count in nanoseconds. Useful for precise timing and benchmarking."
   def ts = malloc(16)
   def r = syscall(228, 1, ts, 0,0,0,0) ; clock_gettime(CLOCK_MONOTONIC)
   if(r != 0){ free(ts) return 0 }
   def raw_sec = load64(ts, 0)
   def raw_nsec = load64(ts, 8)
   def sec = from_int(raw_sec)
   def nsec = from_int(raw_nsec)
   def res = sec * 1000000000 + nsec
   free(ts)
   return res
}
