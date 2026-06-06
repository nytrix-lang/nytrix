;; Keywords: ct constant-time side-channel math crypto
;; Constant-time cryptography primitives for Nytrix
;; These functions use branchless operations to prevent timing side-channel attacks
;; References:
;; - std.math.crypto
module std.math.crypto.ct(ct_select, ct_compare, ct_eq, ct_neq, ct_lt, ct_le, ct_gt, ct_ge, ct_min, ct_max, ct_abs, ct_swap, ct_cond_negate, CT_ZERO, CT_ONE)
use std.core
use std.core.error

def CT_ZERO = 0
def CT_ONE = 1

fn ct_select(int a, int b, int condition) int {
   "Constant-time conditional select.
   Returns `a` if condition != 0, else `b`.
   Executes in constant time - no data-dependent branches."
   __ct_select(a, b, condition)
}

fn ct_compare(ptr a, ptr b, int len) int {
   "Constant-time byte buffer comparison.
   Returns 0 if buffers are equal, non-zero otherwise.
   Uses XOR-accumulate to avoid data-dependent branches.
   a, b: pointers to byte buffers
   len: number of bytes to compare"
   __ct_compare(a, b, len)
}

fn ct_eq(int a, int b) int {
   "Constant-time equality comparison for integers.
   Returns 1 if a == b, 0 otherwise. No branching."
   def x = a ^^ b
   def neg_x = 0 - x
   def or_val = x | neg_x
   1 - ((or_val >> 63) & 1)
}

fn ct_neq(int a, int b) int {
   "Constant-time not-equal comparison for integers."
   1 - ct_eq(a, b)
}

fn ct_lt(int a, int b) int {
   "Constant-time less-than comparison for integers.
   Returns 1 if a < b, 0 otherwise. No branching."
   def diff = a - b
   (diff >> 63) & 1
}

fn ct_le(int a, int b) int {
   "Constant-time less-than-or-equal comparison for integers."
   1 - ct_gt(a, b)
}

fn ct_gt(int a, int b) int {
   "Constant-time greater-than comparison for integers."
   ct_lt(b, a)
}

fn ct_ge(int a, int b) int {
   "Constant-time greater-than-or-equal comparison for integers."
   1 - ct_lt(a, b)
}

fn ct_min(int a, int b) int {
   "Constant-time minimum of two integers.
   Returns the smaller value without branching."
   ct_select(a, b, ct_lt(a, b))
}

fn ct_max(int a, int b) int {
   "Constant-time maximum of two integers."
   ct_select(b, a, ct_lt(a, b))
}

fn ct_abs(int a) int {
   "Constant-time absolute value for signed integers.
   Returns |a| without branching on the sign."
   def mask = a >> 63
   def xored = a ^^ mask
   xored - mask
}

fn ct_swap(int a, int b, int condition) list {
   "Constant-time conditional swap.
   Returns [b, a] if condition != 0, else [a, b].
   No branching based on condition."
   def x = a ^^ b
   def mask_val = condition | (0 - condition)
   def mask_shifted = (mask_val >> 63)
   def mask = 0 - mask_shifted
   def masked = x & mask
   [a ^^ masked, b ^^ masked]
}

fn ct_cond_negate(int a, int condition) int {
   "Constant-time conditional negate.
   Returns -a if condition != 0, else a.
   No branching based on condition."
   def mask_val = condition | (0 - condition)
   def mask_shifted = (mask_val >> 63)
   def mask = 0 - mask_shifted
   def neg_a = 0 - a
   ct_select(neg_a, a, mask)
}
