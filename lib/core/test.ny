;; Keywords: test testing assertions core
;; Unit Testing Framework for Nytrix
;; References:
;; - std.core
module std.core.test(fail, assert_eq, assert_ne, assert_gt, assert_lt, assert_approx_eq)
use std.core
use std.os.sys

fn fail(str message) int {
   "Forces a test to fail."
   eprint("Test failed: ", message)
   __exit(1)
}

fn assert_eq(any actual, any expected, str message="assert eq failed") int {
   "Fails unless `actual` and `expected` are structurally equal."
   if(!eq(actual, expected)){ fail(message + ": expected " + to_str(expected) + ", got " + to_str(actual)) }
   0
}

fn assert_ne(any actual, any unexpected, str message="assert ne failed") int {
   "Fails when `actual` and `unexpected` are structurally equal."
   if(eq(actual, unexpected)){ fail(message + ": did not expect " + to_str(unexpected)) }
   0
}

fn assert_gt(any actual, any threshold, str message="assert gt failed") int {
   "Fails unless `actual > threshold`."
   if(!(actual > threshold)){ fail(message + ": expected " + to_str(actual) + " > " + to_str(threshold)) }
   0
}

fn assert_lt(any actual, any threshold, str message="assert lt failed") int {
   "Fails unless `actual < threshold`."
   if(!(actual < threshold)){ fail(message + ": expected " + to_str(actual) + " < " + to_str(threshold)) }
   0
}

fn assert_approx_eq(number actual, number expected, number epsilon=0.000001, str message="assert approx eq failed") int {
   "Fails unless two numbers differ by at most `epsilon`."
   mut diff = actual - expected
   if(diff < 0){ diff = 0 - diff }
   if(diff > epsilon){ fail(message + ": expected " + to_str(expected) + " +/- " + to_str(epsilon) + ", got " + to_str(actual)) }
   0
}

#main {
   assert_eq(2 + 2, 4, "test assert_eq")
   assert_ne(2 + 2, 5, "test assert_ne")
   assert_gt(5, 3, "test assert_gt")
   assert_lt(3, 5, "test assert_lt")
   assert_approx_eq(1.0, 1.0000001, 0.00001, "test assert_approx_eq")
   print("✓ std.core.test self-test passed")
}
