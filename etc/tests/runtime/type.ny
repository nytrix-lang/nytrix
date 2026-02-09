use std.core *
use std.str.io *

;; Basic functions with explicit types

fn test_add(a: int, b: int): int {
    return a + b
}

fn get_name(): str {
    return "John"
}

fn process(data: str): str {
    return data
}

fn test_primitives() {
    def a: i8 = 10
    def b: i16 = 20
    def c: i32 = 30
    def d: i64 = 40
    def e: u8 = 50
    def f: u16 = 60
    def g: u32 = 70
    def h: u64 = 80
    def i: char = 'A'
    def j: bool = true
    def k: void = 0
    
    assert(a == 10, "i8 failed")
    assert(h == 80, "u64 failed")
    assert(j == true, "bool failed")
}

assert(test_add(10, 20) == 30, "add failed")
assert(get_name() == "John", "get_name failed")
assert(process("hello") == "hello", "process failed")

test_primitives()

print("✓ all runtime type tests passed")
