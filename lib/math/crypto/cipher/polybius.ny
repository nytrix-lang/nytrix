;; Keywords: cipher polybius math crypto
;; Classical cipher routines for Polybius square encoding and decoding.
;; Core substitution step for ADFGX and ADFGVX ciphers
;; Reference:
;; - https://en.wikipedia.org/wiki/Polybius_square
;; - https://en.wikipedia.org/wiki/ADFGVX_cipher
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.polybius(polybius_decode_pair, polybius_decode_text, polybius_encode_char, polybius_encode_text, polybius_make_grid)
use std.core
use std.core.str

fn _pchar(str text, int i) str { utf8_slice(text, i, i + 1, 1) }

fn _pnormalize(str ch) str {
   def up = upper(ch)
   up == "J" ? "I" : up
}

fn polybius_make_grid(str key_phrase, str alphabet="ABCDEFGHIKLMNOPQRSTUVWXYZ", int size=5) str {
   "Build a Polybius square from key phrase. Deduplicates, appends remaining alphabet chars. Returns the grid string."
   mut seen = set(32)
   mut grid = ""
   mut i = 0
   while i < utf8_len(key_phrase) {
      def ch = _pnormalize(_pchar(key_phrase, i))
      if !seen.contains(ch) && str_contains(alphabet, ch) {
         grid = str_add(grid, ch)
         seen = seen.add(ch)
      }
      i += 1
   }
   mut j = 0
   while j < utf8_len(alphabet) {
      def ch = _pchar(alphabet, j)
      if !seen.contains(ch) {
         grid = str_add(grid, ch)
         seen = seen.add(ch)
      }
      j += 1
   }
   grid
}

fn polybius_encode_char(str ch, str grid, str row_labels="12345", str col_labels="12345", int size=5) str {
   "Encode a single character as a(row, col) pair. Returns 2-char string or '' if not in grid."
   def lookup = _pnormalize(ch)
   mut i = 0
   while i < utf8_len(grid) {
      if _pchar(grid, i) == lookup {
         def row = i / size
         def col = i % size
         return _pchar(row_labels, row) + _pchar(col_labels, col)
      }
      i += 1
   }
   ""
}

fn polybius_encode_text(str text, str grid, str row_labels="12345", str col_labels="12345", str sep=" ", int size=5) str {
   "Encode text to Polybius coordinate pairs separated by sep."
   mut out = ""
   mut first = true
   def up = upper(text)
   mut i = 0
   while i < utf8_len(up) {
      def ch = _pchar(up, i)
      def code = polybius_encode_char(ch, grid, row_labels, col_labels, size)
      if code.len > 0 {
         if !first { out = str_add(out, sep) }
         out = str_add(out, code)
         first = false
      }
      i += 1
   }
   out
}

fn polybius_decode_pair(str pair, str grid, int row_base=49, int col_base=49, int size=5) str {
   "Decode a 2-char coordinate pair to a letter using the Polybius grid."
   if pair.len < 2 { return "?" }
   def row = ord_at(pair, 0) - row_base
   def col = ord_at(pair, 1) - col_base
   def idx = row * size + col
   if idx < 0 || idx >= utf8_len(grid) { return "?" }
   _pchar(grid, idx)
}

fn polybius_decode_text(str text, str grid, int row_min=49, int row_max=53, int col_min=49, int col_max=53, int size=5) str {
   "Decode Polybius-encoded text. Strips spaces/commas and processes coordinate pairs."
   mut cleaned = ""
   mut i = 0
   while i < text.len {
      def ch = load8(text, i)
      if ch != 32 && ch != 9 && ch != 44 { cleaned = str_add(cleaned, chr(ch)) }
      i += 1
   }
   mut out = ""
   mut j = 0
   while j + 1 < cleaned.len {
      def ch1, ch2 = ord_at(cleaned, j), ord_at(cleaned, j + 1)
      if ch1 >= row_min && ch1 <= row_max && ch2 >= col_min && ch2 <= col_max {
         out = str_add(out, polybius_decode_pair(str_slice(cleaned, j, j + 2, 1), grid, row_min, col_min, size))
         j = j + 2
      } else {
         j += 1
      }
   }
   out
}
