;; Keywords: cipher auto-detect decode chain classical encoding math crypto
;; Auto-detect and chain-decode classical cipher and encoding schemes.
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.encoding
module std.math.crypto.cipher.auto(decode_try, decode_chain)
use std.core
use std.math.bin (hex_is_valid)
use std.math.crypto.encoding.xor (single_byte_xor_bruteforce)
use std.math.crypto.encoding.encoding (hex_decode)
use std.math.crypto.encoding.base (decode64, decode32, decode16)
use std.math.crypto.encoding.radix (base58_decode_str, ascii85_decode, base92_decode)
use std.math.crypto.cipher.atbash (atbash_text)
use std.math.crypto.cipher.morse (morse_decode)
use std.math.crypto.cipher.bacon (bacon_decode_ab)
use std.math.crypto.cipher.caesar (caesar_bruteforce)
use std.core.str (to_hex)

fn _is_printable_byte(int c) bool {
   case c {
      9, 10, 13 -> true
      32..126 -> true
      _ -> false
   }
}

fn _is_alpha_byte(int c) bool {
   case c {
      65..90, 97..122 -> true
      _ -> false
   }
}

fn _dc_score(str text) int {
   def bs = text.to_bytes
   if bs.len == 0 { return -9999 }
   mut printable = 0
   mut alpha = 0
   mut vowel = 0
   mut space = 0
   mut i = 0
   while i < bs.len {
      def b = bs.get(i)
      if _is_printable_byte(b) { printable = printable + 1 }
      if _is_alpha_byte(b) {
         alpha = alpha + 1
         def u = b >= 97 ? b - 32 : b
         if u == 65 || u == 69 || u == 73 || u == 79 || u == 85 || u == 89 { vowel = vowel + 1 }
      }
      if b == 32 { space = space + 1 }
      i = i + 1
   }
   mut score = printable * 100 - (bs.len - printable) * 200
   if alpha > 0 {
      score = score + alpha * 50
      def vp = vowel * 100 / alpha
      if vp < 15 { score = score - 100 }
      if vp > 55 { score = score - 50 }
   }
   if space > 0 && alpha > 0 {
      def sp = space * 100 / bs.len
      if sp >= 5 && sp <= 35 { score = score + 200 }
      if sp >= 5 { score = score + 100 }
   }
   score
}

fn _dc_push(list cands, str kind, str result, int score) list {
   mut i = 0
   while i < cands.len {
      def row = cands.get(i)
      if row.get(1) == result {
         if score > row.get(2) { cands[i] = [kind, result, score] }
         return cands
      }
      i = i + 1
   }
   cands.append([kind, result, score])
}

fn _dc_sort(list cands) list {
   def out = clone(cands)
   mut i = 1
   while i < out.len {
      def row = out.get(i)
      def s = row.get(2, -99999)
      mut j = i - 1
      while j >= 0 && out.get(j).get(2, -99999) < s {
         out[j + 1] = out.get(j)
         j = j - 1
      }
      out[j + 1] = row
      i = i + 1
   }
   out
}

fn _dc_try_base(str s, list cands) list {
   mut out = cands
   if s.len >= 4 && hex_is_valid(s) {
      def pt = hex_decode(s).text
      out = _dc_push(out, "hex", pt, _dc_score(pt))
   }
   if s.len > 8 && s.len % 4 == 0 {
      def b64 = decode64(s)
      if b64 != "" && b64.len > 2 { out = _dc_push(out, "base64", b64, _dc_score(b64)) }
   }
   if s.len > 4 {
      def b32 = decode32(s)
      if b32 != "" && b32.len > 2 { out = _dc_push(out, "base32", b32, _dc_score(b32)) }
      def b16 = decode16(s)
      if b16 != "" && b16.len > 2 { out = _dc_push(out, "base16", b16, _dc_score(b16)) }
      def b58 = base58_decode_str(s)
      if b58 != nil && b58.len > 2 { out = _dc_push(out, "base58", b58.text, _dc_score(b58.text)) }
      def b85 = ascii85_decode(s)
      if b85 != "" && b85.len > 2 { out = _dc_push(out, "ascii85", b85, _dc_score(b85)) }
   }
   if s.len >= 2 {
      def b92 = base92_decode(s)
      if b92 != "" && b92.len > 2 { out = _dc_push(out, "base92", b92, _dc_score(b92)) }
   }
   out
}

fn _dc_try_cipher(str s, list cands) list {
   mut out = cands
   if s.len < 2 { return out }
   def at = atbash_text(s)
   if at != s { out = _dc_push(out, "atbash", at, _dc_score(at)) }
   if s.contains(".") || s.contains("-") || s.contains("/") {
      def mo = morse_decode(s)
      if mo != "" && mo.len > 1 { out = _dc_push(out, "morse", mo, _dc_score(mo)) }
   }
   def ab = upper(s)
   mut only_ab = true
   mut ai = 0
   while ai < ab.len && only_ab {
      def c = load8(ab, ai)
      if c != 65 && c != 66 { only_ab = false }
      ai = ai + 1
   }
   if only_ab && s.len >= 5 {
      def ba = bacon_decode_ab(ab)
      if ba != "" { out = _dc_push(out, "bacon", ba, _dc_score(ba)) }
   }
   def up = upper(s)
   mut all_alpha = true
   mut uj = 0
   while uj < up.len && all_alpha {
      if !_is_alpha_byte(load8(up, uj)) { all_alpha = false }
      uj = uj + 1
   }
   if all_alpha {
      def results = caesar_bruteforce(s)
      mut k = 0
      while k < results.len {
         def row = results.get(k)
         out = _dc_push(out, "caesar_" + str(row.get(0)), row.get(1), _dc_score(row.get(1)))
         k = k + 1
      }
   }
   out
}

fn _dc_try_raw(list data, list cands) list {
   def best = single_byte_xor_bruteforce(data)
   def pt = best.get(1)
   def sc = best.get(2) + _dc_score(pt)
   _dc_push(cands, "xor_0x" + to_hex(best.get(0)), pt, sc)
}

fn decode_try(str input) list {
   "Tries all known decoding strategies on `input` and returns ranked candidates as [name, text, score] triples."
   mut cands = []
   if input.len == 0 { return cands }
   def ident_score = _dc_score(input)
   cands = cands.append(["identity", input, ident_score])
   cands = _dc_try_base(input, cands)
   cands = _dc_try_cipher(input, cands)
   def bs = input.to_bytes
   if bs.len <= 2048 { cands = _dc_try_raw(bs, cands) }
   _dc_sort(cands)
}

fn decode_chain(str input) list {
   "Recursively applies decode_try to find the best multi-step decoding chain. Returns the merged candidate list."
   mut best = decode_try(input)
   if best.len == 0 { return best }
   def top = best.get(0)
   def top_result = top.get(1)
   def top_score = top.get(2)
   if top_result == input || top_score <= _dc_score(input) { return best }
   def next = decode_chain(top_result)
   mut merged = clone(best)
   mut ni = 0
   while ni < next.len {
      def nr = next.get(ni)
      merged = _dc_push(merged, nr.get(0), nr.get(1), nr.get(2))
      ni = ni + 1
   }
   _dc_sort(merged)
}

#main {
   def r0 = decode_try("")
   assert(r0.len == 0, "empty input returns empty")
   def r1 = decode_try("URYYB, JBEYQ!")
   assert(r1.len > 0, "non-empty input returns candidates")
   def top = r1.get(0)
   assert(top.get(1) != "", "top candidate result non-empty")
   def r2 = decode_try("48656C6C6F")
   mut has_hex = false
   mut ri = 0
   while ri < r2.len {
      if r2.get(ri).get(1) == "Hello" { has_hex = true }
      ri = ri + 1
   }
   assert(has_hex, "hex decode produces Hello")
   print("pass")
}
