;; Keywords: math bigint
;; Math Bigint module.

module std.math.bigint (
   is_bigint, _big_make, _big_digits, _big_sign, _big_abs_cmp, _big_from_int, bigint,
   bigint_from_str, bigint_to_str, _big_add_abs, _big_sub_abs, bigint_add, bigint_sub,
   _big_mul_abs, bigint_mul, _big_mul_small, _big_add_small, _digits_prepend,
   _big_divmod_abs, bigint_div, bigint_mod, bigint_cmp, bigint_eq
)
use std.core *
use std.core as core
use std.core.error *
use std.str *
use std.str.io *

fn is_bigint(x){
   "Returns **true** if `x` is a [[std.math.bigint::bigint]] object."
   if(eq(is_list(x), false)){ return false }
   if(eq(core.len(x) < 3, true)){ return false }
   return eq(load64(x, 16), 107)
}

fn _big_make(sign, digits){
   "Internal: build bigint with sign and digits (lsf), normalize."
   mut actual_digits = list_clone(digits)
   mut n_actual = core.len(actual_digits)
   while(n_actual > 0 && get(actual_digits, n_actual - 1) == 0){
      pop(actual_digits)
      n_actual -= 1
   }
   if(n_actual == 0){
      sign = 0
   }
   mut out = list(3)
    out = append(out, 107)
    out = append(out, sign)
    out = append(out, actual_digits)
   out
}

fn _big_digits(b){
   "Internal: return digits list."
   load64(b, 32)
}

fn _big_sign(b){
   "Internal: return sign."
   load64(b, 24)
}

fn _big_abs_cmp(a, b){
   "Internal: compare |a| and |b|, returns -1/0/1."
   mut da = _big_digits(a)
   def db = _big_digits(b)
   mut na = core.len(da)
   mut nb = core.len(db)
   if(na < nb){ return -1 }
   if(na > nb){ return 1 }
   mut i = na - 1
   while(i >= 0){
      mut va = get(da, i)
      mut vb = get(db, i)
      if(va < vb){ return -1 }
      if(va > vb){ return 1 }
      i -= 1
   }
   return 0
}

fn _big_from_int(n){
   "Internal: build bigint from int."
   if(n == 0){ return _big_make(0, list(0)) }
   mut sign = 1
   if(n < 0){ sign = -1  n = -n }
   mut digits = list(4)
   while(n > 0){
       digits = append(digits, n % 1000000000)
      n /= 1000000000
   }
   _big_make(sign, digits)
}

fn bigint(x){
   "Convert an integer, string, or existing bigint to a [[std.math.bigint::bigint]] object."
   if(is_bigint(x)){ return x }
   if(is_int(x)){ return _big_from_int(x) }
   if(is_str(x)){ return bigint_from_str(x) }
   _big_make(0, list(0))
}

fn bigint_from_str(s){
   "Parses a decimal string into a [[std.math.bigint::bigint]]."
   if(str_len(s) == 0){ return _big_make(0, list(0)) }
   mut sign = 1
   mut i = 0
   if(load8(s, 0) == 45){
      sign = -1
      i = 1
   }
   mut res = _big_make(0, list(0))
   mut n = str_len(s)
   while(i < n){
      def c = load8(s, i)
      if(c >= 48 && c <= 57){
         res = _big_mul_small(res, 10)
         res = _big_add_small(res, c - 48)
      }
      i += 1
   }
   def digs = list_clone(_big_digits(res))
   if(core.len(digs) == 0){ return _big_make(0, list(0)) }
   _big_make(sign, digs)
}

fn bigint_to_str(b){
   "Converts a [[std.math.bigint::bigint]] to its decimal string representation."
   if(is_bigint(b) == false){ return "0" }
   mut sign = _big_sign(b)
   def digits = _big_digits(b)
   mut n = core.len(digits)
   if(sign == 0 || n == 0){ return "0" }
   mut out = ""
   mut i = n - 1
   out = to_str(get(digits, i))
   i -= 1
   while(i >= 0){
      def part = to_str(get(digits, i))
      mut pad = 9 - str_len(part)
      while(pad > 0){
         out = f"{out}0"
         pad -= 1
      }
      out = f"{out}{part}"
      i -= 1
   }
   if(sign < 0){ out = f"-{out}" }
   out
}

fn _big_add_abs(a, b){
   "Internal: add |a| + |b|."
   mut da = _big_digits(a)
   def db = _big_digits(b)
   mut na = core.len(da)
   mut nb = core.len(db)
   mut n = na
   if(nb > n){ n = nb }
   mut out = list(n + 1)
   mut carry = 0
   mut i = 0
   while(i < n || carry > 0){
      mut va = 0
      mut vb = 0
      if(i < na){ va = get(da, i) }
      if(i < nb){ vb = get(db, i) }
      mut sum = va + vb + carry
      if(sum >= 1000000000){
         sum -= 1000000000
         carry = 1
      } else {
         carry = 0
      }
       out = append(out, sum)
      i += 1
   }
   if(carry){  out = append(out, carry) }
   _big_make(1, out)
}

fn _big_sub_abs(a, b){
   "Internal: compute |a| - |b| where |a| >= |b|."
   mut da = _big_digits(a)
   def db = _big_digits(b)
   mut na = core.len(da)
   mut nb = core.len(db)
   mut out = list(na)
   mut borrow = 0
   mut i = 0
   while(i < na){
      mut va = get(da, i) - borrow
      mut vb = 0
      if(i < nb){ vb = get(db, i) }
      if(va < vb){
         va += 1000000000
         borrow = 1
      } else {
         borrow = 0
      }
       out = append(out, va - vb)
      i += 1
   }
   _big_make(1, out)
}

fn bigint_add(a, b){
   "Adds two bigints together."
   if(is_bigint(a) == false){ a = bigint(a) }
   if(is_bigint(b) == false){ b = bigint(b) }
   mut sa = _big_sign(a)
   mut sb = _big_sign(b)
   if(sa == 0){ return b }
   if(sb == 0){ return a }
   if(sa == sb){
      def res = _big_add_abs(a, b)
      return _big_make(sa, list_clone(_big_digits(res)))
   }
   mut cmp = _big_abs_cmp(a, b)
   if(cmp == 0){ return _big_make(0, list(0)) }
   if(cmp > 0){
      def res = _big_sub_abs(a, b)
      return _big_make(sa, list_clone(_big_digits(res)))
   }
   def res = _big_sub_abs(b, a)
   _big_make(sb, list_clone(_big_digits(res)))
}

fn bigint_sub(a, b){
   "Subtracts bigint `b` from bigint `a`."
   if(is_bigint(a) == false){ a = bigint(a) }
   if(is_bigint(b) == false){ b = bigint(b) }
   mut sb = _big_sign(b)
   def neg = _big_make(-sb, list_clone(_big_digits(b)))
   bigint_add(a, neg)
}

fn _big_mul_abs(a, b){
   "Internal: multiply |a| * |b|."
   mut da = _big_digits(a)
   def db = _big_digits(b)
   mut na = core.len(da)
   mut nb = core.len(db)
   if(na == 0 || nb == 0){ return _big_make(0, list(0)) }
   mut out = list(na + nb + 1)
   mut i = 0
   while(i < na + nb + 1){  out = append(out, 0)  i += 1 }
   i = 0
   while(i < na){
      mut carry = 0
      mut j = 0
      while(j < nb){
         mut idx = i + j
         def cur = get(out, idx)
         mut prod = cur + get(da, i) * get(db, j) + carry
         carry = prod / 1000000000
         prod %= 1000000000
         set_idx(out, idx, prod)
         j += 1
      }
      if(carry > 0){
         def idx2 = i + nb
         set_idx(out, idx2, get(out, idx2) + carry)
      }
      i += 1
   }
   _big_make(1, out)
}

fn bigint_mul(a, b){
   "Multiplies two bigints."
   if(is_bigint(a) == false){ a = bigint(a) }
   if(is_bigint(b) == false){ b = bigint(b) }
   mut sa = _big_sign(a)
   mut sb = _big_sign(b)
   if(sa == 0 || sb == 0){ return _big_make(0, list(0)) }
   def res = _big_mul_abs(a, b)
   _big_make(sa * sb, list_clone(_big_digits(res)))
}

fn _big_mul_small(a, m){
   "Internal: multiply bigint by small int m."
   if(m == 0){ return _big_make(0, list(0)) }
   mut da = _big_digits(a)
   def na = core.len(da)
   mut out = list(na + 1)
   mut carry = 0
   mut i = 0
   while(i < na){
      mut prod = get(da, i) * m + carry
      carry = prod / 1000000000
      prod %= 1000000000
       out = append(out, prod)
      i += 1
   }
   if(carry > 0){  out = append(out, carry) }
   _big_make(_big_sign(a), out)
}

fn _big_add_small(a, v){
   "Internal: add small int v to bigint."
   mut da = list_clone(_big_digits(a))
   mut i = 0
   mut carry = v
   while(carry > 0){
      if(i >= core.len(da)){  da = append(da, 0) }
      mut sum = get(da, i) + carry
      if(sum >= 1000000000){
         set_idx(da, i, sum - 1000000000)
         carry = 1
      } else {
         set_idx(da, i, sum)
         carry = 0
      }
      i += 1
   }
   mut s = _big_sign(a)
   if(core.len(da) > 0){ s = 1 }
   _big_make(s, da)
}

fn _digits_prepend(digits, v){
   "Internal: prepend v to digits list."
   mut out = list(core.len(digits) + 1)
    out = append(out, v)
   mut i = 0
   def n = core.len(digits)
   while(i < n){
       out = append(out, get(digits, i))
      i += 1
   }
   out
}

fn _big_divmod_abs(a, b){
   "Internal: divmod |a| / |b| -> [q, r]."
   if(_big_sign(b) == 0){ panic("bigint division by zero") }
   mut cmp = _big_abs_cmp(a, b)
   if(cmp < 0){ return [_big_make(0, list(0)), a] }
   if(cmp == 0){ return [_big_make(1, [1]), _big_make(0, list(0))] }
   def da = _big_digits(a)
   def n = core.len(da)
   mut qdigits = list(n)
   mut i = 0
   while(i < n){  qdigits = append(qdigits, 0)  i += 1 }
   mut r = _big_make(0, list(0))
   mut idx = n - 1
   while(idx >= 0){
      ; r = r * base + da[idx]
      mut rd = list_clone(_big_digits(r))
      rd = _digits_prepend(rd, get(da, idx))
      r = _big_make(_big_sign(r), rd)
      ; find q digit by binary search
      mut lo = 0  mut hi = 1000000000 - 1
      mut best = 0
      while(lo <= hi){
         mut mid = (lo + hi) / 2
         mut prod = _big_mul_small(b, mid)
         def c = _big_abs_cmp(prod, r)
         if(c <= 0){
            best = mid
            lo = mid + 1
         } else {
            hi = mid - 1
         }
      }
      if(best > 0){
         mut prod2 = _big_mul_small(b, best)
         r = _big_sub_abs(r, prod2)
      }
      set_idx(qdigits, idx, best)
      idx -= 1
   }
   mut q = _big_make(1, qdigits)
   return [q, r]
}

fn bigint_div(a, b){
   "Integer division of bigints."
   if(is_bigint(a) == false){ a = bigint(a) }
   if(is_bigint(b) == false){ b = bigint(b) }
   mut sa = _big_sign(a)
   mut sb = _big_sign(b)
   if(sb == 0){ panic("bigint division by zero") }
   if(sa == 0){ return _big_make(0, list(0)) }
   def res = _big_divmod_abs(a, b)
   def q = get(res, 0)
   _big_make(sa * sb, list_clone(_big_digits(q)))
}

fn bigint_mod(a, b){
   "Modulo of bigints."
   if(is_bigint(a) == false){ a = bigint(a) }
   if(is_bigint(b) == false){ b = bigint(b) }
   mut sb = _big_sign(b)
   if(sb == 0){ panic("bigint division by zero") }
   def res = _big_divmod_abs(a, b)
   def r = get(res, 1)
   _big_make(_big_sign(a), list_clone(_big_digits(r)))
}

fn bigint_cmp(a, b){
   "Compares two bigints. Returns -1 if a < b, 1 if a > b, and 0 if equal."
   if(is_bigint(a) == false){ a = bigint(a) }
   if(is_bigint(b) == false){ b = bigint(b) }
   mut sa = _big_sign(a)
   def sb = _big_sign(b)
   if(sa < sb){ return -1 }
   if(sa > sb){ return 1 }
   if(sa == 0){ return 0 }
   def c = _big_abs_cmp(a, b)
   c * sa
}

fn bigint_eq(a, b){
   "Returns **true** if bigints `a` and `b` are equal."
   bigint_cmp(a, b) == 0
}

