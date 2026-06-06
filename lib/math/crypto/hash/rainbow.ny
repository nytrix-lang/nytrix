;; Keywords: hash rainbow math crypto
;; Hash-analysis routines for rainbow tables and dictionary hash lookup.
;; Also includes simple preimage lookup with MD5/SHA1.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc1321
;; - https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
;; References:
;; - std.math.crypto.hash
;; - std.math.crypto
module std.math.crypto.hash.rainbow(rainbow_chain, build_rainbow_table, rainbow_lookup, build_dict_table, dict_crack)
use std.core
use std.math.nt

fn rainbow_chain(any start, fnptr hash_fn, fnptr reduce_fn, int chain_len) list {
   "Build one rainbow chain of length chain_len from start.
   hash_fn(plaintext) -> hash. reduce_fn(hash, step) -> plaintext.
   Returns [start, end]."
   mut cur = start
   mut i = 0
   while(i < chain_len){
      cur = reduce_fn(hash_fn(cur), i)
      i += 1
   }
   [start, cur]
}

fn build_rainbow_table(list starts, fnptr hash_fn, fnptr reduce_fn, int chain_len) list {
   "Build a rainbow table from a list of starting plaintexts.
   Returns a list of [start, end] chain entries."
   mut table = []
   mut i = 0
   while(i < starts.len){
      table = table.append(rainbow_chain(starts.get(i), hash_fn, reduce_fn, chain_len))
      i += 1
   }
   table
}

fn rainbow_lookup(any target_hash, list table, fnptr hash_fn, fnptr reduce_fn, int chain_len) any {
   "Search a rainbow table for target_hash.
   For each table entry, walk backwards from the end to find a matching chain.
   Returns the cracked plaintext or nil."
   mut i = 0
   while(i < table.len){
      def entry = table.get(i)
      def start = entry.get(0)
      def end = entry.get(1)
      mut step = chain_len - 1
      while(step >= 0){
         mut cur = target_hash
         mut j = step
         while(j < chain_len){
            cur = hash_fn(reduce_fn(cur, j))
            j += 1
         }
         def endpoint = reduce_fn(target_hash, chain_len - 1)
         if(endpoint == end || step == 0){
            mut pt = start
            mut k = 0
            while(k < chain_len){
               if(hash_fn(pt) == target_hash){ return pt }
               pt = reduce_fn(hash_fn(pt), k)
               k += 1
            }
         }
         step = step - 1
      }
      i += 1
   }
   nil
}

fn build_dict_table(list words, fnptr hash_fn) dict {
   "Build a simple dictionary hash table: {hash -> word}.
   words: list of strings. hash_fn: function(word) -> hash string.
   Returns a dict for fast O(1) lookup."
   mut table = dict(words.len + 8)
   mut i = 0
   while(i < words.len){
      def w = words.get(i)
      table.set(hash_fn(w), w)
      i += 1
   }
   table
}

fn dict_crack(any target_hash, dict dict_table) any {
   "Look up a hash in a prebuilt dictionary table. Returns word or nil."
   dict_table.get(target_hash, nil)
}
