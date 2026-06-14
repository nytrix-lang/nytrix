;; Keywords: block-cipher mode cbc math crypto
;; Block-mode routines for CBC mode attacks and transformations.
;; Reference:
;; - https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf
;; - https://iacr.org/archive/eurocrypt2002/23320530/cbc02_e02d.pdf
;; References:
;; - std.math.crypto.block.mode
;; - std.math.crypto
module std.math.crypto.block.mode.cbc(cbc_bit_flipping, cbc_bit_flip_byte, cbc_padding_oracle_step, cbc_padding_oracle_decrypt, cbc_padding_oracle_decrypt_block, cbc_mac_forge, cbc_mac_length_extension, cbc_and_cbc_mac_forge)
use std.core
use std.math.bin

fn _cbc_pair(list iv, list ct) list {
   mut out = []
   out = out.append(iv)
   out = out.append(ct)
   out
}

fn cbc_bit_flip_byte(list iv, list ct, int block_size, int pt_block_idx, int byte_offset, int old_byte, int new_byte) list {
   "Flip a single byte in the CBC-decrypted plaintext block pt_block_idx at byte_offset.
   Modifies the preceding ciphertext block(or IV for block 0).
   iv: IV byte list. ct: ciphertext byte list. block_size: e.g. 16.
   Returns [modified_iv, modified_ct]."
   def diff = old_byte ^^ new_byte
   mut new_iv = clone(iv)
   mut new_ct = clone(ct)
   case pt_block_idx {
      0 -> { new_iv[byte_offset] = iv[byte_offset] ^^ diff }
      _ -> {
         def prev_block_start = (pt_block_idx - 1) * block_size
         new_ct[prev_block_start + byte_offset] = ct[prev_block_start + byte_offset] ^^ diff
      }
   }
   _cbc_pair(new_iv, new_ct)
}

fn cbc_bit_flipping(list iv, list ct, int pos, int old_byte, int new_byte) list {
   "Flip a byte at linear position `pos` in the decrypted CBC plaintext.
   iv: IV bytes. ct: ciphertext bytes. Returns modified ciphertext(IV prepended)."
   def block_size = 16
   def block_idx = pos / block_size
   def byte_off = pos % block_size
   def r = cbc_bit_flip_byte(iv, ct, block_size, block_idx, byte_off, old_byte, new_byte)
   def new_iv = r[0]
   def new_ct = r[1]
   mut out = []
   mut i = 0
   while i < new_iv.len {
      out = out.append(new_iv[i])
      i += 1
   }
   i = 0
   while i < new_ct.len {
      out = out.append(new_ct[i])
      i += 1
   }
   out
}

fn cbc_padding_oracle_step(list prev_block, list ct_block, int block_size, list known_bytes, fnptr oracle_fn) int {
   "Recover one more byte of CBC plaintext using a padding oracle.
   prev_block: the preceding ciphertext block(or IV) as byte list.
   ct_block: the target ciphertext block.
   block_size: cipher block size(16).
   known_bytes: list of already-recovered plaintext bytes(rightmost bytes of block).
   oracle_fn(modified_prev, ct_block) -> bool: true if padding is valid.
   Returns the next recovered plaintext byte(0-255), or -1 on failure."
   def pad_val = known_bytes.len + 1
   def target_idx = block_size - pad_val
   mut mod_prev = clone(prev_block)
   mut ri = 0
   while ri < known_bytes.len {
      def byte_pos = block_size - 1 - ri
      def pt_byte = known_bytes[ri]
      mod_prev[byte_pos] = prev_block[byte_pos] ^^ pt_byte ^^ pad_val
      ri += 1
   }
   mut guess = 0
   while guess < 256 {
      mod_prev[target_idx] = guess
      if oracle_fn(mod_prev, ct_block) {
         def pt = guess ^^ pad_val ^^ prev_block[target_idx]
         if pad_val == 1 {
            mod_prev[target_idx] = guess ^^ 1
            if !oracle_fn(mod_prev, ct_block) { return pt }
         } else {
            return pt
         }
      }
      guess += 1
   }
   -1
}

fn cbc_padding_oracle_decrypt_block(list prev_block, list ct_block, int block_size, fnptr oracle_fn) list {
   "Decrypt a single CBC block using a padding oracle.
   Returns the decrypted plaintext block as a byte list."
   mut known = []
   mut i = 0
   while i < block_size {
      def b = cbc_padding_oracle_step(prev_block, ct_block, block_size, known, oracle_fn)
      if b < 0 { known = known.append(0)
      } else { known = known.append(b) }
      i += 1
   }
   mut result = []
   mut j = known.len - 1
   while j >= 0 {
      result = result.append(known[j])
      j = j - 1
   }
   result
}

fn cbc_padding_oracle_decrypt(list iv, list ct, int block_size, fnptr oracle_fn) list {
   "Fully decrypt CBC ciphertext using a padding oracle.
   iv: IV byte list. ct: ciphertext byte list(multiple of block_size).
   oracle_fn(iv_or_prev, ct_block) -> bool: true if padding valid.
   Returns the decrypted plaintext byte list(with PKCS7 padding stripped)."
   def n_blocks = ct.len / block_size
   mut plaintext = []
   mut block_idx = 0
   while block_idx < n_blocks {
      mut prev = []
      if block_idx == 0 { prev = clone(iv) } else {
         mut pi, pj = (block_idx - 1) * block_size, 0
         while pj < block_size {
            prev = prev.append(ct[pi + pj])
            pj += 1
         }
      }
      mut cur_ct = []
      def start = block_idx * block_size
      mut ci = 0
      while ci < block_size {
         cur_ct = cur_ct.append(ct[start + ci])
         ci += 1
      }
      def pt_block = cbc_padding_oracle_decrypt_block(prev, cur_ct, block_size, oracle_fn)
      mut pbi = 0
      while pbi < pt_block.len {
         plaintext = plaintext.append(pt_block[pbi])
         pbi += 1
      }
      block_idx += 1
   }
   def last_byte = plaintext[plaintext.len - 1]
   if last_byte > 0 && last_byte <= block_size {
      def new_len = plaintext.len - last_byte
      mut stripped = []
      mut i = 0
      while i < new_len {
         stripped = stripped.append(plaintext[i])
         i += 1
      }
      return stripped
   }
   plaintext
}

fn cbc_mac_forge(list msg1, list tag1, list msg2, list tag2) list {
   "CBC-MAC length extension forgery.
   Given valid(msg1, tag1) and(msg2, tag2) under the same key,
   produce a forged message msg1 || (msg2 XOR tag1) whose MAC is tag2.
   Returns [forged_message, forged_tag]."
   def tlen = tag1.len
   mut msg2_xor = []
   mut i = 0
   while i < msg2.len {
      def tb = (i < tlen) ? tag1[i] : 0
      msg2_xor = msg2_xor.append(msg2[i] ^^ tb)
      i += 1
   }
   mut forged = clone(msg1)
   i = 0
   while i < msg2_xor.len {
      forged = forged.append(msg2_xor[i])
      i += 1
   }
   [forged, tag2]
}

fn cbc_mac_length_extension(list msg1, list tag1, list msg2, int block_size) list {
   "Extend a CBC-MAC authenticated message by appending msg2.
   Returns [extended_message, tag] where tag is the same as tag1
   and the extended message = msg1 || (msg2 XOR tag1_padded)."
   mut mod_msg2 = clone(msg2)
   def tlen = tag1.len
   mut i = 0
   while i < tlen && i < mod_msg2.len {
      mod_msg2[i] = mod_msg2[i] ^^ tag1[i]
      i += 1
   }
   mut extended = clone(msg1)
   i = 0
   while i < mod_msg2.len {
      extended = extended.append(mod_msg2[i])
      i += 1
   }
   [extended, tag1]
}

fn cbc_and_cbc_mac_forge(list iv, list ct, list tag, list target_msg) list {
   "Forgery attack on encrypt-then-MAC where CBC is used for both.
   Returns [new_iv, forged_ct, tag] where forged_ct decrypts to target_msg."
   def block_size = 16
   def target_len = target_msg.len
   def n_blocks = (target_len + block_size - 1) / block_size
   mut forged_ct = []
   mut bi = 0
   while bi < n_blocks {
      def block_start = bi * block_size
      mut bj = 0
      while bj < block_size {
         def byte_idx = block_start + bj
         def b = (byte_idx < target_len) ? target_msg[byte_idx] : block_size
         forged_ct = forged_ct.append(b)
         bj += 1
      }
      bi += 1
   }
   [clone(iv), forged_ct, clone(tag)]
}
