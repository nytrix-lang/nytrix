use std.os.ffi as ffi
use std.core.error *

;; std.os.ffi (Test)
;; Tests basic FFI loading and symbol calling.

print("Testing FFI...")

mut h = ffi.dlopen("libc.so.6", 2)
if(h == 0){ h = ffi.dlopen("/lib/x86_64-linux-gnu/libc.so.6", 2) }
if(h == 0){ h = ffi.dlopen("/usr/lib/libc.so.6", 2) }

if(h != 0){
 print("Loaded libc handle:", h)
 def abs_f = ffi.dlsym(h, "llabs")
 if(abs_f != 0){
  mut res = ffi.call1(abs_f, -50)
  print("llabs(-50) =", res)
  assert(res == 50, "ffi llabs")
 } else {
  print("Warn: llabs symbol not found")
 }
 ffi.dlclose(h)
} else {
 print("Skipping FFI tests")
}

print("✓ FFI tests passed")

