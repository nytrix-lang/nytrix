;; Keywords: core test
;; Core Test module.

use std.core *
use std.os.sys *
module std.core.test (
   assert, test, STDERR_FD, write_stderr, t_assert, t_assert_eq, fail
)

def STDERR_FD = 2

fn write_stderr(s){
   "Writes the given string to stderr."
   syscall(1, STDERR_FD, s, str_len(s), 0,0,0)
}

fn assert(cond, msg="Assertion failed"){
   if(cond){ 1 }
   else {
      def s = f"Generic Assertion Failed: {msg}\n"
      write_stderr(s)
      syscall(60, 1, 0,0,0,0,0)
   }
}

fn t_assert(cond, msg="Assertion failed"){
   assert(cond, msg)
}

fn t_assert_eq(a, b, msg="Assertion failed"){
   if(a != b){
   }
}

fn fail(message){
   "Forces a test to fail."
   write_stderr("Test failed: ")
   write_stderr(message)
   write_stderr("\n")
   syscall(60, 1, 0,0,0,0,0)
}
