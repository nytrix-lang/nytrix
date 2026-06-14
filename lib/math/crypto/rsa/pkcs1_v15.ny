;; Keywords: rsa pkcs1-v15 math crypto
;; RSA PKCS#1 v1.5 padding and oracle analysis routines.
;; Reference:
;; - RFC 8017
;; - PKCS #1 v2.2, EMSA-PKCS1-v1_5 and RSAES-PKCS1-v1_5
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.pkcs1_v15(emsa_pkcs1_v15_encode, rsa_pkcs1_v15_sign, rsa_pkcs1_v15_verify, eme_pkcs1_v15_encode, eme_pkcs1_v15_decode, rsa_pkcs1_v15_encrypt, rsa_pkcs1_v15_decrypt, rsa_pkcs1_v15_padding_oracle)
use std.core
use std.math.nt
use std.math.random
use std.math.crypto.hash
use std.math.crypto.rsa.op (compute_phi, compute_d)

def _ASN1_MD5 = [0x30,0x20,0x30,0x0c,0x06,0x08,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x02,0x05,0x05,0x00,0x04,0x10]
def _ASN1_SHA1 = [0x30,0x21,0x30,0x09,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,0x05,0x00,0x04,0x14]
def _ASN1_SHA256 = [0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20]

fn _bytes_concat(list a, list b) list {
   def n = a.len + b.len
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < a.len {
      __store_item_fast(out, i, a[i])
      i += 1
   }
   mut j = 0
   while j < b.len {
      __store_item_fast(out, i + j, b[j])
      j += 1
   }
   out
}

fn _bigint_to_fixed_bytes(any value, int n) list {
   mut out = list(n)
   __list_set_len(out, n)
   mut x = Z(value)
   mut i = n
   while i > 0 {
      i -= 1
      __store_item_fast(out, i, int(x & Z(255)))
      x = x >> Z(8)
   }
   out
}

fn _pkcs1_hash_bytes(str algo, any msg) any {
   if algo == "md5" { return md5(msg).unhex }
   if algo == "sha1" { return sha1(msg).unhex }
   if algo == "sha256" { return sha256(msg) }
   nil
}

fn _pkcs1_digest_info_prefix(str algo) any {
   if algo == "md5" { return _ASN1_MD5 }
   if algo == "sha1" { return _ASN1_SHA1 }
   if algo == "sha256" { return _ASN1_SHA256 }
   nil
}

fn _nonzero_random_byte() int { int(rand() % 255) + 1 }

fn _valid_nonzero_bytes(list ps) bool {
   mut i = 0
   while i < ps.len {
      def b = ps[i]
      if b <= 0 || b > 255 { return false }
      i += 1
   }
   true
}

fn emsa_pkcs1_v15_encode(any msg, int em_len, str algo="sha256") any {
   "Encode msg using EMSA-PKCS1-v1_5 for the selected hash algorithm."
   def digest = _pkcs1_hash_bytes(algo, msg)
   if digest == nil { return nil }
   def prefix = _pkcs1_digest_info_prefix(algo)
   if prefix == nil { return nil }
   def t = _bytes_concat(prefix, digest)
   if em_len < t.len + 11 { return nil }
   def ps_len = em_len - t.len - 3
   mut em = list(em_len)
   __list_set_len(em, em_len)
   __store_item_fast(em, 0, 0)
   __store_item_fast(em, 1, 1)
   mut i = 0
   while i < ps_len {
      __store_item_fast(em, i + 2, 0xff)
      i += 1
   }
   __store_item_fast(em, ps_len + 2, 0)
   i = 0
   while i < t.len {
      __store_item_fast(em, ps_len + 3 + i, t[i])
      i += 1
   }
   em
}

fn rsa_pkcs1_v15_sign(any msg, number d, number n, str algo="sha256") any {
   "Sign msg using RSA PKCS#1 v1.5."
   def k = (bit_length(n) + 7) / 8
   def em = emsa_pkcs1_v15_encode(msg, k, algo)
   if em == nil { return nil }
   power_mod(bytes_to_bigint(em), d, n)
}

fn rsa_pkcs1_v15_verify(any msg, number sig, number e, number n, str algo="sha256") bool {
   "Verify an RSA PKCS#1 v1.5 signature."
   def k = (bit_length(n) + 7) / 8
   def em_expect = emsa_pkcs1_v15_encode(msg, k, algo)
   if em_expect == nil { return false }
   power_mod(sig, e, n) == bytes_to_bigint(em_expect)
}

fn eme_pkcs1_v15_encode(list message, int k, any ps=nil) any {
   "Encode byte list message as an RSAES-PKCS1-v1_5 block of k bytes.
   ps may be supplied for deterministic tests and must contain nonzero bytes."
   if k < message.len + 11 { return nil }
   def ps_len = int(k - message.len - 3)
   mut pad = []
   if ps == nil {
      pad = list(ps_len)
      __list_set_len(pad, ps_len)
      mut i = 0
      while i < ps_len {
         __store_item_fast(pad, i, _nonzero_random_byte())
         i += 1
      }
   } else {
      if ps.len != ps_len { return nil }
      if !_valid_nonzero_bytes(ps) { return nil }
      pad = ps
   }
   mut out = list(k)
   __list_set_len(out, k)
   __store_item_fast(out, 0, 0)
   __store_item_fast(out, 1, 2)
   mut j = 0
   while j < pad.len {
      __store_item_fast(out, j + 2, pad[j])
      j += 1
   }
   __store_item_fast(out, ps_len + 2, 0)
   j = 0
   while j < message.len {
      def b = message[j]
      if b < 0 || b > 255 { return nil }
      __store_item_fast(out, ps_len + 3 + j, b)
      j += 1
   }
   out
}

fn eme_pkcs1_v15_decode(list em) any {
   "Decode an RSAES-PKCS1-v1_5 encoded block. Returns message bytes or nil."
   if em.len < 11 { return nil }
   if em[0] != 0 || em[1] != 2 { return nil }
   mut sep = -1
   mut i = 2
   while i < em.len {
      if em[i] == 0 && sep < 0 { sep = i }
      i += 1
   }
   if sep < 10 { return nil }
   i = 2
   while i < sep {
      if em[i] == 0 { return nil }
      i += 1
   }
   def msg_len = em.len - sep - 1
   mut msg = list(msg_len)
   __list_set_len(msg, msg_len)
   i = sep + 1
   mut j = 0
   while i < em.len {
      __store_item_fast(msg, j, em[i])
      i += 1
      j += 1
   }
   msg
}

fn rsa_pkcs1_v15_encrypt(list message, number e, number n, any ps=nil) any {
   "Encrypt byte list message with RSAES-PKCS1-v1_5. Returns integer ciphertext."
   def k = (bit_length(n) + 7) / 8
   def em = eme_pkcs1_v15_encode(message, k, ps)
   if em == nil { return nil }
   power_mod(bytes_to_bigint(em), e, n)
}

fn rsa_pkcs1_v15_decrypt(number ciphertext, number d, number n) any {
   "Decrypt RSAES-PKCS1-v1_5 integer ciphertext. Returns message bytes or nil."
   def k, m = (bit_length(n) + 7) / 8, power_mod(ciphertext, d, n)
   eme_pkcs1_v15_decode(_bigint_to_fixed_bytes(m, k))
}

fn rsa_pkcs1_v15_padding_oracle(number ciphertext, number d, number n) bool {
   "Return true if RSA decryption has RSAES-PKCS1-v1_5 block structure."
   rsa_pkcs1_v15_decrypt(ciphertext, d, n) != nil
}

#main {
   def msg = "ny".to_bytes
   def ps = [1,2,3,4,5,6,7,8]
   def em = eme_pkcs1_v15_encode(msg, msg.len + 11, ps)
   assert(em != nil && eme_pkcs1_v15_decode(em) == msg, "PKCS1 v1.5 deterministic EME roundtrip")
   print("✓ std.math.crypto.rsa.pkcs1_v15 self-test passed")
}
