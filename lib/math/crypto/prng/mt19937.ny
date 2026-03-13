;; Keywords: prng mt19937
;; PRNG analysis routines for MT19937 tempering, cloning, and state recovery.
;; Full state recovery from 624 consecutive 32-bit outputs via untemper.
;; Reference:
;; - https://www.math.sci.hiroshima-u.ac.jp/m-mat/MT/ARTICLES/mt.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap5.pdf
module std.math.crypto.prng.mt19937(mt_untemper, mt_temper, mt_clone, mt_predict, mt_generate, mt19937_next, mt_twist_output_candidates, mt19937_smt_recover_state_prefix)
use std.core
use std.math.nt
use std.math.smt

fn _untemper_right(any: y, int: shift): any {
   mut r, i = y & 0xFFFFFFFF, 0
   while(i < 6){
      r = (y ^^ (r >> shift)) & 0xFFFFFFFF
      i += 1
   }
   r & 0xFFFFFFFF
}

fn _untemper_left(any: y, int: shift, any: mask): any {
   mut r, i = y & 0xFFFFFFFF, 0
   while(i < 6){
      r = (y ^^ ((r << shift) & mask)) & 0xFFFFFFFF
      i += 1
   }
   r & 0xFFFFFFFF
}

fn mt_untemper(any: y): any {
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

fn mt_temper(any: s): any {
   "Apply MT19937 tempering to internal state word s. Returns the output value."
   mut y = s & 0xFFFFFFFF
   y = y ^^ (y >> 11)
   y = y ^^ ((y << 7) & 0x9D2C5680)
   y = y ^^ ((y << 15) & 0xEFC60000)
   y = y ^^ (y >> 18)
   y & 0xFFFFFFFF
}

fn mt_twist_output_candidates(any: output_i1, any: output_i397): list {
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

fn mt_clone(list: outputs_624): list {
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

fn mt_generate(list: state): list {
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

fn mt_predict(list: state, int: count): list {
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

fn mt19937_next(list: state_orig, int: idx_in): list {
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

fn _mt_smt_bv32(any: ctx, str: name): any { z3_bv_const(ctx, name, 32) }

fn _mt_smt_bv32u(any: ctx, any: v): any { z3_bv_u64(ctx, v & 0xFFFFFFFF, 32) }

fn _mt_smt_shr(any: ctx, any: a, int: s): any { z3_bvlshr(ctx, a, z3_bv_u64(ctx, s, 32)) }

fn _mt_smt_shl(any: ctx, any: a, int: s): any { z3_bvshl(ctx, a, z3_bv_u64(ctx, s, 32)) }

fn _mt_smt_temper(any: ctx, any: x): any {
   mut y = x
   y = z3_bvxor(ctx, y, _mt_smt_shr(ctx, y, 11))
   y = z3_bvxor(ctx, y, z3_bvand(ctx, _mt_smt_shl(ctx, y, 7), _mt_smt_bv32u(ctx, 0x9D2C5680)))
   y = z3_bvxor(ctx, y, z3_bvand(ctx, _mt_smt_shl(ctx, y, 15), _mt_smt_bv32u(ctx, 0xEFC60000)))
   y = z3_bvxor(ctx, y, _mt_smt_shr(ctx, y, 18))
   y
}

fn mt19937_smt_recover_state_prefix(any: constraints): dict {
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
