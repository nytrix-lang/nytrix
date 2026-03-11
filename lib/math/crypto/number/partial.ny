;; Keywords: number-theory partial
;; Crypto number-theory routines for partial-integer modeling for known and unknown bit ranges.
;; Layout:
;; [bit_length, unknown_count, components]
;; where each component is [value_or_nil, bit_length] in little-endian segment
;; order, growing toward the most-significant end.
module std.math.crypto.number.partial(partial_integer_new, partial_integer_add_known, partial_integer_add_unknown, partial_integer_known_lsb, partial_integer_known_msb, partial_integer_known_middle, partial_integer_unknown_lsb, partial_integer_unknown_msb, partial_integer_unknown_middle, partial_integer_matches, partial_integer_sub, partial_integer_known_and_unknowns, partial_integer_unknown_bounds, partial_integer_to_int, partial_integer_to_string_le, partial_integer_to_string_be, partial_integer_to_bits_le, partial_integer_to_bits_be, partial_integer_from_bits_le)
use std.core
use std.core.str as str

fn _pi_pair(any: a, any: b): list { [a, b] }

fn _pi_triple(any: a, any: b, any: c): list { [a, b, c] }

fn partial_integer_new(): list {
   "Create an empty partial integer."
   _pi_triple(0, 0, [])
}

fn _pi_valid(any: pi): bool { is_list(pi) && pi.len >= 3 }

fn _pi_bit_length(any: pi): int { _pi_valid(pi) ? pi[0] : 0 }

fn _pi_unknowns(any: pi): int { _pi_valid(pi) ? pi[1] : 0 }

fn _pi_components(any: pi): list {
   def cs = _pi_valid(pi) ? pi[2] : 0
   is_list(cs) ? cs : []
}

fn _pi_make(int: bit_length, int: unknowns, list: components): list { _pi_triple(bit_length, unknowns, components) }

fn _pi_push(any: pi, any: value, int: bit_length): list {
   def base = _pi_valid(pi) ? pi : partial_integer_new()
   def comp = _pi_pair(value, bit_length)
   _pi_make(_pi_bit_length(base) + bit_length, _pi_unknowns(base), _pi_components(base).append(comp))
}

fn partial_integer_add_known(any: pi, number: value, int: bit_length): list {
   "Append a known bit segment to a partial integer."
   _pi_push(pi, value, bit_length)
}

fn partial_integer_add_unknown(any: pi, int: bit_length): list {
   "Append an unknown bit segment to a partial integer."
   def out = _pi_push(pi, nil, bit_length)
   _pi_make(_pi_bit_length(out), _pi_unknowns(out) + 1, _pi_components(out))
}

fn partial_integer_from_bits_le(list: bits): list {
   "Build a partial integer from little-endian bits, using nil for unknown bits."
   mut out = partial_integer_new()
   mut i = 0
   while(i < bits.len){
      def b = bits.get(i)
      if(b == nil){ out = partial_integer_add_unknown(out, 1) }
      else { out = partial_integer_add_known(out, int(b) & 1, 1) }
      i += 1
   }
   out
}

fn partial_integer_known_lsb(any: pi): list {
   "Return [value, bits] for the known least-significant prefix."
   mut lsb = 0
   mut bits = 0
   def cs = _pi_components(pi)
   mut i = 0
   while(i < cs.len){
      def c, v = cs[i], c[0]
      def n = c[1]
      if(v == nil){ return _pi_pair(lsb, bits) }
      lsb += v << bits
      bits += n
      i += 1
   }
   _pi_pair(lsb, bits)
}

fn partial_integer_known_msb(any: pi): list {
   "Return [value, bits] for the known most-significant suffix."
   mut msb = 0
   mut bits = 0
   def cs = _pi_components(pi)
   mut i = cs.len - 1
   while(i >= 0){
      def c, v = cs[i], c[0]
      def n = c[1]
      if(v == nil){ return _pi_pair(msb, bits) }
      msb = (msb << n) + v
      bits += n
      i -= 1
   }
   _pi_pair(msb, bits)
}

fn partial_integer_known_middle(any: pi): list {
   "Return [value, bits] for the first contiguous known middle segment."
   mut middle = 0
   mut bits = 0
   def cs = _pi_components(pi)
   mut i = 0
   while(i < cs.len){
      def c, v = cs[i], c[0]
      def n = c[1]
      if(v == nil){ if(bits > 0){ return _pi_pair(middle, bits) } } else {
         middle += v << bits
         bits += n
      }
      i += 1
   }
   _pi_pair(middle, bits)
}

fn partial_integer_unknown_lsb(any: pi): int {
   "Return the bit length of the unknown least-significant prefix."
   mut bits = 0
   def cs = _pi_components(pi)
   mut i = 0
   while(i < cs.len){
      def c = cs[i]
      if(c[0] != nil){ return bits }
      bits += c[1]
      i += 1
   }
   bits
}

fn partial_integer_unknown_msb(any: pi): int {
   "Return the bit length of the unknown most-significant suffix."
   mut bits = 0
   def cs = _pi_components(pi)
   mut i = cs.len - 1
   while(i >= 0){
      def c = cs[i]
      if(c[0] != nil){ return bits }
      bits += c[1]
      i -= 1
   }
   bits
}

fn partial_integer_unknown_middle(any: pi): int {
   "Return the bit length of the first contiguous unknown middle segment."
   mut bits = 0
   def cs = _pi_components(pi)
   mut i = 0
   while(i < cs.len){
      def c = cs[i]
      if(c[0] == nil){ if(bits > 0){ return bits } } else { bits += c.get(1, 0) }
      i += 1
   }
   bits
}

fn partial_integer_matches(any: pi, number: v): bool {
   "Return true if integer v matches all known segments."
   mut shift = 0
   def cs = _pi_components(pi)
   mut i = 0
   while(i < cs.len){
      def c = cs[i]
      def cv = c[0]
      def n = c[1]
      if(cv != nil && (((v >> shift) % (1 << n)) != cv)){ return false }
      shift += n
      i += 1
   }
   true
}

fn partial_integer_sub(any: pi, list: unknowns): any {
   "Substitute unknown segment values and return the completed integer."
   if(unknowns.len != _pi_unknowns(pi)){ return nil }
   mut out = 0
   mut shift = 0
   mut ui = 0
   def cs = _pi_components(pi)
   mut i = 0
   while(i < cs.len){
      def c = cs[i]
      def cv = c[0]
      def n = c[1]
      if(cv == nil){
         out += unknowns[ui] << shift
         ui += 1
      } else {
         out += cv << shift
      }
      shift += n
      i += 1
   }
   out
}

fn partial_integer_known_and_unknowns(any: pi): list {
   "Return [known_value, unknown_offsets, unknown_lengths]."
   mut known = 0
   mut offs = []
   mut lens = []
   mut off = 0
   def cs = _pi_components(pi)
   mut i = 0
   while(i < cs.len){
      def c = cs[i]
      def cv = c[0]
      def n = c[1]
      if(cv == nil){
         offs = offs.append(off)
         lens = lens.append(n)
      } else {
         known += cv << off
      }
      off += n
      i += 1
   }
   _pi_triple(known, offs, lens)
}

fn partial_integer_unknown_bounds(any: pi): list {
   "Return exclusive bounds for each unknown segment."
   mut out = []
   def cs = _pi_components(pi)
   mut i = 0
   while(i < cs.len){
      def c = cs[i]
      if(c[0] == nil){ out = out.append(1 << c[1]) }
      i += 1
   }
   out
}

fn partial_integer_to_int(any: pi): any {
   "Return the concrete integer when no unknown segments remain."
   _pi_unknowns(pi) == 0 ? partial_integer_sub(pi, []) : nil
}

fn _pi_log2_pow2(int: base): int {
   mut n, v = 0, base
   while(v > 1 && (v % 2) == 0){
      v /= 2
      n += 1
   }
   v == 1 ? n : -1
}

fn partial_integer_to_string_le(any: pi, int: base, str: symbols="0123456789abcdefghijklmnopqrstuvwxyz"): any {
   "Render partial-integer digits least-significant first."
   def bits_per_el = _pi_log2_pow2(base)
   if(bits_per_el <= 0 || base > 36 || symbols.len < base){ return nil }
   mut chars = []
   def cs = _pi_components(pi)
   mut i = 0
   while(i < cs.len){
      def c = cs[i]
      mut v = c[0]
      def n = c[1]
      if((n % bits_per_el) != 0){ return nil }
      mut j = 0
      while(j < (n / bits_per_el)){
         if(v == nil){ chars = chars.append("?") } else {
            def idx = v % base
            chars = chars.append(str.utf8_slice(symbols, idx, idx + 1, 1))
            v = v / base
         }
         j += 1
      }
      i += 1
   }
   chars
}

fn partial_integer_to_string_be(any: pi, int: base, str: symbols="0123456789abcdefghijklmnopqrstuvwxyz"): any {
   "Render partial-integer digits most-significant first."
   def chars = partial_integer_to_string_le(pi, base, symbols)
   if(chars == nil){ return nil }
   reverse(chars)
}

fn partial_integer_to_bits_le(any: pi, str: symbols="01"): any {
   "Render partial-integer bits least-significant first."
   partial_integer_to_string_le(pi, 2, symbols)
}

fn partial_integer_to_bits_be(any: pi, str: symbols="01"): any {
   "Render partial-integer bits most-significant first."
   partial_integer_to_string_be(pi, 2, symbols)
}
