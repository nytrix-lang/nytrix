;; Keywords: core test
;; Core Test module.

module std.core.test (
   assert, test, STDERR_FD, write_stderr, t_assert, t_assert_eq, fail
)
use std.core *
use std.os.sys *
use std.str *

def STDERR_FD = 2

fn write_stderr(s){
   "Writes the given string to stderr."
   unwrap(sys_write(STDERR_FD, s, str_len(s)))
}

fn assert(cond, msg="Assertion failed"){
   "Asserts `cond`; on failure prints message and exits with status 1."
   if(cond){ 1 }
   else {
      def s = f"Generic Assertion Failed: {msg}\n"
      write_stderr(s)
      __exit(1)
   }
}

fn t_assert(cond, msg="Assertion failed"){
   "Alias for `assert` used by test files."
   assert(cond, msg)
}

fn t_assert_eq(a, b, msg="Assertion failed"){
   "Asserts equality; on failure prints message and exits with status 1."
   if(a != b){
      def s = f"Assertion Eq Failed: {a} != {b} - {msg}\n"
      write_stderr(s)
      __exit(1)
   }
}

fn fail(message){
   "Forces a test to fail."
   write_stderr("Test failed: ")
   write_stderr(message)
   write_stderr("\n")
   __exit(1)
}

if(comptime{__main()}){
    ;; Basic assertion helpers plus control-flow checks.

    assert(true, "assert true")
    t_assert(true, "t_assert true")
    t_assert_eq(2 + 2, 4, "t_assert_eq")
    t_assert_eq(STDERR_FD, 2, "stderr fd")

    mut x = 1
    if x == 1 {
       assert(true, "if works")
    } else {
       assert(false, "else failed")
    }

    x = 2
    if x == 1 {
       assert(false, "if failed")
    } elif x == 2 {
       assert(true, "elif works")
    } else {
       assert(false, "else failed")
    }

    x = 4
    if x == 1 {
       assert(false, "if failed")
    } elif x == 2 {
       assert(false, "elif failed")
    } else {
       assert(true, "else works")
    }
}
