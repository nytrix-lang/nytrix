use std.core
use std.core.error
use std.core.io
use std.os.sys

layout ExternDivResult {
   i32: quot,
   i32: rem
}

if(comptime{ __os_name() == "windows" }){
   extern "" {
      fn c_getpid(): i32 as "GetCurrentProcessId"
      fn my_getpid(): i32 as "GetCurrentProcessId"
   }
} else {
   extern "" {
      fn c_getpid(): i32 as "getpid"
      fn my_getpid(): i32 as "getpid"
   }
   ;; `extern "c"` expands to a link declaration plus extern fn declarations.
   ;; Functions inside the block default their native symbol to the local name.
   extern "c" {
      fn getpid(): i32
      fn c_div(i32: numer, i32: denom): ExternDivResult as "div"
   }
}

print("Testing runtime extern support via libc...")
def pid = c_getpid()

;; print("c_getpid returned:", pid)
def my_pid = my_getpid()
assert(my_pid == pid, "my_getpid(aliased) works")

if(comptime{ __os_name() != "windows" }){
   def i32: block_pid = getpid()
   assert(block_pid > 0, "extern block getpid")
   assert(block_pid == pid, "extern block getpid matches aliased getpid")
   def div_result = c_div(17, 5)
   assert(load_layout(div_result, "ExternDivResult", "quot") == 3, "extern layout ABI return quot")
   assert(load_layout(div_result, "ExternDivResult", "rem") == 2, "extern layout ABI return rem")
   free(div_result)
}

print("✓ runtime extern test passed")
