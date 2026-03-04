;; Keywords: core test
;; Unit Testing Framework for Nytrix

module std.core.test (
   test, t_assert, t_assert_eq, fail
)
use std.core *
use std.os.sys *
use std.str *

fn t_assert(cond, msg="Assertion failed"){
   "Alias for `assert` used by test files."
   assert(cond, msg)
}

fn t_assert_eq(a, b, msg="Assertion failed"){
   "Asserts equality; on failure prints message and it will panic."
   assert_eq(a, b, msg)
}

fn fail(message){
   "Forces a test to fail."
   eprint("Test failed: ", message)
   __exit(1)
}

if(comptime{__main()}){
   ; Basic assertion helpers plus control-flow checks.

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
