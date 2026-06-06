;; Keywords: prng mt19937 math crypto
;; PRNG analysis routines for MT19937 tempering, cloning, and state recovery.
;; Full state recovery from 624 consecutive 32-bit outputs via untemper.
;; Reference:
;; - https://www.math.sci.hiroshima-u.ac.jp/m-mat/MT/ARTICLES/mt.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap5.pdf
;; References:
;; - std.math.crypto.prng
;; - std.math.crypto
module std.math.crypto.prng.mt19937(mt_untemper, mt_temper, mt_clone, mt_predict, mt_generate, mt19937_next, mt_twist_output_candidates, mt19937_smt_recover_state_prefix, py_mt19937_seed_int, py_mt19937_getrandbits, py_getrandbits_unpack_words_full, py_getrandbits_word_constraints, py_random_float_to_mt_parts, py_mt19937_randbytes, py_mt19937_randbelow, py_mt19937_randrange, py_mt19937_sample_range)
use std.core
use std.math.nt
use std.math.scalar (pow)
use std.math.smt

fn _untemper_right(any y, int shift) any {
   mut r, i = y & 0xFFFFFFFF, 0
   while(i < 6){
      r = (y ^^ (r >> shift)) & 0xFFFFFFFF
      i += 1
   }
   r & 0xFFFFFFFF
}

fn _untemper_left(any y, int shift, any mask) any {
   mut r, i = y & 0xFFFFFFFF, 0
   while(i < 6){
      r = (y ^^ ((r << shift) & mask)) & 0xFFFFFFFF
      i += 1
   }
   r & 0xFFFFFFFF
}

fn mt_untemper(any y) any {
   "Reverse all MT19937 tempering operations to recover the internal state word from output y.
   The tempering operations in order: y^=(y>>11), y^=(y<<7)&0x9D2C5680,
   y^=(y<<15)&0xEFC60000, y^=(y>>18). Returns the 32-bit state word."
   mut v = y & 0xFFFFFFFF
   v = _untemper_right(v, 18)
   v = _untemper_left(v, 15, 0xEFC60000)
   v = _untemper_left(v, 7, 0x9D2C5680)
   v = _untemper_right(v, 11)
   v & 0xFFFFFFFF
}

fn mt_temper(any s) any {
   "Apply MT19937 tempering to internal state word s. Returns the output value."
   mut y = s & 0xFFFFFFFF
   y = y ^^ (y >> 11)
   y = y ^^ ((y << 7) & 0x9D2C5680)
   y = y ^^ ((y << 15) & 0xEFC60000)
   y = y ^^ (y >> 18)
   y & 0xFFFFFFFF
}

fn mt_twist_output_candidates(any output_i1, any output_i397) list {
   "Recover the two possible twisted MT19937 outputs at index i from pre-twist
   outputs at state indices i+1 and i+397. The top bit of state[i] is unknown,
   producing two candidates."
   def mt_i1 = mt_untemper(output_i1)
   def mt_i397 = mt_untemper(output_i397)
   mut out = list(2)
   store64(out, 2, 0)
   mut msb = 0
   while(msb < 2){
      def y = (msb * 0x80000000) | (mt_i1 & 0x7FFFFFFF)
      mut word = mt_i397 ^^ (y >> 1)
      if((y & 1) != 0){ word = word ^^ 0x9908B0DF }
      __store_item_fast(out, msb, mt_temper(word))
      msb += 1
   }
   out
}

fn mt_clone(list outputs_624) list {
   "Recover MT19937 internal state array from 624 consecutive 32-bit outputs.
   Returns the state list(624 elements), ready for use with mt_predict."
   def n = outputs_624.len
   mut state = list(n)
   store64(state, n, 0)
   mut i = 0
   while(i < n){
      __store_item_fast(state, i, mt_untemper(__load_item_fast(outputs_624, i)))
      i += 1
   }
   state
}

fn mt_generate(list state) list {
   "Perform one MT19937 twist on state(624 words), returning the updated state.
   This generates the next 624 outputs."
   mut s, i = clone(state), 0
   while(i < 624){
      def hi = __load_item_fast(s, i) & 0x80000000
      def lo = __load_item_fast(s, (i + 1) % 624) & 0x7FFFFFFF
      def y = hi | lo
      mut val = __load_item_fast(s, (i + 397) % 624) ^^ (y >> 1)
      if(y & 1 != 0){ val = val ^^ 0x9908B0DF }
      __store_item_fast(s, i, val)
      i += 1
   }
   s
}

fn mt_predict(list state, int count) list {
   "Predict the next count MT19937 outputs from a cloned state(624 words).
   Returns list of predicted 32-bit values."
   def n = state.len
   if(n < 624){ return [] }
   mut results = list(count)
   store64(results, count, 0)
   mut s = state
   mut generated = 0
   mut idx = 0
   while(generated < count){
      if(idx >= 624){
         s = mt_generate(s)
         idx = 0
      }
      __store_item_fast(results, generated, mt_temper(__load_item_fast(s, idx)))
      idx += 1
      generated += 1
   }
   results
}

fn mt19937_next(list state_orig, int idx_in) list {
   "Generate the next MT19937 output from the recovered state at given index.
   Returns [output_value, updated_state, new_index]."
   mut state = clone(state_orig)
   mut idx = int(idx_in)
   if(idx >= 624){
      state = mt_generate(state)
      idx = 0
   }
   def out = mt_temper(__load_item_fast(state, idx))
   def new_index = idx + 1
   [out, state, new_index]
}

fn _mt_smt_bv32(any ctx, str name) any { z3_bv_const(ctx, name, 32) }

fn _mt_smt_bv32u(any ctx, any v) any { z3_bv_u64(ctx, v & 0xFFFFFFFF, 32) }

fn _mt_smt_shr(any ctx, any a, int s) any { z3_bvlshr(ctx, a, z3_bv_u64(ctx, s, 32)) }

fn _mt_smt_shl(any ctx, any a, int s) any { z3_bvshl(ctx, a, z3_bv_u64(ctx, s, 32)) }

fn _mt_smt_temper(any ctx, any x) any {
   mut y = x
   y = z3_bvxor(ctx, y, _mt_smt_shr(ctx, y, 11))
   y = z3_bvxor(ctx, y, z3_bvand(ctx, _mt_smt_shl(ctx, y, 7), _mt_smt_bv32u(ctx, 0x9D2C5680)))
   y = z3_bvxor(ctx, y, z3_bvand(ctx, _mt_smt_shl(ctx, y, 15), _mt_smt_bv32u(ctx, 0xEFC60000)))
   y = z3_bvxor(ctx, y, _mt_smt_shr(ctx, y, 18))
   y
}

fn mt19937_smt_recover_state_prefix(any constraints) dict {
   "Recover a prefix of MT19937 internal state words from constraints on tempered outputs.
   constraints: list of dicts, each containing:
   - index: output index in the modeled stream
   - kind: \"eq\" for full u32 equality, or \"hi\" for top-bit constraints
   - value: expected value(for kind=hi, this is already shifted: out >> (32-bits))
   - bits: number of top bits for kind=hi
   Returns dict:
   - sat: bool
   - model_words: list of u32 state words(length max_index+1) when sat."
   if(constraints == nil || constraints.len == 0){ return dict().set("sat", false) }
   if(!z3_available()){ return dict().set("sat", false) }
   def ctx = z3_ctx_new()
   if(!ctx){ return dict().set("sat", false) }
   def s = z3_solver_new(ctx)
   if(!s){
      z3_ctx_del(ctx)
      return dict().set("sat", false)
   }
   mut max_idx = 0
   mut i = 0
   while(i < constraints.len){
      def c = constraints.get(i)
      def idx = int(c.get("index", 0))
      if(idx > max_idx){ max_idx = idx }
      i += 1
   }
   def nwords = max_idx + 1
   mut words = []
   i = 0
   while(i < nwords){
      words = words.append(_mt_smt_bv32(ctx, "s" + to_str(i)))
      i += 1
   }
   i = 0
   while(i < constraints.len){
      def c = constraints.get(i)
      def idx = int(c.get("index", 0))
      def kind = c.get("kind", "")
      def v = int(c.get("value", 0))
      def out = _mt_smt_temper(ctx, words.get(idx))
      if(kind == "eq"){ z3_solver_assert(ctx, s, z3_eq(ctx, out, _mt_smt_bv32u(ctx, v))) } elif(kind == "hi"){
         def bits = int(c.get("bits", 0))
         def sh = 32 - bits
         def out_hi = _mt_smt_shr(ctx, out, sh)
         z3_solver_assert(ctx, s, z3_eq(ctx, out_hi, z3_bv_u64(ctx, v, 32)))
      }
      i += 1
   }
   def sat = z3_solver_check(ctx, s)
   mut result = dict()
   result = result.set("sat", sat)
   if(!sat){
      z3_solver_del(ctx, s)
      z3_ctx_del(ctx)
      return result
   }
   mut model_words = []
   i = 0
   while(i < nwords){
      def mv = z3_model_eval_u64(ctx, s, words.get(i))
      model_words = model_words.append((mv == nil) ? 0 : (int(mv) & 0xFFFFFFFF))
      i += 1
   }
   result = result.set("model_words", model_words)
   z3_solver_del(ctx, s)
   z3_ctx_del(ctx)
   result
}

fn _mt_init_genrand(any seed32) list {
   mut state = []
   state = state.append(seed32 & 0xFFFFFFFF)
   mut i = 1
   while(i < 624){
      def prev = state.get(i - 1)
      def t = prev ^^ (prev >> 30)
      state = state.append((1812433253 * t + i) & 0xFFFFFFFF)
      i += 1
   }
   state
}

fn _seed_int_to_key32(any seed_int) list {
   mut n = seed_int
   if(n < 0){ n = -n }
   if(n == 0){ return [0] }
   mut keys = []
   while(n > 0){
      keys = keys.append(n & 0xFFFFFFFF)
      n = n >> 32
   }
   keys
}

fn _mt_init_by_array(list keys) list {
   def key_len = keys.len
   mut mt = _mt_init_genrand(19650218)
   mut i = 1
   mut j = 0
   mut k = 624
   if(key_len > 624){ k = key_len }
   while(k > 0){
      def prev = mt.get(i - 1)
      def t = prev ^^ (prev >> 30)
      def cur = mt.get(i)
      mt[i] = ((cur ^^ ((t * 1664525) & 0xFFFFFFFF)) + (keys.get(j) & 0xFFFFFFFF) + j) & 0xFFFFFFFF
      i += 1
      j += 1
      if(i >= 624){
         mt[0] = mt.get(623)
         i = 1
      }
      if(j >= key_len){ j = 0 }
      k -= 1
   }
   k = 623
   while(k > 0){
      def prev = mt.get(i - 1)
      def t = prev ^^ (prev >> 30)
      def cur = mt.get(i)
      mt[i] = ((cur ^^ ((t * 1566083941) & 0xFFFFFFFF)) - i) & 0xFFFFFFFF
      i += 1
      if(i >= 624){
         mt[0] = mt.get(623)
         i = 1
      }
      k -= 1
   }
   mt[0] = 0x80000000
   mt
}

fn py_mt19937_seed_int(any seed_int) list {
   "Returns [state, index] for Python-compatible MT19937 seeded with integer seed_int."
   def keys = _seed_int_to_key32(seed_int)
   def st = _mt_init_by_array(keys)
   [st, 624]
}

fn py_mt19937_getrandbits(list state_orig, int index, int k) list {
   "Python-compatible getrandbits(k). Returns [value, updated_state, new_index]."
   if(k <= 0){ return [0, state_orig, index] }
   def words = (k + 31) / 32
   mut state = state_orig
   mut idx = index
   mut out = Z(0)
   mut i = 0
   mut kk = k
   while(i < words){
      def r = mt19937_next(state, idx)
      mut w = r.get(0) & 0xFFFFFFFF
      state, idx = r.get(1), r.get(2)
      if(kk < 32){
         w = w >> (32 - kk)
      }
      out = out + (Z(w) << (32 * i))
      kk = kk - 32
      i += 1
   }
   [out, state, idx]
}

fn py_mt19937_randbelow(list state_in, int idx_in, int n) list {
   "Python Random._randbelow(n). Returns [value, updated_state, new_index]."
   if(n <= 0){ return [0, state_in, idx_in] }
   def k = bit_length(Z(n))
   mut state = state_in
   mut idx = idx_in
   mut r = Z(n)
   while(r >= Z(n)){
      def got = py_mt19937_getrandbits(state, idx, k)
      r = Z(got[0])
      state, idx = got[1], got[2]
   }
   [int(r), state, idx]
}

fn py_mt19937_randrange(list state_in, int idx_in, int start, int stop) list {
   "Python Random.randrange(start, stop) for positive unit step. Returns [value, updated_state, new_index]."
   def width = stop - start
   def got = py_mt19937_randbelow(state_in, idx_in, width)
   [start + int(got[0]), got[1], got[2]]
}

fn py_mt19937_sample_range(list state_in, int idx_in, int n, int k) list {
   "Python Random.sample(range(n), k) index sequence for n <= CPython's pool threshold.
   Returns [indices, updated_state, new_index]."
   if(k < 0 || k > n){ return [[], state_in, idx_in] }
   mut pool = []
   mut i = 0
   while(i < n){
      pool = pool.append(i)
      i += 1
   }
   mut out = []
   mut state = state_in
   mut idx = idx_in
   i = 0
   while(i < k){
      def got = py_mt19937_randbelow(state, idx, n - i)
      def j = int(got[0])
      state, idx = got[1], got[2]
      out = out.append(pool[j])
      pool[j] = pool[n - i - 1]
      i += 1
   }
   [out, state, idx]
}

fn py_getrandbits_unpack_words_full(any value, int k) list {
   "Invert Python getrandbits(k) packing, omitting the final partial word."
   if(k <= 0){ return [] }
   def words = (k + 31) / 32
   def rem = k % 32
   def acc = Z(value)
   mut out = []
   mut i = 0
   while(i < words){
      def w = (acc >> Z(32 * i)) & Z(0xFFFFFFFF)
      if(!(rem != 0 && i == words - 1)){ out = out.append(w) }
      i += 1
   }
   out
}

fn _py_mt_constraint(int index, str kind, any value, int bits) dict {
   {"index": index, "kind": kind, "value": value, "bits": bits}
}

fn py_getrandbits_word_constraints(any value, int k, int start_index=0) list {
   "Convert Python getrandbits(k) output into MT output constraints."
   if(k <= 0){ return [] }
   def words = (k + 31) / 32
   def rem = k % 32
   def acc = Z(value)
   mut out = []
   mut i = 0
   while(i < words){
      def shift = Z(32 * i)
      if(rem != 0 && i == words - 1){
         def mask = (Z(1) << Z(rem)) - Z(1)
         out = out.append(_py_mt_constraint(start_index + i, "hi", (acc >> shift) & mask, rem))
      } else {
         out = out.append(_py_mt_constraint(start_index + i, "eq", (acc >> shift) & Z(0xFFFFFFFF), 32))
      }
      i += 1
   }
   out
}

fn py_random_float_to_mt_parts(f64 f) list {
   "Convert Python random.random() into the two high-bit MT19937 output parts."
   def scaled = int(f * pow(2.0, 53.0))
   def a = scaled >> 26
   def b = scaled & ((1 << 26) - 1)
   [a, 27, b, 26]
}

fn _fixed_little_endian_bytes(any value, int nbytes) list {
   def be = bigint_to_bytes(value)
   mut out = list(nbytes)
   mut i = 0
   while(i < nbytes){
      def src = be.len - 1 - i
      out[i] = src >= 0 ? be[src] : 0
      i += 1
   }
   out
}

fn py_mt19937_randbytes(list state_in, int idx_in, int nbytes) list {
   "Reproduce CPython Random.randbytes(nbytes) from a cloned MT state."
   if(nbytes <= 0){ return [[], state_in, idx_in] }
   def r = py_mt19937_getrandbits(state_in, idx_in, nbytes * 8)
   [_fixed_little_endian_bytes(r[0], nbytes), r[1], r[2]]
}

#main {
   fn _mt_check(bool cond, str msg) int {
      if(!cond){ __panic(msg) }
      0
   }
   def sample = 2754794679
   _mt_check(mt_temper(mt_untemper(sample)) == sample, "mt19937 temper round-trip")
   def seed = Z(123456789)
   def seeded = py_mt19937_seed_int(seed)
   mut st = seeded.get(0)
   mut idx = seeded.get(1)
   def fresh1 = py_mt19937_getrandbits(st, idx, 1)
   def fresh5 = py_mt19937_getrandbits(st, idx, 5)
   def fresh32 = py_mt19937_getrandbits(st, idx, 32)
   def fresh33 = py_mt19937_getrandbits(st, idx, 33)
   _mt_check(fresh1.get(0) == Z(1) && fresh5.get(0) == Z(20) && fresh32.get(0) == Z(2754794679) && fresh33.get(0) == Z(2754794679), "mt19937 fresh getrandbits")
   def r0 = py_mt19937_getrandbits(st, idx, 0)
   _mt_check(r0.get(0) == Z(0) && r0.get(1) == st && r0.get(2) == idx, "mt19937 getrandbits zero")
   def r1 = py_mt19937_getrandbits(st, idx, 1)
   _mt_check(r1.get(0) == Z(1), "mt19937 getrandbits 1")
   st, idx = r1.get(1), r1.get(2)
   def r5 = py_mt19937_getrandbits(st, idx, 5)
   _mt_check(r5.get(0) == Z(14), "mt19937 getrandbits 5")
   st, idx = r5.get(1), r5.get(2)
   def r32 = py_mt19937_getrandbits(st, idx, 32)
   _mt_check(r32.get(0) == Z(2328685183), "mt19937 getrandbits 32")
   st, idx = r32.get(1), r32.get(2)
   def r33 = py_mt19937_getrandbits(st, idx, 33)
   _mt_check(r33.get(0) == Z(7344202699), "mt19937 getrandbits 33")
   def words = py_getrandbits_unpack_words_full(r33.get(0), 33)
   _mt_check(words.len == 1 && words.get(0) == (r33.get(0) & Z(0xFFFFFFFF)), "mt19937 unpack words")
   def constraints = py_getrandbits_word_constraints(r33.get(0), 33, 7)
   _mt_check(constraints.len == 2, "mt19937 constraints count")
   _mt_check(constraints.get(0).get("kind", "") == "eq" && constraints.get(0).get("index", 0) == 7, "mt19937 full constraint")
   _mt_check(constraints.get(1).get("kind", "") == "hi" && constraints.get(1).get("bits", 0) == 1, "mt19937 partial constraint")
   def neg = py_mt19937_seed_int(Z(0) - seed)
   def neg_first = py_mt19937_getrandbits(neg.get(0), neg.get(1), 32)
   def pos = py_mt19937_seed_int(seed)
   def pos_first = py_mt19937_getrandbits(pos.get(0), pos.get(1), 32)
   _mt_check(neg_first.get(0) == pos_first.get(0), "mt19937 negative seed")
   def rb8 = py_mt19937_randbytes(seeded.get(0), seeded.get(1), 8)
   _mt_check(rb8.get(0) == [183, 212, 50, 164, 124, 119, 56, 113], "mt19937 randbytes 8")
   def rb0 = py_mt19937_randbytes(seeded.get(0), seeded.get(1), 0)
   _mt_check(rb0.get(0) == [] && rb0.get(2) == seeded.get(1), "mt19937 randbytes zero")
   def below = py_mt19937_randbelow(seeded.get(0), seeded.get(1), 100)
   _mt_check(below.get(0) == 82, "mt19937 randbelow")
   def rr = py_mt19937_randrange(seeded.get(0), seeded.get(1), 10, 20)
   _mt_check(rr.get(0) == 17, "mt19937 randrange")
   def sample_range = py_mt19937_sample_range(seeded.get(0), seeded.get(1), 10, 4)
   _mt_check(sample_range.get(0) == [7, 8, 6, 2], "mt19937 sample range")
   _mt_check(py_random_float_to_mt_parts(0.6414006161858726) == [86087333, 27, 29680093, 26], "mt19937 float parts")
   print("✓ std.math.crypto.prng.mt19937 self-test passed")
}
