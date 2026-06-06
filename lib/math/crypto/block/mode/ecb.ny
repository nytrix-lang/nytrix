;; Keywords: block-cipher mode ecb math crypto
;; Block-mode routines for ECB detection and byte-at-a-time oracle attacks.
;; Reference:
;; - https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf
;; References:
;; - std.math.crypto.block.mode
;; - std.math.crypto
module std.math.crypto.block.mode.ecb(ecb_detect, ecb_byte_by_byte, ecb_byte_at_a_time_suffix, cbc_fixed_iv_byte_at_a_time_suffix, oracle_detect_block_size, zero_pad_to_block, locked_dungeon_mod_pad, ecb_split_blocks, ecb_splice_blocks)
use std.core
use std.math.bin

fn ecb_split_blocks(list ciphertext, int block_size=16) list {
   "Split ciphertext bytes into fixed-size ECB blocks. Drops no trailing bytes."
   mut blocks = []
   mut i = 0
   while(i < ciphertext.len){
      def end = (i + block_size < ciphertext.len) ? i + block_size : ciphertext.len
      blocks = blocks.append(slice(ciphertext, i, end))
      i += block_size
   }
   blocks
}

fn ecb_splice_blocks(list blocks) list {
   "Concatenate selected ECB blocks into a forged ciphertext."
   mut out = []
   mut i = 0
   while(i < blocks.len){
      def block = blocks[i]
      mut j = 0
      while(j < block.len){
         out = out.append(block[j])
         j += 1
      }
      i += 1
   }
   out
}

fn zero_pad_to_block(list data, int block_size, int zero_byte=48) list {
   "Pad a byte list with zero_byte until its length is a multiple of block_size.
   This models legacy oracles that append ASCII '0' instead of PKCS#7."
   mut out = clone(data)
   while(out.len % block_size != 0){ out = out.append(zero_byte) }
   out
}

fn locked_dungeon_mod_pad(list data, int secret_size, int block_size=16, int pad_limit=48) list {
   "Model a quirky byte-at-a-time oracle that normally PKCS#7-pads, but for
   inputs longer than pad_limit deletes a window around the secret boundary."
   def input_len = data.len
   if(input_len > pad_limit){
      def excess_len = input_len - pad_limit
      mut out = []
      if(excess_len > secret_size){
         mut i = secret_size
         while(i < secret_size + pad_limit && i < data.len){
            out = out.append(data[i])
            i += 1
         }
      } else {
         mut i = 0
         while(i < secret_size - excess_len && i < data.len){
            out = out.append(data[i])
            i += 1
         }
         i = secret_size
         while(i < data.len){
            out = out.append(data[i])
            i += 1
         }
      }
      return out
   }
   pkcs7_pad(data, block_size)
}

fn ecb_detect(list ciphertext, int block_size) int {
   "Detect ECB mode by checking for duplicate ciphertext blocks.
   ciphertext: byte list of ciphertext
   block_size: size of cipher blocks(e.g. 16 for AES)
   Returns the number of duplicate block pairs found(0 means likely not ECB)."
   def ct_len = ciphertext.len
   def num_blocks = ct_len / block_size
   mut duplicates = 0
   mut i = 0
   while(i < num_blocks){
      mut j = i + 1
      while(j < num_blocks){
         mut match_found = true
         mut k = 0
         while(k < block_size){
            def bi, bj = ciphertext[i * block_size + k], ciphertext[j * block_size + k]
            if(bi != bj){
               match_found = false
               k = block_size
            }
            k += 1
         }
         if(match_found){ duplicates += 1 }
         j += 1
      }
      i += 1
   }
   duplicates
}

fn ecb_byte_by_byte(fnptr oracle_fn, int block_size) list {
   "Byte-at-a-time suffix recovery entrypoint.
   Recovers up to three blocks if the exact secret length is unknown."
   ecb_byte_at_a_time_suffix(oracle_fn, block_size, block_size * 3)
}

fn oracle_detect_block_size(fnptr oracle_fn, int max_probe=128) any {
   "Detect the block size of a deterministic append-and-encrypt oracle by watching ciphertext length jumps."
   def base_len = len(oracle_fn([]))
   mut n = 1
   while(n <= max_probe){
      mut probe = []
      mut i = 0
      while(i < n){
         probe = probe.append(65)
         i += 1
      }
      def cur_len = len(oracle_fn(probe))
      if(cur_len > base_len){ return cur_len - base_len }
      n += 1
   }
   nil
}

fn _bytes_equal_at(list a, int a_start, list b, int b_start, int block_size) bool {
   mut i = 0
   while(i < block_size){
      if(a[a_start + i] != b[b_start + i]){ return false }
      i += 1
   }
   true
}

fn _byte_at_a_time_suffix(fnptr oracle_fn, int block_size, int max_len) list {
   mut recovered = []
   mut byte_idx = 0
   while(byte_idx < max_len){
      def block_index = byte_idx / block_size
      def offset_in_block = byte_idx % block_size
      def prefix_len = block_size - 1 - offset_in_block
      mut prefix = []
      mut i = 0
      while(i < prefix_len){
         prefix = prefix.append(65)
         i += 1
      }
      def ct = oracle_fn(prefix)
      def target_start = block_index * block_size
      if(ct.len < target_start + block_size){ return recovered }
      mut guess = 0
      mut found = false
      while(guess < 256){
         mut test_input = clone(prefix)
         i = 0
         while(i < recovered.len){
            test_input = test_input.append(recovered[i])
            i += 1
         }
         test_input = test_input.append(guess)
         def test_ct = oracle_fn(test_input)
         if(
            test_ct.len >= target_start + block_size &&
            _bytes_equal_at(ct, target_start, test_ct, target_start, block_size)
         ){
            recovered = recovered.append(guess)
            found = true
            guess = 256
         }
         guess += 1
      }
      if(!found){ return recovered }
      byte_idx += 1
   }
   recovered
}

fn ecb_byte_at_a_time_suffix(fnptr oracle_fn, int block_size, int max_len) list {
   "Recover a secret appended to attacker-controlled input by a deterministic ECB oracle.
   oracle_fn(prefix_bytes) must return ciphertext for prefix || secret."
   _byte_at_a_time_suffix(oracle_fn, block_size, max_len)
}

fn cbc_fixed_iv_byte_at_a_time_suffix(fnptr oracle_fn, int block_size, int max_len) list {
   "Recover an appended secret from deterministic CBC with a fixed IV.
   Identical controlled prefixes keep previous CBC blocks identical,
   so the same block dictionary strategy works for the target block."
   _byte_at_a_time_suffix(oracle_fn, block_size, max_len)
}
