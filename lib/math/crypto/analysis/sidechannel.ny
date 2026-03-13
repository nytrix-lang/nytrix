;; Keywords: analysis side-channel traces
;; Cryptanalysis scoring and recovery routines for side-channel trace thresholding and key-byte recovery.
module std.math.crypto.analysis.sidechannel(trace_column_sums, threshold_trace_bits, bits_lsb_to_bigint, threshold_trace_text, aes_sbox_lsb_leak_count, aes_sbox_lsb_recover_byte, aes_sbox_lsb_recover_key)
use std.math.bin as bin
use std.math.nt
use std.math.crypto.symmetric.aes (aes_sbox)

fn trace_column_sums(list: traces): list {
   "Return per-column sums for a rectangular list of numeric traces."
   def rows = traces.len
   if(rows == 0){ return [] }
   def width = traces[0].len
   mut sums = bin.zero_list(width)
   mut i = 0
   while(i < rows){
      def row = traces[i]
      assert(row.len == width, "trace rows must have equal width")
      mut j = 0
      while(j < width){
         sums[j] = sums[j] + row[j]
         j += 1
      }
      i += 1
   }
   sums
}

fn threshold_trace_bits(list: traces, int: threshold=120): list {
   "Classify each trace column as 1 when its average is at least threshold."
   def rows = traces.len
   if(rows == 0){ return [] }
   def sums = trace_column_sums(traces)
   def n = sums.len
   mut bits = bin.zero_list(n)
   mut i = 0
   while(i < n){
      bits[i] = sums[i] >= threshold * rows ? 1 : 0
      i += 1
   }
   store64(bits, n, 0)
   bits
}

fn bits_lsb_to_bigint(list: bits): bigint {
   "Pack a bit list whose index 0 is the least significant bit."
   def n = bits.len
   mut out = Z(0)
   mut i = 0
   while(i < n){
      if(bits[i] != 0){ out += Z(1) << i }
      i += 1
   }
   out
}

fn threshold_trace_text(list: traces, int: threshold=120): str {
   "Recover ASCII text from thresholded LSB-first trace columns."
   def n = bits_lsb_to_bigint(threshold_trace_bits(traces, threshold))
   n.bytes.trim0.text
}

fn aes_sbox_lsb_leak_count(list: plaintext, list: key): int {
   "Return the leakage count: sum of AES S-box output LSBs for plaintext[i] ^ key[i]."
   def S = aes_sbox()
   def n = plaintext.len < key.len ? plaintext.len : key.len
   mut count = 0
   mut i = 0
   while(i < n){
      count += S[(plaintext[i] ^^ key[i]) & 255] & 1
      i += 1
   }
   count
}

fn aes_sbox_lsb_recover_byte(list: leaks): int {
   "Recover one AES key byte from 256 leakage counts where plaintext byte p ranges 0..255.
   Other byte positions must be fixed, so the minimum leakage is treated as the constant baseline."
   if(leaks == nil || leaks.len != 256){ return -1 }
   def S = aes_sbox()
   mut base = leaks[0]
   mut i = 1
   while(i < 256){
      if(leaks[i] < base){ base = leaks[i] }
      i += 1
   }
   mut k = 0
   while(k < 256){
      mut ok = true
      mut p = 0
      while(p < 256){
         if((leaks[p] - base) != (S[(p ^^ k) & 255] & 1)){ ok = false }
         p += 1
      }
      if(ok){ return k }
      k += 1
   }
   -1
}

fn aes_sbox_lsb_recover_key(list: leak_rows): list {
   "Recover an AES-style key from one 256-count leakage row per key byte."
   mut key = bin.zero_list(leak_rows.len)
   mut i = 0
   while(i < leak_rows.len){
      key[i] = aes_sbox_lsb_recover_byte(leak_rows[i])
      i += 1
   }
   store64(key, leak_rows.len, 0)
   key
}
