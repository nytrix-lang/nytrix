;; expect: cannot assign string literal to int
use std.core

def int: value = "not an int"
print(value)
