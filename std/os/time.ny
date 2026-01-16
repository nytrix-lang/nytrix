;; Keywords: os time
;; Os Time module.

use std.core
module std.os.time (
   time, sleep, msleep, ticks
)

fn time(){
   "Unix time in seconds (epoch timestamp)."
   def ts = __malloc(16)
   def r = __syscall(228, 0, ts, 0,0,0,0) ; "clock_gettime(CLOCK_REALTIME)"
   if(r != 0){ __free(ts) return 0 }
   ; load64 returns raw (even/pointer) seconds
   def raw_sec = load64(ts)
   ; To use as integer in Nytrix, we must tag it.
   def res = from_int(raw_sec)
   __free(ts)
   return res
}

fn msleep(ms){
   "Sleep milliseconds."
   def ts = __malloc(16)
   store64(ts, to_int(ms / 1000), 0)
   store64(ts, to_int((ms % 1000) * 1000000), 8)
   __syscall(35, ts, 0, 0, 0, 0, 0) ; nanosleep(ts, NULL)
   __free(ts)
}

fn sleep(s){
   "Sleep seconds."
   msleep(s * 1000)
}

fn ticks(){
   "Raw monotonic ticks (nanoseconds)."
   def ts = __malloc(16)
   def r = __syscall(228, 1, ts, 0,0,0,0) ; "clock_gettime(CLOCK_MONOTONIC)"
   if(r != 0){ __free(ts) return 0 }
   def raw_sec = load64(ts, 0)
   def raw_nsec = load64(ts, 8)
   def sec = from_int(raw_sec)
   def nsec = from_int(raw_nsec)
   def res = sec * 1000000000 + nsec
   __free(ts)
   return res
}