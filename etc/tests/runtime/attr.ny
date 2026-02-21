use std.core *
use std.os *
use std.str *

if(comptime{ arch() == "x86_64" && os() == "windows" }){
    @naked
    fn naked_add_x86(a, b){
        asm("
            lea -1(%rcx, %rdx), %rax
            ret
        ", "")
    }
} elif(comptime{ arch() == "x86_64" }){
    @naked
    fn naked_add_x86(a, b){
        asm("
            lea -1(%rdi, %rsi), %rax
            ret
        ", "")
    }
} elif(comptime{ arch() == "aarch64" || arch() == "arm64" }){
    @naked
    fn naked_add_arm64(a, b){
        asm("
            add x0, x0, x1
            sub x0, x0, #1
            ret
        ", "")
    }
}

;; Test @naked attribute
def run_naked = env("NYTRIX_TEST_NAKED")
print("Testing @naked...")
if(run_naked == "1"){
    if(comptime{ arch() == "x86_64" }){
        print("Testing @naked for x86_64...")
        def res = naked_add_x86(10, 20)
        print("naked_add(10, 20) =", res)
        assert(res == 30, "naked_add failed")
        print("✓ @naked x86 passed")
    } elif(comptime{ arch() == "aarch64" || arch() == "arm64" }){
        print("Testing @naked for ARM64...")
        def res = naked_add_arm64(10, 20)
        print("naked_add(10, 20) =", res)
        assert(res == 30, "naked_add failed")
        print("✓ @naked ARM passed")
    }
} else {
    print("Skipping @naked execution (set NYTRIX_TEST_NAKED=1 to enable)")
}

;; Test Multi-Input ASM
print("Testing Multi-Input ASM...")
if(comptime{ arch() == "x86_64" }){
    def a = 100
    def b = 50
    def c = asm("lea -1($1, $2), $0", "=r,r,r", a, b)
    print("asm_add(100, 50) =", c)
    assert(c == 150, "multi-input asm failed")
} elif(comptime{ arch() == "aarch64" || arch() == "arm64" }){
    def a = 100
    def b = 50
    def c = asm("add $0, $1, $2\nsub $0, $0, #1", "=r,r,r", a, b)
    print("asm_add(100, 50) =", c)
    assert(c == 150, "multi-input asm failed")
}
print("✓ Multi-Input ASM passed")

print("Testing @jit attribute...")
@jit
fn fast_add(x, y){
    return x + y
}
assert(fast_add(10, 20) == 30, "@jit function call failed")
def fast_ref = fast_add
assert(fast_ref(7, 8) == 15, "@jit function pointer call failed")
print("✓ @jit passed")

print("Testing @thread attribute...")
@thread
fn worker_fn(base=41){
    print("  Worker running with base =", base)
    return base + 1
}
assert(worker_fn() == 42, "@thread default-arg call failed")
assert(worker_fn(99) == 100, "@thread one-arg call failed")

@thread
fn worker_sum(a, b, c=0){
    return a + b + c
}
assert(worker_sum(10, 20) == 30, "@thread multi-arg call failed")
assert(worker_sum(10, 20, 3) == 33, "@thread multi-arg default/explicit failed")

@thread
fn worker_sleep(ms=200){
    msleep(ms)
    return ms
}

def t0 = ticks()
worker_sleep(200) ;; statement call should detach (no implicit join)
def t1 = ticks()
def detach_ms = (t1 - t0) / 1000000
assert(detach_ms < 120, "@thread statement call should be non-blocking")
assert(worker_sleep(20) == 20, "@thread value call should still join/return value")
msleep(220)
print("✓ @thread passed")

print("Testing @pure and @effects attributes...")
@pure
fn pure_inc(x){
    return x + 1
}
assert(pure_inc(9) == 10, "@pure function failed")

@effects(none)
fn pure_mix(a, b, c=0){
    return a * b + c
}
assert(pure_mix(6, 7) == 42, "@effects(none) function failed")
assert(pure_mix(6, 7, 2) == 44, "@effects(none) default/explicit failed")

@effects(alloc)
fn make_pair_list(a, b){
    return [a, b]
}
def pair = make_pair_list(3, 4)
assert(len(pair) == 2, "@effects(alloc) list length failed")
assert(pair[0] == 3, "@effects(alloc) list item[0] failed")
assert(pair[1] == 4, "@effects(alloc) list item[1] failed")

@effects(io, alloc, ffi, thread)
fn echo_once(x){
    print("echo_once:", x)
    return x
}
assert(echo_once(77) == 77, "@effects(...) function failed")
print("✓ @pure/@effects passed")

print("Testing @llvm attribute...")
@llvm(noinline)
fn llvm_noinline_add(a, b){
    return a + b
}
assert(llvm_noinline_add(5, 7) == 12, "@llvm(noinline) function failed")

@llvm("frame-pointer", "all")
fn llvm_fp_add(a, b){
    return a + b
}
assert(llvm_fp_add(8, 9) == 17, "@llvm(name, value) function failed")
print("✓ @llvm passed")
