;; Keywords: symmetric composition
;; Symmetric-crypto routines for cipher composition experiments and checks.
module std.math.crypto.symmetric.composition(encrypt_and_mac_encrypt, encrypt_and_mac_decrypt, encrypt_then_mac_encrypt, encrypt_then_mac_decrypt, mac_then_encrypt_encrypt, mac_then_encrypt_decrypt)
use std.core

fn _composition_equal(any: a, any: b): bool {
   if(is_list(a) && is_list(b)){
      if(a.len != b.len){ return false }
      mut i = 0
      while(i < a.len){
         if(!_composition_equal(a.get(i), b.get(i))){ return false }
         i += 1
      }
      return true
   }
   a == b
}

fn encrypt_and_mac_encrypt(any: msg, fnptr: enc_fn, fnptr: mac_fn): list {
   "Encrypt-and-MAC: MAC is computed over plaintext."
   def ct = enc_fn(msg)
   def tag = mac_fn(msg)
   [ct, tag]
}

fn encrypt_and_mac_decrypt(any: ct, any: tag, fnptr: dec_fn, fnptr: mac_fn): any {
   "Decrypt then authenticate the recovered plaintext."
   def msg = dec_fn(ct)
   if(msg == nil){ return nil }
   def expected = mac_fn(msg)
   if(_composition_equal(expected, tag)){ return msg }
   nil
}

fn encrypt_then_mac_encrypt(any: msg, fnptr: enc_fn, fnptr: mac_fn): list {
   "Encrypt-then-MAC: MAC is computed over ciphertext."
   def ct = enc_fn(msg)
   def tag = mac_fn(ct)
   [ct, tag]
}

fn encrypt_then_mac_decrypt(any: ct, any: tag, fnptr: dec_fn, fnptr: mac_fn): any {
   "Authenticate ciphertext before decryption."
   if(!_composition_equal(mac_fn(ct), tag)){ return nil }
   dec_fn(ct)
}

fn mac_then_encrypt_encrypt(any: msg, fnptr: enc_fn, fnptr: mac_fn): list {
   "MAC-then-Encrypt: encrypt the plaintext concatenated with its tag."
   def tag = mac_fn(msg)
   [enc_fn([msg, tag]), tag]
}

fn mac_then_encrypt_decrypt(any: ct, fnptr: dec_fn, fnptr: mac_fn): any {
   "Decrypt, then split [msg, tag] and authenticate."
   def payload = dec_fn(ct)
   if(!is_list(payload) || payload.len < 2){ return nil }
   def msg = payload.get(0)
   def tag = payload.get(1)
   def expected = mac_fn(msg)
   if(_composition_equal(expected, tag)){ return msg }
   nil
}
