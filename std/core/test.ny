;; Keywords: core test
;; Core Test module.

use std.core
module std.core.test (
   STDERR_FD, write_stderr, t_assert, t_asse__eq, fail
)

def STDERR_FD = 2

fn write_stderr(s){
   "Writes the given string to stderr."
   __syscall(1, STDERR_FD, s, str_len(s))
}

fn t_assert(condition, message){
   "Asserts that a condition is true. If not, prints a message and exits."
   if(!condition){
      write_stderr("Assertion failed: ")
      write_stderr(message)
      write_stderr("\n")
      __syscall(60, 1, 0,0,0,0,0)
   }
}

fn t_asse__eq(a, b, message){
   "Asserts that two values are equal."
   if(a != b){
      write_stderr("Equality assertion failed: ")
      write_stderr(message)
      write_stderr("\n")
      write_stderr("  Expected: ")
      write_stderr(b)
      write_stderr("\n")
      write_stderr("  Got: ")
      write_stderr(a)
      write_stderr("\n")
      __syscall(60, 1, 0,0,0,0,0)
   }
}

fn fail(message){
   "Forces a test to fail."
   write_stderr("Test failed: ")
   write_stderr(message)
   write_stderr("\n")
   __syscall(60, 1, 0,0,0,0,0)
}