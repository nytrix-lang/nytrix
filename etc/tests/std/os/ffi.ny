use std.core *
use std.os.ffi as ffi
use std.core.error *

;; std.os.ffi (Test)
;; Tests basic FFI loading and symbol calling.

print("Testing FFI...")

mut h = ffi.dlopen("libc.so.6", ffi.RTLD_NOW())
if(h == 0){ h = ffi.dlopen("/lib/x86_64-linux-gnu/libc.so.6", ffi.RTLD_NOW()) }
if(h == 0){ h = ffi.dlopen("/usr/lib/libc.so.6", ffi.RTLD_NOW()) }
if(h == 0){ h = ffi.dlopen("libc.so", ffi.RTLD_NOW()) }

if(h != 0){
 print("Loaded libc handle:", h)
 
 ; Test old-style call
 def abs_f = ffi.dlsym(h, "llabs")
 if(abs_f != 0){
  mut res = ffi.call1(abs_f, -50)
  print("llabs(-50) =", res)
  assert(res == 50, "ffi llabs")
 }

 ; Test bind_all
 def names = ["malloc", "free", "getpid"]
 def lib = ffi.bind_all(h, names)
 if(lib.getpid != 0){
    def pid = lib.getpid()()
    print("PID from bind_all lib (dot access):", pid)
    assert(typeof(pid) == "int", "pid is int")
    assert(pid > 0, "getpid works")
    
    ; Test import_all (manual libc handle)
    ffi.import_all(h, ["getpid"])
   ; Now getpid should be in global scope
   def g = globals()
   print("Global table type:", typeof(g))
   def f = get(g, "getpid")
   print("getpid value from globals:", f)
   
   def pid2 = getpid()
   print("PID from global getpid (direct call):", pid2)
   assert(pid2 == pid, "global getpid works")

   ; Test extern_all (linked symbols)
   ffi.extern_all([["getpid", 0]])
   def pid3_raw = getpid()
   def pid3_tag = from_int(pid3_raw)
   print("PID from extern getpid:", pid3_tag)
   assert(pid3_tag > 0, "extern getpid works")
   assert(pid3_raw == to_int(pid), "extern getpid matches ffi pid")
 }

 ffi.dlclose(h)
} else {
 print("Skipping FFI tests (libc not found)")
}

print("✓ FFI tests passed")
