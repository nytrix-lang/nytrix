;;; time.ny --- os time module

;; Keywords: os time

;;; Commentary:

;; Os Time module.

use std.core
module std.os.time (
	time, sleep, msleep, ticks
)

fn time(){
	"Unix time in seconds (epoch timestamp)."
	def ts = rt_malloc(16)
	def r = rt_syscall(228, 0, ts, 0,0,0,0) ; "clock_gettime(CLOCK_REALTIME)"
	if(r != 0){ rt_free(ts) return 0 }
	; load64 returns raw (even/pointer) seconds
	def raw_sec = load64(ts)
	; To use as integer in Nytrix, we must tag it.
	def res = from_int(raw_sec)
	rt_free(ts)
	return res
}

fn sleep(s){
	"Sleep seconds."
	return rt_sleep(s * 1000)
}

fn msleep(ms){
	"Sleep milliseconds."
	return rt_sleep(ms)
}

fn ticks(){
	"Raw monotonic ticks (nanoseconds)."
	def ts = rt_malloc(16)
	def r = rt_syscall(228, 1, ts, 0,0,0,0) ; "clock_gettime(CLOCK_MONOTONIC)"
	if(r != 0){ rt_free(ts) return 0 }
	def raw_sec = load64(ts, 0)
	def raw_nsec = load64(ts, 8)
	def sec = from_int(raw_sec)
	def nsec = from_int(raw_nsec)
	def res = sec * 1000000000 + nsec
	rt_free(ts)
	return res
}
