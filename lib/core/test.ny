;; Keywords: test testing assertions
;; Unit Testing Framework for Nytrix
module std.core.test(fail)
use std.core
use std.os.sys

fn fail(str: message): int {
   "Forces a test to fail."
   eprint("Test failed: ", message)
   __exit(1)
}
