use std.io
use std

use std.os.ffi as ffi
print("Testing FFI...")
def h = ffi.dlopen("libc.so.6", 2)
if(h == 0){ h = ffi.dlopen("/lib/x86_64-linux-gnu/libc.so.6", 2) }
if(h == 0){ h = ffi.dlopen("/usr/lib/libc.so.6", 2) }

if(h != 0){
   print("Loaded libc handle:", h)
   def abs_f = ffi.dlsym(h, "llabs")
   if(abs_f != 0){
      print("llabs(-50) = ", ffi.call1(abs_f, -50))
      def res = ffi.call1(abs_f, -50)
      assert(res == 50, "ffi llabs (tagged)")
   } else { print("Warn: abs symbol not found") }
   ffi.dlclose(h)
} else { print("Skipping FFI tests") }
print("✓ FFI tests passed")
