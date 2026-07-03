use std.core
use std.core.reflect
use std.math.nt
use std.math.big
use std.os
use std.core.str
use std.core.error
use std.core.io

;; --- Unary & Operator Tests (formerly unary.ny) ---
def a = 10
def b = -a
assert(b == -10, "unary negation variable")
def c = -5
assert(c == -5, "unary negation literal")
def d = ~0
assert(d == -1, "bitwise not zero")
def e = ~1
assert(e == -2, "bitwise not one")
def f = 0
assert(-f == 0, "unary negation zero")
mut x = 100
x = -x
assert(x == -100, "unary negation mutable")
mut y = 10
y = ~y
assert(y == -11, "bitwise not mutable")

;; Large numbers (within 63-bit range)
def big = 9223372036854775807
def big_neg = -big
assert(big_neg == -9223372036854775807, "unary negation max int")
assert(-5 == -5, "negation of positive")
assert(-(-5) == 5, "negation of negative")
assert(~0 == -1, "bitwise not of 0")
assert(~(-1) == 0, "bitwise not of -1")
def v_a = 10
def v_b = -v_a
assert(v_b == -10, "var negation")
def v_c = ~v_a
assert(v_c == -11, "var bitwise not")

;; Increment / Decrement
mut count = 5
++count
assert(count == 6, "++pre-increment")
--count
assert(count == 5, "--pre-decrement")

;; Increment in while loop header
mut loop_i = 0
mut loop_sum = 0
mut i_seq = 0
while i_seq < 10 ++i_seq {
   loop_sum = loop_sum + i_seq
}

assert(loop_sum == 45, "loop sequence failed")

;; Compound Assignment
mut val = 10
val += 5
assert(val == 15, "+= failed")
val -= 3
assert(val == 12, "-= failed")
val *= 2
assert(val == 24, "*= failed")
val /= 4
assert(val == 6, "/= failed")
val %= 4
assert(val == 2, "%= failed")
print("✓ unary and operator tests passed")

;; --- BigInt Tests (formerly bigint.ny) ---
def n_bi = 12345
def x1_bi = __bigint_from_int(n_bi)
def x2_bi = bigint_from_int(n_bi)
def x3_bi = nt_bigint(n_bi)
def x4_bi = Z(n_bi)
def bigint_tag = __runtime_tag("bigint")
assert(__tagof(x1_bi) == bigint_tag, "__bigint_from_int tag")
assert(__tagof(x2_bi) == bigint_tag, "bigint_from_int tag")
assert(__tagof(x3_bi) == bigint_tag, "nt_bigint tag")
assert(__tagof(x4_bi) == bigint_tag, "Z tag")
assert(__bigint_to_str(x1_bi) == "12345", "__bigint_to_str from int")
assert(__bigint_to_str(x2_bi) == "12345", "__bigint_to_str bigint_from_int")
assert(__bigint_to_str(x4_bi) == "12345", "__bigint_to_str Z")
assert(type(x4_bi) == "bigint", "type(Z(12345))")
assert(to_str(x4_bi) == "12345", "to_str(Z(12345))")
assert(bigint_eq(Z(2), Z(2)), "bigint_eq basic")
assert(Z(2) == Z(2), "== bigint basic")
assert(Z(2) != Z(3), "!= bigint basic")
def mul_bi = bigint_mul(Z(3), Z(4))
assert(to_str(mul_bi) == "12", "bigint_mul + to_str")
assert(bigint_to_str(mul_bi) == "12", "bigint_to_str")
def r8_bi = nth_root(Z(8), 3)
assert(r8_bi != nil, "nth_root returns")
assert(r8_bi == Z(2), "nth_root cbrt(8)")
def sugar_x = Z(21)
assert(sugar_x.str == "21", "inferred bigint property str")
assert(sugar_x.bits == 5, "inferred bigint property bits")
assert(Z(2).pow_int(8).str == "256", "bigint method on constructor")
assert(Z(5).xor(Z(2)).str == "7", "bigint xor method")
assert((Z(2)^Z(8)).str == "256", "bigint power operator")
assert((Z(5)^^Z(2)).str == "7", "bigint xor operator")
assert((5^^Z(2)).str == "7", "int bigint xor operator")
assert(Z(4).sqrt_mod(17).powmod(2, 17) == Z(4), "bigint modular sqrt method")
def qroots = Z(1).quadratic_roots_mod(0, -4, 17)
assert(qroots.len == 2, "bigint quadratic roots method")
assert(bigint_xor(Z(255), Z(2)).str == "253", "bigint xor helper")
assert((Z(8) + Z(13)).str == "21", "bigint operator sugar")
assert(bigint_add(bigint_mul(Z(1), Z(16)), Z(5)).str == "21", "nt reexported bigint helpers")
assert(__bigint_submul(Z(20), Z(3), Z(4)) == Z(8), "__bigint_submul direct")
assert(__bigint_modinv(Z(3), Z(11)) == Z(4), "__bigint_modinv direct")
assert(__bigint_modinv(Z(2), Z(4)) == Z(0), "__bigint_modinv no inverse")
assert(__bigint_iroot(Z(27), 3) == Z(3), "__bigint_iroot direct")
assert(__bigint_clz(Z(1)) == 63, "__bigint_clz single bit")
assert(__bigint_clz(Z(2)^Z(63)) == 0, "__bigint_clz full top limb")
assert(__bigint_ctz(Z(40)) == 3, "__bigint_ctz direct")
static_assert([1, 2, 3].long == 0x010203, "static list .long")
static_assert("ABC".long == 0x414243, "static string .long")
static_assert(123.long == 123, "static int .long")
assert([1, 2, 3].long == Z(0x010203), "list .long")
assert("ABC".long == Z(0x414243), "string .long")
assert(123.long == Z(123), "int .long")
assert(1.9.long == Z(1), "float .long")
def list_long_bytes = [1, 2, 3].long.bytes
assert(type_shape(list_long_bytes) == "list<int>", ".long.bytes keeps typed byte list")
assert(list_long_bytes == [1, 2, 3], ".long.bytes roundtrip")
assert("010203".unhex.long.bytes == [1, 2, 3], ".unhex.long.bytes roundtrip")
assert([1, 2, 3].long.as_bytes.long.bytes == [1, 2, 3], "bigint bytes aliases chain")
print("✓ bigint tests passed")

;; --- Inline ASM Tests (formerly asm.ny) ---
def arch_name = arch()
mut platform_guard = "other"
#unix {
   platform_guard = "unix"
}

#elif windows {
   platform_guard = "windows"
}

#else {
   platform_guard = "other"
}

#endif
#windows {
   assert(platform_guard == "windows", "#windows platform guard")
}

#else {
   assert(platform_guard == "unix", "#unix platform guard")
}

#endif
#x86 {
   print("Testing inline assembly for x86...")
   mut x_asm = asm("mov $1, $0", "=r,r", 42)
   assert(x_asm == 42, "asm return 42")
   mut s_asm = asm("mov $1, $0", "=r,r", 123)
   print("✓ std.core.asm[x86] tests passed")
}

#elif arm || aarch64 {
   print("Testing inline assembly for ARM...")
   mut x_asm = asm("mov $0, $1", "=r,r", 42)
   mut x_check = x_asm
   #if arm && !aarch64 {
      x_check = x_asm & 4294967295
   }
   assert(x_check == 42, "asm return 42")
   print("✓ std.core.asm[arm] tests passed")
}

#endif

;; --- Attributes & Advanced ASM (formerly attr.ny) ---
if comptime { arch() == "x86_64" && os() == "windows" } {
   @naked
   fn naked_add_x86(a, b) {
      asm("
         lea -1(%rcx, %rdx), %rax
         ret
      ", "")
   }
} elif comptime { arch() == "x86_64" } {
   @naked
   fn naked_add_x86(a, b) {
      asm("
         lea -1(%rdi, %rsi), %rax
         ret
      ", "")
   }
} elif comptime { arch() == "aarch64" || arch() == "arm64" } {
   @naked
   fn naked_add_arm64(a, b) {
      asm("
         add x0, x0, x1
         sub x0, x0, #1
         ret
      ", "")
   }
}

def run_naked = env("NYTRIX_TEST_NAKED")

if run_naked == "1" {
   if comptime { arch() == "x86_64" } {
      def res = naked_add_x86(10, 20)
      assert(res == 30, "naked_add failed")
   } elif comptime { arch() == "aarch64" || arch() == "arm64" } {
      def res = naked_add_arm64(10, 20)
      assert(res == 30, "naked_add failed")
   }
}

if comptime { arch() == "x86_64" } {
   def a_asm = 100
   def b_asm = 50
   def c_asm = asm("lea -1($1, $2), $0", "=r,r,r", a_asm, b_asm)
   assert(c_asm == 150, "multi-input asm failed")
} elif comptime { arch() == "aarch64" || arch() == "arm64" } {
   def a_asm = 100
   def b_asm = 50
   def c_asm = asm("add $0, $1, $2\nsub $0, $0, #1", "=r,r,r", a_asm, b_asm)
   assert(c_asm == 150, "multi-input asm failed")
}

@jit
fn fast_add(x, y) {
   return x + y
}

assert(fast_add(10, 20) == 30, "@jit function call failed")

@thread
fn worker_fn(base=41) {
   return base + 1
}

assert(worker_fn() == 42, "@thread default-arg call failed")

@pure
fn pure_inc(x) {
   return x + 1
}

assert(pure_inc(9) == 10, "@pure function failed")

fn backend_ctpop(x) {
   backend_intrinsic("ctpop.i64", x)
}

assert(backend_ctpop(0xf0f0) == 8, "backend_intrinsic(...) ctpop intrinsic failed")

@hot
@jit
fn hot_loop(n) {
   mut sum = 0
   mut i = 0
   while i < n {
      sum = sum + i
      i += 1
   }
   return sum
}

assert(hot_loop(10) == 45, "@hot function failed")
print("✓ attributes and advanced asm tests passed")

;; --- Struct Tests (formerly struct.ny) ---
struct Vec2 {
   i32 x,
   i32 y
}

struct Mixed {
   bool flag,
   i32 count,
   i64 total
}

struct PackedPair pack(1){
   i32 left,
   i64 right
}

assert(__layout_size("Vec2") == 8, "Vec2 size")
assert(__layout_offset("Vec2", "y") == 4, "Vec2.y offset")
assert(__layout_size("PackedPair") == 12, "PackedPair align")
print("✓ struct tests passed")
print("✓ runtime_core tests passed")
