;; Keywords: protocol seal
;; SEAL-style byte transforms and digest/counter XOR routines.
;;
;; These are deliberately small primitives rather than an automatic oracle:
;; many sealed payloads are some keyed byte stream XORed with a blob,
;; sometimes with the nonce/counter tucked into the blob itself.
module std.math.crypto.protocol.seal(seal_digest_bytes, seal_counter_bytes, seal_repeating_key_xor, seal_digest_repeat_xor, seal_counter_xor, seal_hmac_sha256_counter_xor, seal_blake2s_keyed_counter_xor, seal_single_byte_xor, seal_single_byte_add, seal_single_byte_sub, seal_reverse_bytes, seal_mostly_printable_ascii, seal_has_prefix, seal_has_suffix, seal_ascii_envelope)
use std.math.bin as bin
use std.math.crypto.hash as hash

fn seal_reverse_bytes(list: bs): list {
   "Returns a byte list in reverse order."
   mut out = []
   mut i = bs.len - 1
   while(i >= 0){
      out = out.append(bs.get(i))
      i -= 1
   }
   out
}

fn seal_has_prefix(list: bs, any: prefix): bool {
   "Returns true when byte list `bs` starts with `prefix`."
   if(bs == nil || prefix == nil){ return false }
   def p = is_str(prefix) ? prefix.to_bytes : prefix
   if(bs.len < p.len){ return false }
   mut i = 0
   while(i < p.len){
      if((bs.get(i) & 255) != (p.get(i) & 255)){ return false }
      i += 1
   }
   true
}

fn seal_has_suffix(list: bs, any: suffix): bool {
   "Returns true when byte list `bs` ends with `suffix`."
   if(bs == nil || suffix == nil){ return false }
   def s = is_str(suffix) ? suffix.to_bytes : suffix
   if(bs.len < s.len){ return false }
   mut i = 0
   def off = bs.len - s.len
   while(i < s.len){
      if((bs.get(off + i) & 255) != (s.get(i) & 255)){ return false }
      i += 1
   }
   true
}

fn seal_mostly_printable_ascii(list: bs, int: min_pct=95): bool {
   "Returns true when at least `min_pct` percent of bytes are printable ASCII."
   if(bs == nil || bs.len == 0){ return false }
   mut good = 0
   mut i = 0
   while(i < bs.len){
      def b = bs.get(i) & 255
      if((b >= 32 && b <= 126) || b == 10 || b == 13 || b == 9){ good += 1 }
      i += 1
   }
   good * 100 >= bs.len * min_pct
}

fn seal_ascii_envelope(list: bs, any: prefix="", any: suffix="", int: min_pct=95): bool {
   "Checks prefix, suffix, and printable ASCII ratio for decoded payloads."
   seal_has_prefix(bs, prefix) && seal_has_suffix(bs, suffix) && seal_mostly_printable_ascii(bs, min_pct)
}

fn _seal_u32be(int: n): list { [(n >> 24) & 255, (n >> 16) & 255, (n >> 8) & 255, n & 255] }

fn _seal_u32le(int: n): list { [n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255] }

fn _seal_u64be(int: n): list {
   [
      (n >> 56) & 255, (n >> 48) & 255, (n >> 40) & 255, (n >> 32) & 255,
      (n >> 24) & 255, (n >> 16) & 255, (n >> 8) & 255, n & 255
   ]
}

fn _seal_u64le(int: n): list {
   [
      n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255,
      (n >> 32) & 255, (n >> 40) & 255, (n >> 48) & 255, (n >> 56) & 255
   ]
}

fn seal_counter_bytes(int: counter, str: format): list {
   "Render a counter as bytes. format: byte, be4, le4, be8, le8, dec."
   if(format == "byte"){ return [counter & 255] }
   if(format == "be4"){ return _seal_u32be(counter) }
   if(format == "le4"){ return _seal_u32le(counter) }
   if(format == "be8"){ return _seal_u64be(counter) }
   if(format == "le8"){ return _seal_u64le(counter) }
   if(format == "dec"){ return to_str(counter).to_bytes }
   panic("unknown counter format: " + format)
}

fn seal_digest_bytes(str: name, any: data): list {
   "Return digest bytes for hash functions whose local APIs differ."
   if(name == "sha256"){ return hash.sha256(data) }
   if(name == "md5"){ return hash.md5(data).unhex }
   if(name == "sha1"){ return hash.sha1(data).unhex }
   if(name == "sha512"){ return hash.sha512(data).unhex }
   if(name == "sha3_256"){ return hash.sha3_256(data).unhex }
   if(name == "sha3_512"){ return hash.sha3_512(data).unhex }
   if(name == "blake2s"){ return hash.blake2s(data).unhex }
   panic("unknown digest: " + name)
}

fn _seal_repeat_to_len(list: key, int: n): list {
   mut out = []
   if(key == nil || key.len == 0){ return out }
   while(out.len < n){
      mut i = 0
      while(i < key.len && out.len < n){
         out = out.append(key.get(i) & 255)
         i += 1
      }
   }
   out
}

fn _seal_append_all(list: out, list: chunk): list {
   mut acc = out
   mut i = 0
   while(i < chunk.len){
      acc = acc.append(chunk.get(i) & 255)
      i += 1
   }
   acc
}

fn seal_repeating_key_xor(list: blob, list: key): list {
   "XOR blob with a repeating key."
   if(key == nil || key.len == 0){ return [] }
   blob.xor(_seal_repeat_to_len(key, blob.len))
}

fn seal_digest_repeat_xor(list: blob, any: seed, str: digest_name): list {
   "XOR blob with digest(seed) repeated to blob length."
   seal_repeating_key_xor(blob, seal_digest_bytes(digest_name, seed))
}

fn seal_counter_xor(list: blob, list: seed, str: digest_name, str: counter_format="be4", str: order="seed-counter", int: start=0): list {
   "XOR blob with digest(seed||counter) or digest(counter||seed) blocks."
   mut stream = []
   mut ctr = start
   while(stream.len < blob.len){
      def ctr_bytes = seal_counter_bytes(ctr, counter_format)
      def data = (order == "counter-seed") ? ctr_bytes.concat(seed) : seed.concat(ctr_bytes)
      stream = _seal_append_all(stream, seal_digest_bytes(digest_name, data))
      ctr += 1
   }
   blob.xor(slice(stream, 0, blob.len))
}

fn seal_hmac_sha256_counter_xor(list: blob, any: key, str: counter_format="be4", int: start=0): list {
   "XOR blob with HMAC-SHA256(key, counter) blocks."
   mut stream = []
   mut ctr = start
   while(stream.len < blob.len){
      stream = _seal_append_all(stream, hash.sha256_hmac(key, seal_counter_bytes(ctr, counter_format)))
      ctr += 1
   }
   blob.xor(slice(stream, 0, blob.len))
}

fn _seal_key32(list: key): list {
   if(key.len <= 32){ return key }
   slice(key, 0, 32)
}

fn seal_blake2s_keyed_counter_xor(list: blob, list: key, str: counter_format="be4", int: start=0): list {
   "XOR blob with keyed BLAKE2s(counter, key=key[:32]) blocks."
   mut stream = []
   mut ctr = start
   def k = _seal_key32(key)
   while(stream.len < blob.len){
      stream = _seal_append_all(stream, hash.blake2s(seal_counter_bytes(ctr, counter_format), k, 32).unhex)
      ctr += 1
   }
   blob.xor(slice(stream, 0, blob.len))
}

fn seal_single_byte_xor(list: blob, int: k): list {
   "XORs every byte in `blob` with one byte key `k`."
   mut out = []
   mut i = 0
   while(i < blob.len){
      out = out.append((blob.get(i) ^^ k) & 255)
      i += 1
   }
   out
}

fn seal_single_byte_add(list: blob, int: k): list {
   "Adds one byte key `k` to every byte in `blob` modulo 256."
   mut out = []
   mut i = 0
   while(i < blob.len){
      out = out.append((blob.get(i) + k) & 255)
      i += 1
   }
   out
}

fn seal_single_byte_sub(list: blob, int: k): list {
   "Subtracts one byte key `k` from every byte in `blob` modulo 256."
   mut out = []
   mut i = 0
   while(i < blob.len){
      out = out.append((blob.get(i) - k) & 255)
      i += 1
   }
   out
}
