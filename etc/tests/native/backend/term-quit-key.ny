use std.core
use std.core.term
use std.os (file_exists)

assert_eq(is_quit_key(27), true)
assert_eq(is_quit_key(3), true)
assert_eq(is_quit_key(0), false)
assert_eq(is_quit_key(113), false)
assert_eq(file_exists("."), true)
