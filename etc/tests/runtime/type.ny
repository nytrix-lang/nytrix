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

fn maybe_int(flag: bool): ?int {
    if(flag){ return 7 }
    return nil
}

fn read_opt(v: ?int): ?int {
    return v
}

fn need_int(v: int): int {
    return v
}

fn is_nonzero(v: int): bool {
    return v != 0
}

fn test_primitives(){
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

fn test_null_contracts(){
    def p: *int = nil
    assert(p == 0, "typed pointer nil assignment failed")
    def o: ?int = nil
    assert(o == 0, "typed nullable int nil assignment failed")
    def q: ?int = 9
    assert(q == 9, "typed nullable int value assignment failed")
}

fn test_flow_null_narrowing(){
    def a: ?int = 5
    if(a != nil){
        def v: int = a
        assert(v == 5, "if x != nil narrowing failed")
        assert(need_int(a) == 5, "call-site narrowing failed")
    }

    def b: ?int = 6
    if(b == nil){
        assert(false, "unexpected nil in else-narrowing test")
    }else{
        def w: int = b
        assert(w == 6, "else branch narrowing failed")
        assert(need_int(b) == 6, "else branch call-site narrowing failed")
    }

    def c: ?int = 7
    if(nil != c){
        def x: int = c
        assert(x == 7, "reversed nil != x narrowing failed")
    }

    def d: ?int = 8
    if(nil == d){
        assert(false, "unexpected nil in reversed equality narrowing test")
    }else{
        def y: int = d
        assert(y == 8, "reversed nil == x else narrowing failed")
    }

    def e: ?int = 10
    if(e != nil && need_int(e) == 10){
        assert(true, "logical && rhs narrowing failed")
    }else{
        assert(false, "logical && rhs narrowing branch failed")
    }

    def f: ?int = 11
    if(f == nil || need_int(f) == 11){
        assert(true, "logical || rhs narrowing failed")
    }else{
        assert(false, "logical || rhs narrowing branch failed")
    }

    def g: ?int = 12
    if(g == nil || false){
        assert(false, "logical || else branch narrowing setup failed")
    }else{
        def z: int = g
        assert(z == 12, "logical || else branch narrowing failed")
    }

    mut h: ?int = 13
    if(h != nil){
        h = nil
        assert(h == 0, "mutable nullable assignment after narrowing failed")
    }

    def i: ?int = 14
    if((i != nil) && is_nonzero(i)){
        assert(true, "nested logical narrowing failed")
    }else{
        assert(false, "nested logical narrowing branch failed")
    }

    def j: ?int = 2
    def k: ?int = 3
    if(j != nil && k != nil){
        def sum: int = need_int(j) + need_int(k)
        assert(sum == 5, "multi-var && branch narrowing failed")
    }else{
        assert(false, "multi-var && branch should be true")
    }

    def m: ?int = 4
    def n: ?int = 5
    if(m == nil || n == nil){
        assert(false, "multi-var || else narrowing setup failed")
    }else{
        def mv: int = m
        def nv: int = n
        assert(mv + nv == 9, "multi-var || else branch narrowing failed")
    }
}

assert(test_add(10, 20) == 30, "add failed")
assert(get_name() == "John", "get_name failed")
assert(process("hello") == "hello", "process failed")
assert(maybe_int(true) == 7, "nullable return value failed")
assert(maybe_int(false) == 0, "nullable nil return failed")
assert(read_opt(nil) == 0, "nullable param nil failed")
assert(read_opt(5) == 5, "nullable param value failed")

test_primitives()
test_null_contracts()
test_flow_null_narrowing()

print("✓ all runtime type tests passed")
