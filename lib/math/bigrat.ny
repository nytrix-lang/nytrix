;; Keywords: bigrat rational fraction
;; BigInt and BigRational arithmetic for cryptographic math.
;; Search aliases: math, bigrat, rational
;;
;; BigInt: unsigned integer stored as digit array, base 10^9, little-endian.
;;   Each element is one decimal "digit" (0 to 999999999).
;;   Index 0 is the least significant digit.
;;
;; BigRational: exact rational stored as [numerator, denominator, sign].
;;   numerator, denominator: BigInt digit arrays (always positive/unsigned).
;;   sign: integer 1 or -1.
;;   Invariant: denominator != 0, always in lowest terms after rat_simplify.
;;
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap14.pdf
;; - https://en.wikipedia.org/wiki/Arbitrary-precision_arithmetic
module std.math.crypto.bigrat(BIG_BASE, big_trim, big_copy, big_cmp, big_gte, big_gt, big_eq, big_sub, big_add, big_add_small, big_mul_small, big_mul, big_mod_digits, big_powmod, big_gcd, big_div_small, big_div, big_from_dec, big_to_dec, big_to_hex, hex_to_ascii, hex_val, int2s, rat_new, rat_zero, rat_one, rat_from_int, rat_from_int_den, rat_sign, rat_num, rat_den, rat_neg, rat_abs, rat_simplify, rat_add, rat_sub, rat_mul, rat_div, rat_cmp, rat_eq, rat_lt, rat_lte, rat_gt, rat_gte, rat_to_float, rat_floor, rat_round)
use std.core.str (Builder, builder_append, builder_to_str, builder_free, atof)

fn BIG_BASE(): int { "Returns the internal BigInt limb base." 1000000000 }

fn big_trim(list: lst): list {
   "Remove trailing zero digits from a BigInt digit array."
   mut n = lst.len
   while(n > 1 && lst.get(n - 1) == 0){
      lst = _big_rm_last(lst)
      n = lst.len
   }
   lst
}

fn _big_rm_last(list: lst): list {
   def n = lst.len
   if(n <= 1){ return [0] }
   mut result = []
   mut i = 0
   while(i < n - 1){
      result = result.append(lst.get(i))
      i += 1
   }
   result
}

fn big_copy(list: digits): list {
   "Return a copy of a BigInt digit array."
   def n = digits.len
   mut result = []
   mut i = 0
   while(i < n){
      result = result.append(digits.get(i))
      i += 1
   }
   result
}

fn big_cmp(list: a, list: b): int {
   "Compare two unsigned BigInt digit arrays. Returns -1, 0, or 1."
   def na, nb = a.len, b.len
   if(na > nb){ return 1 }
   if(na < nb){ return -1 }
   mut i = na - 1
   while(i >= 0){
      def ai, bi = a.get(i), b.get(i)
      if(ai > bi){ return 1 }
      if(ai < bi){ return -1 }
      i = i - 1
   }
   0
}

fn big_gte(list: a, list: b): bool { "Returns true when BigInt a >= b." big_cmp(a, b) >= 0 }

fn big_gt(list: a, list: b): bool { "Returns true when BigInt a > b." big_cmp(a, b) > 0 }

fn big_eq(list: a, list: b): bool { "Returns true when BigInt a == b." big_cmp(a, b) == 0 }

fn big_sub(list: a, list: b): list {
   "Subtract BigInt b from a. Assumes a >= b. Returns digit array."
   def na = a.len
   mut result = []
   mut borrow = 0
   def base = BIG_BASE()
   mut i = 0
   while(i < na){
      def ai, bi = a.get(i), (i < b.len) ? b.get(i) : 0
      def diff = ai - bi - borrow
      if(diff < 0){
         result = result.append(diff + base)
         borrow = 1
      } else {
         result = result.append(diff)
         borrow = 0
      }
      i += 1
   }
   big_trim(result)
}

fn big_add(list: a, list: b): list {
   "Add two BigInt digit arrays. Returns digit array."
   def na, nb = a.len, b.len
   def nmax = (na > nb) ? na : nb
   mut result = []
   mut carry = 0
   def base = BIG_BASE()
   mut i = 0
   while(i < nmax || carry > 0){
      def ai, bi = (i < na) ? a.get(i) : 0, (i < nb) ? b.get(i) : 0
      def sum = ai + bi + carry
      carry = sum / base
      result = result.append(sum % base)
      i += 1
   }
   big_trim(result)
}

fn big_mul_small(list: digits, int: n): list {
   "Multiply a BigInt digit array by a small non-negative integer."
   def len_d = digits.len
   if(n == 0){ return [0] }
   if(n == 1){ return big_copy(digits) }
   mut result = []
   mut carry = 0
   def base = BIG_BASE()
   mut i = 0
   while(i < len_d){
      def prod = digits.get(i) * n + carry
      carry = prod / base
      result = result.append(prod % base)
      i += 1
   }
   while(carry > 0){
      result = result.append(carry % base)
      carry = carry / base
   }
   big_trim(result)
}

fn big_add_small(list: digits, int: n): list {
   "Add a small non-negative integer to a BigInt digit array."
   if(n == 0){ return big_copy(digits) }
   def len_d = digits.len
   mut result = []
   mut carry = n
   def base = BIG_BASE()
   mut i = 0
   while(i < len_d){
      def sum = digits.get(i) + carry
      carry = sum / base
      result = result.append(sum % base)
      i += 1
   }
   while(carry > 0){
      result = result.append(carry % base)
      carry = carry / base
   }
   result
}

fn big_mul(list: a, list: b): list {
   "Multiply two BigInt digit arrays using schoolbook multiplication."
   def na, nb = a.len, b.len
   def nr = na + nb
   mut result = []
   mut i = 0
   while(i < nr){
      result = result.append(0)
      i += 1
   }
   def base = BIG_BASE()
   i = 0
   while(i < na){
      mut carry = 0
      def ai = a.get(i)
      mut j = 0
      while(j < nb){
         def idx = i + j
         def old = result.get(idx)
         def prod = ai * b.get(j) + old + carry
         carry = prod / base
         result[idx] = prod % base
         j += 1
      }
      def idx2 = i + nb
      if(idx2 < result.len){
         def old2 = result.get(idx2)
         result[idx2] = old2 + carry
      }
      i += 1
   }
   big_trim(result)
}

fn big_mod_digits(list: a, list: n): list {
   "Compute a mod n for two BigInt digit arrays."
   def na, nn = a.len, n.len
   if(na == 0){ return [0] }
   if(na < nn){ return big_copy(a) }
   if(na == nn){
      def cmp = big_cmp(a, n)
      if(cmp < 0){ return big_copy(a) }
      if(cmp == 0){ return [0] }
      return big_sub(a, n)
   }
   mut rem = [0]
   mut i = na - 1
   while(i >= 0){
      def d = a.get(i)
      rem = big_mul_small(rem, BIG_BASE())
      rem = big_add_small(rem, d)
      rem = big_trim(rem)
      while(big_gte(rem, n)){ rem = big_sub(rem, n) }
      i = i - 1
   }
   big_trim(rem)
}

fn big_powmod(list: base_digits, list: exp_digits, list: mod_digits): list {
   "Compute base^exp mod m for BigInt digit arrays using square-and-multiply."
   def zero = [0]
   def one = [1]
   def exp_is_zero = (exp_digits.len == 1 && exp_digits.get(0) == 0)
   if(exp_is_zero){ return big_copy(one) }
   def base_is_zero = (base_digits.len == 1 && base_digits.get(0) == 0)
   if(base_is_zero){ return big_copy(zero) }
   mut b, e = big_mod_digits(base_digits, mod_digits), big_copy(exp_digits)
   mut result = big_copy(one)
   while(e.len > 1 || e.get(0) > 0){
      def e0 = e.get(0)
      if(e0 & 1 != 0){
         def prod = big_mul(result, b)
         result = big_mod_digits(prod, mod_digits)
      }
      def sq = big_mul(b, b)
      b = big_mod_digits(sq, mod_digits)
      def qr = big_div_small(e, 2)
      e = qr.get(0)
   }
   result
}

fn big_gcd(list: a, list: b): list {
   "Compute gcd of two BigInt digit arrays using the Euclidean algorithm."
   mut aa = big_copy(a)
   mut bb = big_copy(b)
   while(!(bb.len == 1 && bb.get(0) == 0)){
      def r = big_mod_digits(aa, bb)
      aa = bb
      bb = r
   }
   big_trim(aa)
}

fn big_div_small(list: digits, int: d): list {
   "Divide a BigInt digit array by a small integer. Returns [quotient, remainder]."
   def n = digits.len
   if(n == 0){ return [[0], 0] }
   def base = BIG_BASE()
   mut result = []
   mut i = 0
   while(i < n){
      result = result.append(0)
      i += 1
   }
   mut rem = 0
   i = n - 1
   while(i >= 0){
      def cur = rem * base + digits.get(i)
      def q = cur / d
      rem = cur % d
      result[i] = q
      i = i - 1
   }
   [big_trim(result), rem]
}

fn big_div(list: a, list: b): list {
   "Divide BigInt a by b. Returns [quotient, remainder] as digit arrays."
   def cmp = big_cmp(a, b)
   if(cmp < 0){ return [[0], big_copy(a)] }
   if(cmp == 0){ return [[1], [0]] }
   mut rem = [0]
   def na = a.len
   mut quot = []
   mut i = na - 1
   while(i >= 0){
      rem = big_mul_small(rem, BIG_BASE())
      rem = big_add_small(rem, a.get(i))
      rem = big_trim(rem)
      mut lo = 0
      mut hi = BIG_BASE() - 1
      while(lo < hi){
         def mid = (lo + hi + 1) / 2
         def trial = big_mul_small(b, mid)
         if(big_gte(rem, trial)){ lo = mid } else { hi = mid - 1 }
      }
      quot = quot.append(lo)
      if(lo > 0){ rem = big_sub(rem, big_mul_small(b, lo)) }
      i = i - 1
   }
   def nq = quot.len
   mut quot_le = []
   mut j = nq - 1
   while(j >= 0){
      quot_le = quot_le.append(quot.get(j))
      j = j - 1
   }
   [big_trim(quot_le), big_trim(rem)]
}

fn big_from_dec(str: s): list {
   "Parse a decimal string into a BigInt digit array."
   def n = s.len
   if(n == 0){ return [0] }
   mut result = [0]
   mut pos = 0
   while(pos < n){
      def remaining = n - pos
      def take = (remaining < 9) ? remaining : 9
      mut chunk_val = 0
      mut ci = 0
      while(ci < take){
         def c = load8(s, pos + ci)
         chunk_val = chunk_val * 10 + (c - 48)
         ci += 1
      }
      mut p10 = 1
      mut k = 0
      while(k < take){
         p10 = p10 * 10
         k += 1
      }
      result = big_mul_small(result, p10)
      result = big_add_small(result, chunk_val)
      result = big_trim(result)
      pos = pos + take
   }
   result
}

fn big_to_dec(list: digits): str {
   "Convert a BigInt digit array to a decimal string."
   def n = digits.len
   if(n == 0){ return "0" }
   mut s, i = Builder(max(16, n * 9 + 8)), n - 1
   while(i >= 0){
      def val = digits.get(i)
      def chunk = int2s(val)
      if(i < n - 1){
         mut pad = 9 - chunk.len
         while(pad > 0){
            s = builder_append(s, "0")
            pad = pad - 1
         }
      }
      s, i = builder_append(s, chunk), i - 1
   }
   def out = builder_to_str(s)
   builder_free(s)
   (out.len == 0) ? "0" : out
}

fn big_to_hex(list: digits): str {
   "Convert a BigInt digit array to a lowercase hexadecimal string."
   def n = digits.len
   if(n == 0){ return "0" }
   if(n == 1 && digits.get(0) == 0){ return "0" }
   def hex_s = "0123456789abcdef"
   mut working = big_copy(digits)
   mut hex_chars = []
   while(working.len > 1 || working.get(0) != 0){
      def qr = big_div_small(working, 16)
      working = qr.get(0)
      def rem_val = qr.get(1)
      hex_chars = hex_chars.append(rem_val)
   }
   def nhex = hex_chars.len
   if(nhex == 0){ return "0" }
   mut result = Builder(max(16, nhex + 8))
   mut i = nhex - 1
   while(i >= 0){
      def h, c = hex_chars.get(i), hex_s.get(h)
      result = builder_append(result, chr(load8(c, 0)))
      i = i - 1
   }
   def out = builder_to_str(result)
   builder_free(result)
   out
}

fn hex_to_ascii(str: hex_str): str {
   "Decode a hex string to an ASCII string."
   def n = hex_str.len
   mut result = Builder(max(16, (n / 2) + 8))
   mut i = 0
   while(i + 1 < n){
      def h1, h2 = load8(hex_str, i), load8(hex_str, i + 1)
      def bv = hex_val(h1) * 16 + hex_val(h2)
      if(bv > 0){ result = builder_append(result, chr(bv)) }
      i = i + 2
   }
   def out = builder_to_str(result)
   builder_free(result)
   out
}

fn hex_val(int: c): int {
   "Return the numeric value of a hex digit byte(0-9, a-f, A-F)."
   case c {
      48..57 -> c - 48
      97..102 -> c - 87
      65..70 -> c - 55
      _ -> 0
   }
}

fn int2s(int: n): str {
   "Convert a non-negative integer to its decimal string."
   if(n == 0){ return "0" }
   mut val = n
   mut digs = []
   while(val > 0){
      digs = digs.append(val % 10 + 48)
      val = val / 10
   }
   def nd = digs.len
   mut result = Builder(max(16, nd + 8))
   mut i = nd - 1
   while(i >= 0){
      result = builder_append(result, chr(digs.get(i)))
      i = i - 1
   }
   def out = builder_to_str(result)
   builder_free(result)
   out
}

fn rat_new(list: num, list: den, int: sign): list {
   "Create a BigRational from unsigned digit arrays num, den, and sign(1 or -1).
   den must not be zero. Does not simplify automatically — call rat_simplify if needed."
   [num, den, sign]
}

fn rat_zero(): list {
   "Return the BigRational zero(0/1)."
   [[0], [1], 1]
}

fn rat_one(): list {
   "Return the BigRational one(1/1)."
   [[1], [1], 1]
}

fn rat_from_int(int: n): list {
   "Construct a BigRational from a plain integer(may be negative)."
   if(n == 0){ return rat_zero() }
   def sign = (n < 0) ? -1 : 1
   def abs_n = (n < 0) ? (0 - n) : n
   [[abs_n], [1], sign]
}

fn rat_from_int_den(int: num_int, int: den_int): list {
   "Construct a BigRational from two plain integers num/den(signs handled)."
   if(num_int == 0){ return rat_zero() }
   def sign = ((num_int < 0) != (den_int < 0)) ? -1 : 1
   def abs_n = (num_int < 0) ? (0 - num_int) : num_int
   def abs_d = (den_int < 0) ? (0 - den_int) : den_int
   rat_simplify([[abs_n], [abs_d], sign])
}

fn rat_sign(list: r): int { r.get(2) }

fn rat_num(list: r): list { r.get(0) }

fn rat_den(list: r): list { r.get(1) }

fn _rat_num_is_zero(list: r): bool {
   def n = rat_num(r)
   n.len == 1 && n.get(0) == 0
}

fn rat_neg(list: r): list {
   "Negate a BigRational."
   if(_rat_num_is_zero(r)){ return r }
   [rat_num(r), rat_den(r), 0 - rat_sign(r)]
}

fn rat_abs(list: r): list {
   "Absolute value of a BigRational."
   [rat_num(r), rat_den(r), 1]
}

fn rat_simplify(list: r): list {
   "Reduce a BigRational to lowest terms by dividing by gcd(num, den)."
   def num = rat_num(r)
   def den = rat_den(r)
   def sign = rat_sign(r)
   def is_zero = (num.len == 1 && num.get(0) == 0)
   if(is_zero){ return rat_zero() }
   def g = big_gcd(num, den)
   if(big_eq(g, [1])){ return r }
   def new_num = big_div(num, g).get(0)
   def new_den = big_div(den, g).get(0)
   [new_num, new_den, sign]
}

fn rat_add(list: a, list: b): list {
   "Add two BigRationals. Returns simplified result."
   def an, ad = rat_num(a), rat_den(a)
   def as_ = rat_sign(a)
   def bn = rat_num(b)
   def bd = rat_den(b)
   def bs = rat_sign(b)
   def new_den = big_mul(ad, bd)
   def term_a = big_mul(an, bd)
   def term_b = big_mul(bn, ad)
   mut new_num_val = [0]
   mut new_sign = 1
   if(as_ == bs){
      new_num_val = big_add(term_a, term_b)
      new_sign = as_
   } else {
      def cmp = big_cmp(term_a, term_b)
      if(cmp == 0){ return rat_zero() } elif(cmp > 0){
         new_num_val = big_sub(term_a, term_b)
         new_sign = as_
      } else {
         new_num_val = big_sub(term_b, term_a)
         new_sign = bs
      }
   }
   rat_simplify([new_num_val, new_den, new_sign])
}

fn rat_sub(list: a, list: b): list {
   "Subtract BigRational b from a. Returns simplified result."
   rat_add(a, rat_neg(b))
}

fn rat_mul(list: a, list: b): list {
   "Multiply two BigRationals. Returns simplified result."
   def new_num = big_mul(rat_num(a), rat_num(b))
   def new_den = big_mul(rat_den(a), rat_den(b))
   def new_sign = rat_sign(a) * rat_sign(b)
   rat_simplify([new_num, new_den, new_sign])
}

fn rat_div(list: a, list: b): list {
   "Divide BigRational a by b. Returns simplified result. b must not be zero."
   def new_num = big_mul(rat_num(a), rat_den(b))
   def new_den = big_mul(rat_den(a), rat_num(b))
   def new_sign = rat_sign(a) * rat_sign(b)
   rat_simplify([new_num, new_den, new_sign])
}

fn rat_cmp(list: a, list: b): int {
   "Compare two BigRationals. Returns -1, 0, or 1."
   def diff = rat_sub(a, b)
   if(_rat_num_is_zero(diff)){ return 0 }
   (rat_sign(diff) > 0) ? 1 : -1
}

fn rat_eq(list: a, list: b): bool { "Returns true when two BigRationals are equal." rat_cmp(a, b) == 0 }

fn rat_lt(list: a, list: b): bool { "Returns true when a < b for BigRationals." rat_cmp(a, b) < 0 }

fn rat_lte(list: a, list: b): bool { "Returns true when a <= b for BigRationals." rat_cmp(a, b) <= 0 }

fn rat_gt(list: a, list: b): bool { "Returns true when a > b for BigRationals." rat_cmp(a, b) > 0 }

fn rat_gte(list: a, list: b): bool { "Returns true when a >= b for BigRationals." rat_cmp(a, b) >= 0 }

fn rat_to_float(list: r): f64 {
   "Convert a BigRational to a native float(approximate).
   Converts both num and den to decimal strings, parses as floats, divides."
   if(_rat_num_is_zero(r)){ return 0.0 }
   def n_str, d_str = big_to_dec(rat_num(r)), big_to_dec(rat_den(r))
   def n_f, d_f = atof(n_str), atof(d_str)
   def result = n_f / d_f
   (rat_sign(r) < 0) ? (0.0 - result) : result
}

fn rat_floor(list: r): int {
   "Return the floor of a BigRational as a plain integer."
   if(_rat_num_is_zero(r)){ return 0 }
   def qr = big_div(rat_num(r), rat_den(r))
   def q_digits = qr.get(0)
   def rem_digits = qr.get(1)
   def has_rem = !(rem_digits.len == 1 && rem_digits.get(0) == 0)
   mut q = 0
   def nq = q_digits.len
   mut base = 1
   mut i = 0
   while(i < nq){
      q = q + q_digits.get(i) * base
      base = base * BIG_BASE()
      i += 1
   }
   if(rat_sign(r) < 0 && has_rem){ return 0 - q - 1 }
   if(rat_sign(r) < 0){ return 0 - q }
   q
}

fn rat_round(list: r): int {
   "Round a BigRational to the nearest integer(half away from zero)."
   def half = rat_from_int_den(1, 2)
   def shifted = rat_add(rat_abs(r), half)
   def fl = rat_floor(shifted)
   (rat_sign(r) < 0) ? (0 - fl) : fl
}
