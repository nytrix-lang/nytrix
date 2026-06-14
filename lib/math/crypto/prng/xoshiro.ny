;; Keywords: prng xoshiro math crypto
;; PRNG analysis routines for xoshiro/xoroshiro stepping, jumps, and family hints.
;; Reference:
;; - https://prng.di.unimi.it/
;; These expose transitions and jump functions so analysis tools can model,
;; rewind by search/linear algebra, and identify output families.
;; References:
;; - std.math.crypto.prng
;; - std.math.crypto
module std.math.crypto.prng.xoshiro(xorshift32_next, xorshift64star_next, xoroshiro128plus_next, xoroshiro128plus_jump, xoshiro256plusplus_next, xoshiro256plusplus_jump, prng_output_family_hint)
use std.math.nt

def _X64_MOD = Z("18446744073709551616")
def _X32_MOD = Z("4294967296")

fn _u64(any x) any { mod(Z(x), _X64_MOD) }

fn _u32(any x) any { mod(Z(x), _X32_MOD) }

fn _rotl64(any x, any k) any {
   def kk = Z(k)
   _u64((Z(x) << kk) | (Z(x) >> (Z(64) - kk)))
}

fn xorshift32_next(any state) list {
   "Marsaglia-style xorshift32. Returns [next_state, output]."
   mut x = _u32(state)
   x = _u32(x ^^ _u32(x << Z(13)))
   x = _u32(x ^^ (x >> Z(17)))
   x = _u32(x ^^ _u32(x << Z(5)))
   [x, x]
}

fn xorshift64star_next(any state) list {
   "xorshift64* transition/output. Returns [next_state, output]."
   mut x = _u64(state)
   x = _u64(x ^^ (x >> Z(12)))
   x = _u64(x ^^ _u64(x << Z(25)))
   x = _u64(x ^^ (x >> Z(27)))
   [x, _u64(x * Z(2685821657736338717))]
}

fn xoroshiro128plus_next(list state) list {
   "xoroshiro128+ next. State is [s0, s1], returns [next_state, output]."
   mut s0, s1 = _u64(state.get(0)), _u64(state.get(1))
   def result = _u64(s0 + s1)
   s1 = _u64(s1 ^^ s0)
   def ns0, ns1 = _u64(_rotl64(s0, 55) ^^ s1 ^^ _u64(s1 << Z(14))), _rotl64(s1, 36)
   [[ns0, ns1], result]
}

fn xoroshiro128plus_jump(list state) list {
   "Equivalent to 2^64 calls to xoroshiro128plus_next; useful for independent streams."
   def jumps = [Z("13739361407582206667"), Z("15594563132006766882")]
   mut s0, s1 = Z(0), Z(0)
   mut st = state
   mut i = 0
   while i < jumps.len {
      mut b = 0
      while b < 64 {
         if (jumps.get(i) & (Z(1) << Z(b))) != Z(0) { s0, s1 = _u64(s0 ^^ st.get(0)), _u64(s1 ^^ st.get(1)) }
         st = xoroshiro128plus_next(st).get(0)
         b += 1
      }
      i += 1
   }
   [s0, s1]
}

fn xoshiro256plusplus_next(list state) list {
   "xoshiro256++ next. State is [s0,s1,s2,s3], returns [next_state, output]."
   mut s0, s1 = _u64(state.get(0)), _u64(state.get(1))
   mut s2, s3 = _u64(state.get(2)), _u64(state.get(3))
   def result = _u64(_rotl64(_u64(s0 + s3), 23) + s0)
   def t = _u64(s1 << Z(17))
   s2, s3 = _u64(s2 ^^ s0), _u64(s3 ^^ s1)
   s1, s0 = _u64(s1 ^^ s2), _u64(s0 ^^ s3)
   s2, s3 = _u64(s2 ^^ t), _rotl64(s3, 45)
   [[s0, s1, s2, s3], result]
}

fn xoshiro256plusplus_jump(list state) list {
   "Equivalent to 2^128 calls to xoshiro256plusplus_next."
   def jumps = [Z("1733541517147835066"), Z("15395012609548302636"), Z("12202545078643706282"), Z("4155657270789760540")]
   mut s0, s1 = Z(0), Z(0)
   mut s2, s3 = Z(0), Z(0)
   mut st = state
   mut i = 0
   while i < jumps.len {
      mut b = 0
      while b < 64 {
         if (jumps.get(i) & (Z(1) << Z(b))) != Z(0) {
            s0, s1 = _u64(s0 ^^ st.get(0)), _u64(s1 ^^ st.get(1))
            s2, s3 = _u64(s2 ^^ st.get(2)), _u64(s3 ^^ st.get(3))
         }
         st = xoshiro256plusplus_next(st).get(0)
         b += 1
      }
      i += 1
   }
   [s0, s1, s2, s3]
}

fn prng_output_family_hint(str name) str {
   "Return a compact attack hint for common xorshift-family output functions."
   case name {
      "raw" -> "linear over GF(2); recover with linear algebra or Berlekamp-Massey"
      "star", "*" -> "invert odd multiplication then solve linear state transition"
      "starstar", "**" -> "invert rotation and odd multiplication around linear core when full outputs are known"
      "plus", "+" -> "addition introduces carries; use SAT/SMT, branch-and-prune, or many-output search"
      "plusplus", "++" -> "rotated addition with carries; harder than raw/star, model bit carries explicitly"
      _ -> "unknown family"
   }
}
