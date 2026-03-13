;; Keywords: protocol pdf
;; Protocol-analysis routines for PDF password padding, key derivation, and password search.
;; Reference:
;; - https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf
module std.math.crypto.protocol.pdf(pdf_password_pad, pdf_r3_key, pdf_r3_user_hash, pdf_r3_check_user_password, pdf_find_user_password)
use std.math.crypto.hash as hash
use std.math.crypto.block.stream.rc4 (rc4_decrypt_known_key)

fn _pdf_password_padding(): list {
   "Return a fresh copy of the PDF password padding bytes."
   "28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A".unhex
}

fn pdf_password_pad(str: password): list {
   "Pad or truncate a PDF password to 32 bytes."
   def raw = password.to_bytes
   if(raw.len >= 32){ return slice(raw, 0, 32) }
   raw.concat(slice(_pdf_password_padding(), 0, 32 - raw.len))
}

fn pdf_r3_key(str: password, list: owner_hash, list: permissions, list: doc_id, int: key_len_bits=128): list {
   "Derive the PDF revision-3/4 file key for a candidate user password.
   permissions must already be the 4 security-handler bytes."
   mut digest = pdf_password_pad(password).concat(owner_hash).concat(permissions).concat(doc_id)
   mut i = 0
   while(i < 51){
      digest = hash.md5(digest).unhex
      i += 1
   }
   slice(digest, 0, key_len_bits / 8)
}

fn _pdf_repeat_byte(int: byte, int: n): list {
   mut out = list(n)
   mut i = 0
   while(i < n){
      out[i] = byte & 255
      i += 1
   }
   out
}

fn pdf_r3_user_hash(str: password, list: owner_hash, list: permissions, list: doc_id, int: key_len_bits=128): list {
   "Compute the 16-byte PDF revision-3/4 user hash for a candidate password."
   def key = pdf_r3_key(password, owner_hash, permissions, doc_id, key_len_bits)
   mut block = rc4_decrypt_known_key(hash.md5(_pdf_password_padding().concat(doc_id)).unhex, key)
   mut i = 1
   while(i < 20){
      block = rc4_decrypt_known_key(block, key.xor(_pdf_repeat_byte(i, key.len)))
      i += 1
   }
   block
}

fn pdf_r3_check_user_password(str: password, list: user_hash, list: owner_hash, list: permissions, list: doc_id, int: key_len_bits=128): bool {
   "Return true if password matches the given PDF revision-3/4 user hash."
   pdf_r3_user_hash(password, owner_hash, permissions, doc_id, key_len_bits) == user_hash
}

fn pdf_find_user_password(list: candidates, list: user_hash, list: owner_hash, list: permissions, list: doc_id, int: key_len_bits=128): any {
   "Return the first candidate whose PDF revision-3/4 user hash matches, or nil."
   mut i = 0
   while(i < candidates.len){
      def candidate = candidates[i]
      if(pdf_r3_check_user_password(candidate, user_hash, owner_hash, permissions, doc_id, key_len_bits)){
         return candidate
      }
      i += 1
   }
   nil
}
