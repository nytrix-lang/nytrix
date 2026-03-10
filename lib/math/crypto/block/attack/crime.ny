;; Keywords: block-cipher attack crime
;; Block-cipher attack routines for CRIME compression-oracle attack.
;; Recover secrets by exploiting compression before encryption
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc7457
module std.math.crypto.block.attack.crime(crime_attack, crime_recover_with_alphabet, deflate_compress)
use std.core

fn deflate_compress(list: data): list {
   "Simple DEFLATE-like compression using LZ77 + Huffman approximation.
   Implements a simplified version of DEFLATE that finds repeated strings
   and applies basic length encoding.
   data: byte list to compress
   Returns compressed byte list."
   def n = data.len
   if(n == 0){ return [] }
   mut result = list(0)
   mut i = 0
   while(i < n){
      def window_start = (i >= 256) ? (i - 256) : 0
      mut best_len = 0
      mut best_off = 0
      mut j = window_start
      while(j < i){
         if(data[j] == data[i]){
            mut match_len = 0
            while(
               match_len < 18 &&
               i + match_len < n &&
               data[j + match_len] == data[i + match_len]
            ){
               match_len += 1
            }
            if(match_len > best_len){
               best_len = match_len
               best_off = i - j
            }
         }
         j += 1
      }
      if(best_len >= 3){
         result = result.append(1)
         result = result.append(best_len)
         result = result.append(best_off % 256)
         i = i + best_len
      } else {
         result = result.append(0)
         result = result.append(data[i])
         i += 1
      }
   }
   result
}

fn crime_attack(fnptr: encrypt_oracle, list: secret_prefix, list: known_suffix, int: block_size): list {
   "Recover secret via CRIME attack using compression oracle.
   Exploits the fact that compression reveals repeated strings:
   when attacker-controlled input overlaps with secret, compressed size decreases.
   encrypt_oracle: function(data) that compresses+encrypts and returns ciphertext length
   secret_prefix: known prefix before the secret
   known_suffix: known suffix after the secret(optional, use [])
   block_size: encryption block size for alignment
   Returns the recovered secret as a byte list."
   mut recovered = list(0)
   mut charset = list(0)
   mut c = 32
   while(c < 127){
      charset = charset.append(c)
      c += 1
   }
   charset = charset.append(10)
   charset = charset.append(13)
   mut done = false
   mut iter = 0
   while(!done && iter < 256){
      mut best_char = -1
      mut best_size = 999999
      mut ch_idx = 0
      while(ch_idx < charset.len){
         def ch = charset[ch_idx]
         mut test_data = clone(secret_prefix)
         mut ri = 0
         while(ri < recovered.len){
            test_data = test_data.append(recovered[ri])
            ri += 1
         }
         test_data = test_data.append(ch)
         mut si = 0
         while(si < known_suffix.len){
            test_data = test_data.append(known_suffix[si])
            si += 1
         }
         def size = encrypt_oracle(test_data)
         if(size < best_size){
            best_size = size
            best_char = ch
         }
         ch_idx += 1
      }
      if(best_char < 0){ done = true } else { recovered = recovered.append(best_char) }
      iter += 1
   }
   recovered
}

fn crime_recover_with_alphabet(fnptr: length_oracle, list: known_prefix, list: alphabet, int: stop_byte, int: max_len): list {
   "Recover a compression-oracle secret by extending known_prefix with bytes from alphabet.
   length_oracle(payload) must return the encrypted compressed length for secret || payload.
   Stops after stop_byte is recovered or max_len extra bytes are tried."
   mut recovered = clone(known_prefix)
   mut iter = 0
   while(iter < max_len){
      mut best_char = -1
      mut best_size = 999999999
      mut ai = 0
      while(ai < alphabet.len){
         def ch = alphabet[ai]
         mut payload = clone(recovered)
         payload = payload.append(ch)
         def size = length_oracle(payload)
         if(size < best_size){
            best_size = size
            best_char = ch
         }
         ai += 1
      }
      if(best_char < 0){ return recovered }
      recovered = recovered.append(best_char)
      if(best_char == stop_byte){ return recovered }
      iter += 1
   }
   recovered
}
