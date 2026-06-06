;; Keywords: support utilities helpers tools math crypto
;; Byte, text, rotation, and flag-extraction utilities shared by crypto modules.
;;
;; These are intentionally generic utilities.
;; References:
;; - std.math.crypto.support
;; - std.math.crypto
module std.math.crypto.support.tools(scan_lines, collect_lines, bytes_contains, find_subseq, rol_bits, ror_bits, bytes_fixed_from_bigint, bytes_ascii, bytes_is_printable_ascii, bytes_has_prefix, extract_flag, extract_flag_bytes, list_uniq, max_bit_length, str_strip_ws, str_strip_bytes_literal)
use std.core
use std.math.bin
use std.math.big
use std.math.nt
use std.core.str as str

fn scan_lines(str txt, fnptr on_line) any {
   "Call on_line(line_str) for each non-empty line in txt.
   If on_line returns false, stops early."
   def n = txt.len
   mut i = 0
   mut start = 0
   while(i <= n){
      if(i == n || load8(txt, i) == 10){
         mut line = str_slice(txt, start, i)
         start = i + 1
         if(line.len > 0 && load8(line, line.len - 1) == 13){ line = str_slice(line, 0, line.len - 1) }
         line = str.strip(line)
         if(line.len > 0){ if(on_line(line) == false){ return nil } }
      }
      i += 1
   }
   nil
}

fn collect_lines(str txt) list {
   "Return all non-empty lines from txt as a list of strings."
   mut out = []
   def n = txt.len
   mut i = 0
   mut start = 0
   while(i <= n){
      if(i == n || load8(txt, i) == 10){
         mut line = str_slice(txt, start, i)
         start = i + 1
         if(line.len > 0 && load8(line, line.len - 1) == 13){ line = str_slice(line, 0, line.len - 1) }
         line = str.strip(line)
         if(line.len > 0){ out = out.append(line) }
      }
      i += 1
   }
   out
}

fn bytes_contains(list haystack, list needle) bool {
   "Return true if needle(byte list) appears in haystack(byte list)."
   find_subseq(haystack, needle) >= 0
}

fn find_subseq(list xs, list pat) int {
   "Return the first index where pat appears in xs, or -1."
   mut i = 0
   while(i + pat.len <= xs.len){
      mut ok = true
      mut j = 0
      while(j < pat.len){
         if(xs[i + j] != pat[j]){
            ok = false
            break
         }
         j += 1
      }
      if(ok){ return i }
      i += 1
   }
   -1
}

fn rol_bits(any x, int shift, int bits) any {
   "Rotate-left x inside a bits-wide word(BigInt).
   shift may be larger than bits."
   if(bits <= 0){ return Z(0) }
   mut s = shift % bits
   if(s < 0){ s += bits }
   if(s == 0){ return mod(x, Z(1) << bits) }
   def MOD = Z(1) << bits
   def left = (x * (Z(1) << s)) % MOD
   def right = x / (Z(1) << (bits - s))
   (left + right) % MOD
}

fn ror_bits(any x, int shift, int bits) any {
   "Rotate-right x inside a bits-wide word(BigInt)."
   if(bits <= 0){ return Z(0) }
   mut s = shift % bits
   if(s < 0){ s += bits }
   if(s == 0){ return mod(x, Z(1) << bits) }
   rol_bits(x, bits - s, bits)
}

fn bytes_fixed_from_bigint(any x, int n) list {
   "Convert bigint x to big-endian bytes and left-pad with zeros to length n."
   def bs0 = Z(x).bytes
   if(bs0.len >= n){ return bs0 }
   mut pad = []
   mut i = 0
   while(i < (n - bs0.len)){
      pad = pad.append(0)
      i += 1
   }
   pad.extend(bs0)
}

fn bytes_ascii(list bs) str {
   "Convert a byte-list to an ASCII/Latin-1 style string."
   bs.text
}

fn bytes_is_printable_ascii(any bs, int min_len=1, int printable_pct=95) bool {
   "Return true if a byte list is mostly printable ASCII.
   Printable bytes are 0x20..0x7e plus LF. printable_pct is an integer percentage threshold."
   if(bs == nil || !is_list(bs)){ return false }
   def n = bs.len
   if(n < min_len){ return false }
   mut good = 0
   mut i = 0
   while(i < n){
      def b = bs.get(i) & 255
      if((b >= 32 && b < 127) || b == 10){ good += 1 }
      i += 1
   }
   good * 100 >= n * printable_pct
}

fn bytes_has_prefix(any bs, any prefix) bool {
   "Return true if byte-list bs starts with prefix. Prefix may be a string or byte list."
   if(bs == nil || prefix == nil || !is_list(bs)){ return false }
   def pn = prefix.len
   if(bs.len < pn){ return false }
   mut i = 0
   while(i < pn){
      def p = is_list(prefix) ? prefix.get(i) : load8(prefix, i)
      if((bs.get(i) & 255) != (p & 255)){ return false }
      i += 1
   }
   true
}

fn extract_flag(str text, str prefix, str suffix="}") str {
   "Extract prefix...suffix from text, or return an empty string."
   def start = str.find(text, prefix)
   if(start < 0){ return "" }
   def stop = str.find_from(text, suffix, start + prefix.len)
   if(stop < 0){ return "" }
   str.str_slice(text, start, stop + suffix.len)
}

fn extract_flag_bytes(list bs, str prefix, str suffix="}") str {
   "Extract a delimited flag from a byte list."
   extract_flag(bytes_ascii(bs), prefix, suffix)
}

fn list_uniq(any xs) list {
   "Return xs with duplicates removed, preserving first occurrence order."
   if(xs == nil){ return [] }
   mut out = []
   mut i = 0
   while(i < xs.len){
      def v = xs.get(i)
      mut seen = false
      mut j = 0
      while(j < out.len){
         if(out.get(j) == v){
            seen = true
            break
         }
         j += 1
      }
      if(!seen){ out = out.append(v) }
      i += 1
   }
   out
}

fn max_bit_length(list xs) int {
   "Return the largest BigInt bit length in xs."
   mut best = 0
   mut i = 0
   while(i < xs.len){
      def b = bit_length(Z(xs[i]))
      if(b > best){ best = b }
      i += 1
   }
   best
}

fn str_strip_ws(any s) str {
   "Trim ASCII whitespace from both ends."
   if(s == nil){ return "" }
   mut i0, i1 = 0, s.len
   while(i0 < i1 && load8(s, i0) <= 32){ i0 += 1 }
   while(i1 > i0 && load8(s, i1 - 1) <= 32){ i1 -= 1 }
   str.str_slice(s, i0, i1)
}

fn str_strip_bytes_literal(any s) str {
   "Strip byte-literal wrappers like b'..' or b\"..\" when present."
   if(s == nil){ return "" }
   def t, n = str_strip_ws(s), t.len
   if(n >= 3 && load8(t, 0) == 98){
      def q = load8(t, 1)
      if((q == 39 || q == 34) && load8(t, n - 1) == q){ return str.str_slice(t, 2, n - 1) }
   }
   t
}
