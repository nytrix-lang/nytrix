;;; timefmt.ny --- math time formatting helpers

;; Keywords: math timefmt

;;; Commentary:

;; Provide helpers to format unix timestamps using libc's gmtime_r/strftime.

use std.core
use std.core.error
use std.strings.str
use std.os.ffi
module std.math.timefmt (
	format_time
)

def _libc = 0

def _gmtime_r_fn = 0

def _strftime_fn = 0

fn _ensure_libc(){
	if(_libc != 0){ return _libc }
	def handle = dlopen("libc.so.6", RTLD_LAZY)
	if(handle == 0){ handle = dlopen("libc.so", RTLD_LAZY) }
	if(handle == 0){ handle = dlopen("/usr/lib/libc.dylib", RTLD_LAZY) }
	if(handle == 0){ handle = dlopen("/lib/x86_64-linux-gnu/libc.so.6", RTLD_LAZY) }
	if(handle == 0){ panic("timefmt: failed to load libc") }
	_libc = handle
	return handle
}

fn _ensure_fns(){
	_ensure_libc()
	if(_gmtime_r_fn == 0){
		_gmtime_r_fn = dlsym(_libc, "gmtime_r")
		if(_gmtime_r_fn == 0){ panic("timefmt: gmtime_r not available") }
	}
	if(_strftime_fn == 0){
		_strftime_fn = dlsym(_libc, "strftime")
		if(_strftime_fn == 0){ panic("timefmt: strftime not available") }
	}
}

fn format_time(ts){
	_ensure_fns()
	def raw = to_int(ts)
	def tbuf = rt_malloc(8)
	store64(tbuf, raw, 0)
	def tm_buf = rt_malloc(128)
	def got = call2(_gmtime_r_fn, tbuf, tm_buf)
	rt_free(tbuf)
	if(got == 0){
		rt_free(tm_buf)
		panic("timefmt: gmtime_r failed")
	}
	def out = rt_malloc(32)
	def len = call4(_strftime_fn, out, 32, "%Y-%m-%d %H:%M:%S", tm_buf)
	rt_free(tm_buf)
	if(len == 0){
		rt_free(out)
		panic("timefmt: strftime failed")
	}
	def result = cstr_to_str(out)
	rt_free(out)
	return result
}
