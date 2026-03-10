;; Keywords: block-cipher stream otp
;; Stream-cipher routines for one-time-pad reuse analysis and keystream recovery.
;; Reference:
;; - https://netlab.cs.ucla.edu/wiki/files/shannon1949.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
module std.math.crypto.block.stream.otp(otp_reuse_attack, otp_decrypt_known_plaintext, otp_hamming_distance, otp_guess_key_sizes, otp_score_english, otp_recover_reused_key, otp_apply_key, otp_timestamp_sha256_key, otp_timestamp_sha256_xor, otp_timestamp_sha256_bruteforce)
use std.core
use std.math.bin (bit_count)
use std.math.scalar (float, log10)
use std.math.crypto.hash (sha256)

fn _otp_min(int: a, int: b): int { (a < b) ? a : b }

fn _otp_is_printable(int: b): bool { (b >= 32 && b <= 126) || b == 9 || b == 10 || b == 13 }

fn _otp_lower(int: b): int { (b >= 65 && b <= 90) ? (b + 32) : b }

fn _otp_default_printable_weight(int: b): int {
   def c = _otp_lower(b)
   case c {
      32 -> 130
      101 -> 127
      116 -> 91
      97 -> 82
      111 -> 75
      105 -> 70
      110 -> 67
      115 -> 63
      104 -> 61
      114 -> 60
      100 -> 43
      108 -> 40
      99, 117 -> 28
      109 -> 24
      102 -> 22
      103, 119, 121 -> 20
      112 -> 19
      98 -> 15
      118 -> 10
      107 -> 8
      106, 120 -> 2
      113, 122 -> 1
      48..57 -> 12
      34, 39, 44, 45, 46, 58, 59 -> 16
      9, 10, 13 -> 8
      33..126 -> 1
      _ -> -1
   }
}

fn _otp_default_weight(int: b): int {
   def c = _otp_lower(b)
   case c {
      32 -> 130
      101 -> 127
      116 -> 91
      97 -> 82
      111 -> 75
      105 -> 70
      110 -> 67
      115 -> 63
      104 -> 61
      114 -> 60
      100 -> 43
      108 -> 40
      99, 117 -> 28
      109 -> 24
      102 -> 22
      103, 119, 121 -> 20
      112 -> 19
      98 -> 15
      118 -> 10
      107 -> 8
      106, 120 -> 2
      113, 122 -> 1
      48..57 -> 12
      34, 39, 44, 45, 46, 58, 59 -> 16
      9, 10, 13 -> 8
      33..126 -> 1
      _ -> 0
   }
}

fn _otp_freq_weight(any: char_frequencies, int: b, any: char_floor): any {
   case char_frequencies {
      nil -> _otp_default_weight(b)
      _ -> {
         def c = _otp_lower(b)
         def direct = char_frequencies.get(c, nil)
         def weight = direct == nil ? char_frequencies.get([c].text, nil) : direct
         case weight {
            nil -> char_floor
            _ if weight <= 0 -> char_floor
            _ -> log10(weight)
         }
      }
   }
}

fn _otp_insert_ranked(list: ranked, list: item): list {
   mut out = list(0)
   mut inserted = false
   mut i = 0
   while(i < ranked.len){
      def cur = ranked.get(i)
      if(!inserted && item.get(1) > cur.get(1)){
         out = out.append(item)
         inserted = true
      }
      out = out.append(cur)
      i += 1
   }
   if(!inserted){ out = out.append(item) }
   out
}

fn otp_hamming_distance(list: a, list: b): int {
   "Return the bit Hamming distance between two byte lists."
   def n = _otp_min(a.len, b.len)
   mut distance = 0
   mut i = 0
   while(i < n){
      distance += bit_count(__load_item_fast(a, i) ^^ __load_item_fast(b, i))
      i += 1
   }
   distance
}

fn otp_guess_key_sizes(list: ciphertexts, any: max_key_size=nil): list {
   "Rank likely repeated-OTP/repeating-XOR key sizes by normalized Hamming distance.
   Returns key sizes ordered from most likely to least likely."
   def nct = ciphertexts.len
   if(nct == 0){ return [] }
   mut max_len = 0
   mut ci = 0
   while(ci < nct){
      max_len = max(max_len, len(ciphertexts.get(ci)))
      ci += 1
   }
   def limit = min((max_key_size == nil) ? max_len : max_key_size, max_len)
   if(limit < 2){ return [] }
   mut ranked = list(0)
   mut prev_distance = nil
   mut key_size = 2
   while(key_size <= limit){
      mut blocks = list(0)
      ci = 0
      while(ci < nct){
         def chunks = ciphertexts.get(ci).windowed(key_size, key_size)
         mut bi = 0
         while(bi < chunks.len){
            blocks = blocks.append(chunks.get(bi))
            bi += 1
         }
         ci += 1
      }
      if(blocks.len >= 2){
         mut total = 0
         mut bi = 0
         while(bi + 1 < blocks.len){
            total = total + otp_hamming_distance(blocks.get(bi), blocks.get(bi + 1))
            bi += 1
         }
         def distance = (float(total) / float(blocks.len - 1)) / float(key_size)
         if(prev_distance != nil){ ranked = _otp_insert_ranked(ranked, [key_size, prev_distance - distance]) }
         prev_distance = distance
      }
      key_size += 1
   }
   mut out = list(0)
   mut i = 0
   while(i < ranked.len){
      out = out.append(ranked.get(i).get(0))
      i += 1
   }
   out
}

fn otp_score_english(list: bytes, any: char_frequencies=nil, any: char_floor=-5): any {
   "Score a byte list as printable English-like text. Returns nil for non-printable bytes."
   mut score = 0
   mut i = 0
   if(char_frequencies == nil){
      while(i < bytes.len){
         def w = _otp_default_printable_weight(__load_item_fast(bytes, i))
         if(w < 0){ return nil }
         score += w
         i += 1
      }
      return score
   }
   while(i < bytes.len){
      def b = __load_item_fast(bytes, i)
      if(!_otp_is_printable(b)){ return nil }
      score = score + _otp_freq_weight(char_frequencies, b, char_floor)
      i += 1
   }
   score
}

fn _otp_transpose(list: ciphertexts, int: offset, int: key_size): list {
   mut total = 0
   mut ci = 0
   while(ci < ciphertexts.len){
      def ct = ciphertexts.get(ci)
      if(offset < ct.len){ total += ((ct.len - offset + key_size - 1) / key_size) }
      ci += 1
   }
   mut out = list(total)
   store64(out, total, 0)
   mut pos = 0
   ci = 0
   while(ci < ciphertexts.len){
      def ct = ciphertexts.get(ci)
      mut j = offset
      while(j < ct.len){
         __store_item_fast(out, pos, __load_item_fast(ct, j))
         pos += 1
         j = j + key_size
      }
      ci += 1
   }
   out
}

fn _otp_score_english_xor_key(list: bytes, int: key, any: char_frequencies, any: char_floor): any {
   mut score = 0
   mut i = 0
   if(char_frequencies == nil){
      while(i < bytes.len){
         def w = _otp_default_printable_weight(__load_item_fast(bytes, i) ^^ key)
         if(w < 0){ return nil }
         score += w
         i += 1
      }
      return score
   }
   while(i < bytes.len){
      def b = __load_item_fast(bytes, i) ^^ key
      if(!_otp_is_printable(b)){ return nil }
      score = score + _otp_freq_weight(char_frequencies, b, char_floor)
      i += 1
   }
   score
}

fn _otp_frequency_key_byte(list: bytes, any: char_frequencies, any: char_floor): any {
   mut best_key = nil
   mut best_score = nil
   mut k = 0
   while(k < 256){
      def score = _otp_score_english_xor_key(bytes, k, char_frequencies, char_floor)
      if(score != nil && (best_score == nil || score > best_score)){
         best_score = score
         best_key = k
      }
      k += 1
   }
   best_key
}

fn _otp_minimal_period(list: key): list {
   def n = key.len
   mut period = 1
   while(period <= n){
      if(n % period == 0){
         mut ok = true
         mut i = 0
         while(i < n && ok){
            if(__load_item_fast(key, i) != __load_item_fast(key, i % period)){ ok = false }
            i += 1
         }
         if(ok){
            mut out = list(period)
            store64(out, period, 0)
            mut j = 0
            while(j < period){
               __store_item_fast(out, j, __load_item_fast(key, j))
               j += 1
            }
            return out
         }
      }
      period += 1
   }
   key
}

fn otp_recover_reused_key(list: ciphertexts, any: key_size=nil, any: max_key_size=nil, any: char_frequencies=nil, any: char_floor=-5): ?list {
   "Recover a reused OTP/repeating-XOR key by transposition and frequency analysis.
   ciphertexts: list of byte lists encrypted with the same repeating key.
   key_size: optional known key size. If nil, likely sizes are ranked by Hamming distance.
   Returns the best printable key candidate by total plaintext score, or nil."
   def sizes = (key_size == nil) ? otp_guess_key_sizes(ciphertexts, max_key_size) : [key_size]
   mut best_key = nil
   mut best_score = nil
   mut si = 0
   while(si < sizes.len){
      def ks = sizes.get(si)
      mut key = list(0)
      mut ok = true
      mut i = 0
      while(i < ks && ok){
         def column = _otp_transpose(ciphertexts, i, ks)
         def kb = _otp_frequency_key_byte(column, char_frequencies, char_floor)
         if(kb == nil){ ok = false } else { key = key.append(kb) }
         i += 1
      }
      if(ok){
         key = _otp_minimal_period(key)
         mut total_score = 0
         mut ci = 0
         while(ci < ciphertexts.len && ok){
            def plain = otp_apply_key(ciphertexts.get(ci), key)
            def score = otp_score_english(plain, char_frequencies, char_floor)
            if(score == nil){ ok = false } else { total_score = total_score + score }
            ci += 1
         }
         if(ok && (best_score == nil || total_score > best_score)){
            best_score = total_score
            best_key = key
         }
      }
      si += 1
   }
   best_key
}

fn otp_apply_key(list: data, list: key): list {
   "XOR data with a repeating key."
   def n = data.len
   def kn = key.len
   if(kn == 0){ return clone(data) }
   mut out = list(n)
   store64(out, n, 0)
   mut i = 0
   while(i < n){
      __store_item_fast(out, i, __load_item_fast(data, i) ^^ __load_item_fast(key, i % kn))
      i += 1
   }
   out
}

fn otp_timestamp_sha256_key(int: timestamp): list {
   "Return a SHA-256 keystream block derived from the big-endian timestamp bytes."
   sha256(timestamp.bytes)
}

fn otp_timestamp_sha256_xor(list: data, int: timestamp): list {
   "XOR data with a SHA-256 keystream block derived from a timestamp.
   The input must fit in one digest block."
   def key = otp_timestamp_sha256_key(timestamp)
   def n = data.len
   if(n > key.len){ panic("otp timestamp sha256: data too long for one digest") }
   mut out = list(n)
   store64(out, n, 0)
   mut i = 0
   while(i < n){
      __store_item_fast(out, i, (__load_item_fast(data, i) ^^ __load_item_fast(key, i)) & 255)
      i += 1
   }
   out
}

fn _otp_timestamp_sha256_match(list: ciphertext, int: timestamp, str: prefix, str: suffix): any {
   def plain = otp_timestamp_sha256_xor(ciphertext, timestamp)
   if(otp_score_english(plain) == nil){ return nil }
   def text = plain.text
   def prefix_ok = prefix.len == 0 || text.startswith(prefix)
   def suffix_ok = suffix.len == 0 || text.endswith(suffix)
   prefix_ok && suffix_ok ? plain : nil
}

fn otp_timestamp_sha256_bruteforce(list: ciphertext, int: center, int: radius, str: prefix="", str: suffix=""): any {
   "Try timestamp-derived SHA-256 OTP keys in [center-radius, center+radius].
   Searches from center outward and returns the nearest printable plaintext whose
   optional prefix/suffix checks match, else nil."
   mut offset = 0
   while(offset <= radius){
      def lower = center - offset
      def lower_plain = _otp_timestamp_sha256_match(ciphertext, lower, prefix, suffix)
      if(lower_plain != nil){ return [lower, lower_plain] }
      if(offset != 0){
         def upper = center + offset
         def upper_plain = _otp_timestamp_sha256_match(ciphertext, upper, prefix, suffix)
         if(upper_plain != nil){ return [upper, upper_plain] }
      }
      offset += 1
   }
   nil
}

fn otp_reuse_attack(list: ct1, list: ct2): list {
   "XOR two ciphertexts encrypted under the same OTP key to recover P1 XOR P2.
   ct1 and ct2 are byte lists encrypted with the same one-time pad key.
   Returns the XOR of the two plaintexts as a byte list."
   def n1, n2 = ct1.len, ct2.len
   mut n = (n1 < n2) ? n1 : n2
   mut result = list(n)
   store64(result, n, 0)
   mut i = 0
   while(i < n){
      __store_item_fast(result, i, __load_item_fast(ct1, i) ^^ __load_item_fast(ct2, i))
      i += 1
   }
   result
}

fn otp_decrypt_known_plaintext(list: ct, list: known_pt): list {
   "Recover the OTP keystream from known plaintext, then decrypt full ciphertext.
   known_pt is the known plaintext corresponding to the start of ct.
   Returns the full decrypted plaintext as a byte list."
   def ks_len = known_pt.len
   def ct_len = ct.len
   mut result = list(ct_len)
   store64(result, ct_len, 0)
   mut i = 0
   while(i < ct_len){
      def p = (i < ks_len) ? __load_item_fast(known_pt, i) : __load_item_fast(ct, i)
      __store_item_fast(result, i, p)
      i += 1
   }
   result
}

impl list {
   @inline
   fn otp_xor(list: data, list: key): list {
      "XOR this byte list with a repeating OTP key."
      otp_apply_key(data, key)
   }
   @inline
   fn hamming(list: data, list: other): int {
      "Return the Hamming distance between this byte list and another list."
      otp_hamming_distance(data, other)
   }
}
