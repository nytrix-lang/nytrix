;; Keywords: cipher adfgvx math crypto
;; ADFGVX substitution and transposition cipher routines.
;; 6×6 Polybius substitution (letters ADFGVX) + columnar transposition.
;; Reference:
;; - https://en.wikipedia.org/wiki/ADFGVX_cipher
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.adfgvx(adfgvx_encrypt, adfgvx_decrypt, adfgvx_substitute, adfgvx_desubstitute, adfgvx_build_matrix)
use std.core.str

def ADFGVX_COORDS = [
   "AA", "AD", "AF", "AG", "AV", "AX",
   "DA", "DD", "DF", "DG", "DV", "DX",
   "FA", "FD", "FF", "FG", "FV", "FX",
   "GA", "GD", "GF", "GG", "GV", "GX",
   "VA", "VD", "VF", "VG", "VV", "VX",
   "XA", "XD", "XF", "XG", "XV", "XX",
]

fn _char_at(str text, int i) str { utf8_slice(text, i, i + 1, 1) }

fn _normalized_char(str ch) str {
   def up = upper(ch)
   up == "J" ? "I" : up
}

fn _matrix_index(str matrix, str target) int {
   mut found = -1
   mut i = 0
   while(i < matrix.len){
      found = (found < 0 && _char_at(matrix, i) == target) ? i : found
      i += 1
   }
   found
}

fn _coord_index(str pair) int {
   mut found = -1
   mut i = 0
   while(i < ADFGVX_COORDS.len){
      found = (found < 0 && ADFGVX_COORDS.get(i) == pair) ? i : found
      i += 1
   }
   found
}

fn adfgvx_build_matrix(str keyword, bool include_digits) str {
   "Build the 6×6 ADFGVX substitution matrix from a keyword.
   Fills with A-Z(merging I/J) and 0-9 if include_digits is true.
   Returns the 36-character string."
   mut seen = dict(40)
   mut matrix = ""
   mut ki = 0
   while(ki < keyword.len){
      def ck = _normalized_char(_char_at(keyword, ki))
      if(!seen.get(ck, false)){
         seen.set(ck, true)
         matrix = str_add(matrix, ck)
      }
      ki += 1
   }
   mut alphabet = "ABCDEFGHIKLMNOPQRSTUVWXYZ"
   if(include_digits){ alphabet = str_add(alphabet, "0123456789") }
   mut ai = 0
   while(ai < alphabet.len){
      def ch = _char_at(alphabet, ai)
      if(!seen.get(ch, false)){
         seen.set(ch, true)
         matrix = str_add(matrix, ch)
      }
      ai += 1
   }
   matrix
}

fn adfgvx_substitute(str text, str matrix) str {
   "Apply ADFGVX Polybius substitution to text using the given 36-char matrix.
   Returns the substituted string(pairs of ADFGVX chars)."
   mut out = ""
   mut i = 0
   while(i < text.len){
      def found = _matrix_index(matrix, _normalized_char(_char_at(text, i)))
      if(found >= 0){ out = str_add(out, ADFGVX_COORDS.get(found)) }
      i += 1
   }
   out
}

fn adfgvx_desubstitute(str coded, str matrix) str {
   "Reverse ADFGVX Polybius substitution. coded is string of ADFGVX pairs.
   Returns the original characters."
   mut out = ""
   mut i = 0
   while(i + 1 < coded.len){
      def pair = utf8_slice(coded, i, i + 2, 1)
      def found = _coord_index(pair)
      if(found >= 0 && found < matrix.len){ out = str_add(out, _char_at(matrix, found)) }
      i = i + 2
   }
   out
}

fn _col_order(str key) list {
   def n = key.len
   mut used = list(n)
   mut order = list(n)
   mut out_i = 0
   while(out_i < n){
      mut best = -1
      mut i = 0
      while(i < n){
         if(!used.get(i, false)){
            def ch = _char_at(key, i)
            if(best < 0 || ch < _char_at(key, best)){ best = i }
         }
         i += 1
      }
      if(best >= 0){
         order[out_i] = best
         used[best] = true
      }
      out_i += 1
   }
   order
}

fn adfgvx_encrypt(str plaintext, str matrix, str transposition_key) str {
   "Encrypt plaintext using ADFGVX cipher.
   plaintext: input text. matrix: 36-char substitution matrix.
   transposition_key: columnar transposition keyword.
   Returns the ciphertext string."
   def subst = adfgvx_substitute(plaintext, matrix)
   def n_rows = subst.len
   def n_cols = transposition_key.len
   if(n_cols == 0){ return subst }
   def order = _col_order(transposition_key)
   mut out = ""
   mut oi = 0
   while(oi < order.len){
      def col = order.get(oi)
      mut ri = col
      while(ri < n_rows){
         out = str_add(out, _char_at(subst, ri))
         ri = ri + n_cols
      }
      oi += 1
   }
   out
}

fn _column_lengths(list order, int n_chars, int n_cols) list {
   def n_full_rows = n_chars / n_cols
   def extra = n_chars % n_cols
   mut lens = list(n_cols)
   mut oi = 0
   while(oi < n_cols){
      def c = order.get(oi)
      lens[oi] = n_full_rows + (c < extra ? 1 : 0)
      oi += 1
   }
   lens
}

fn _cipher_columns(str ciphertext, list col_lens) list {
   mut cols = list(col_lens.len)
   mut pos = 0
   mut ci = 0
   while(ci < col_lens.len){
      def clen = col_lens.get(ci)
      cols[ci] = utf8_slice(ciphertext, pos, pos + clen, 1)
      pos = pos + clen
      ci += 1
   }
   cols
}

fn _blank_grid(int n_chars) list {
   mut grid = list(n_chars)
   mut i = 0
   while(i < n_chars){
      grid[i] = ""
      i += 1
   }
   grid
}

fn _read_rows(list cols, list order, int n_chars, int n_cols) list {
   mut grid = _blank_grid(n_chars)
   mut off = list(n_cols)
   mut r = 0
   while(r < n_chars / n_cols + 1){
      mut c2 = 0
      while(c2 < n_cols){
         def col_data = cols.get(c2)
         def o = off.get(c2)
         if(o < col_data.len){
            def row = r * n_cols + order.get(c2)
            if(row < grid.len){
               grid[row] = _char_at(col_data, o)
               off[c2] = o + 1
            }
         }
         c2 += 1
      }
      r += 1
   }
   grid
}

fn adfgvx_decrypt(str ciphertext, str matrix, str transposition_key) str {
   "Decrypt ADFGVX ciphertext.
   ciphertext: encrypted string. matrix: 36-char substitution matrix.
   transposition_key: keyword used during encryption.
   Returns the plaintext."
   def n_cols = transposition_key.len
   def n_chars = ciphertext.len
   if(n_cols == 0){ return adfgvx_desubstitute(ciphertext, matrix) }
   def order = _col_order(transposition_key)
   def cols = _cipher_columns(ciphertext, _column_lengths(order, n_chars, n_cols))
   def grid = _read_rows(cols, order, n_chars, n_cols)
   def subst = join(grid, "")
   adfgvx_desubstitute(subst, matrix)
}
