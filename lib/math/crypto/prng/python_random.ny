;; Keywords: prng python-random pseudorandom
;; PRNG analysis routines for Python MT19937 random-output modeling.
;;
;; Implements CPython's integer seeding (init_by_array over 32-bit chunks)
;; plus getrandbits(k) on top of MT19937.
;;
;; Reference: CPython Modules/_randommodule.c
module std.math.crypto.prng.python_random(py_mt19937_seed_int, py_mt19937_getrandbits, py_getrandbits_unpack_words_full, py_getrandbits_word_constraints, py_random_float_to_mt_parts, py_mt19937_randbytes, py_mt19937_randbelow, py_mt19937_randrange, py_mt19937_sample_range)
use std.math.nt
use std.math.scalar (pow)
use std.math.crypto.prng.mt19937 (mt19937_next)

fn _mt_init_genrand(any: seed32): list {
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

fn _seed_int_to_key32(any: seed_int): list {
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

fn _mt_init_by_array(list: keys): list {
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

fn py_mt19937_seed_int(any: seed_int): list {
   "Returns [state, index] for Python-compatible MT19937 seeded with integer seed_int."
   def keys = _seed_int_to_key32(seed_int)
   def st = _mt_init_by_array(keys)
   [st, 624]
}

fn py_mt19937_getrandbits(list: state_orig, int: index, int: k): list {
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

fn py_mt19937_randbelow(list: state_in, int: idx_in, int: n): list {
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

fn py_mt19937_randrange(list: state_in, int: idx_in, int: start, int: stop): list {
   "Python Random.randrange(start, stop) for positive unit step. Returns [value, updated_state, new_index]."
   def width = stop - start
   def got = py_mt19937_randbelow(state_in, idx_in, width)
   [start + int(got[0]), got[1], got[2]]
}

fn py_mt19937_sample_range(list: state_in, int: idx_in, int: n, int: k): list {
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

fn py_getrandbits_unpack_words_full(any: value, int: k): list {
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

fn py_getrandbits_word_constraints(any: value, int: k, int: start_index=0): list {
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
         out = out.append({
               "index": start_index + i,
               "kind": "hi",
               "value": (acc >> shift) & mask,
               "bits": rem
         })
      } else {
         out = out.append({
               "index": start_index + i,
               "kind": "eq",
               "value": (acc >> shift) & Z(0xFFFFFFFF),
               "bits": 32
         })
      }
      i += 1
   }
   out
}

fn py_random_float_to_mt_parts(f64: f): list {
   "Convert Python random.random() into the two high-bit MT19937 output parts."
   def scaled = int(f * pow(2.0, 53.0))
   def a = scaled >> 26
   def b = scaled & ((1 << 26) - 1)
   [a, 27, b, 26]
}

fn _fixed_little_endian_bytes(any: value, int: nbytes): list {
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

fn py_mt19937_randbytes(list: state_in, int: idx_in, int: nbytes): list {
   "Reproduce CPython Random.randbytes(nbytes) from a cloned MT state."
   if(nbytes <= 0){ return [[], state_in, idx_in] }
   def r = py_mt19937_getrandbits(state_in, idx_in, nbytes * 8)
   [_fixed_little_endian_bytes(r[0], nbytes), r[1], r[2]]
}
