;; Exact crypt(3) compatibility vectors (not format-only checks).
use core.math.crypto.analysis.crypt as crypt
use core.math.crypto.hash as hash
use core.math.crypto.analysis.dictionary (unix_crypt_hash)

fn repeated_byte_buffer(int n) bytes {
   mut bytes out = bytes(n)
   mut i = 0
   while i < n {
      store8(out, 97, i)
      i += 1
   }
   out
}

fn test_sha256_digest_spans() {
   mut byte_msg = bytes(3)
   store8(byte_msg, 97, 0)
   store8(byte_msg, 98, 1)
   store8(byte_msg, 99, 2)
   assert(hash.sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256-empty")
   assert(hash.sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256-abc")
   assert(hash.sha256_hex(byte_msg) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256-bytes")
   mut bytes reusable_digest = bytes(32)
   assert(hash.sha256_into(byte_msg, reusable_digest), "sha256-into-accepted")
   assert(reusable_digest.hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256-into-bytes")
   assert(hash.sha256_into(repeated_byte_buffer(55), reusable_digest), "sha256-into-55-accepted")
   assert(reusable_digest.hex == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", "sha256-into-55-byte-boundary")
   assert(hash.sha256_into(repeated_byte_buffer(56), reusable_digest), "sha256-into-56-accepted")
   assert(reusable_digest.hex == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", "sha256-into-56-byte-boundary")
   assert(hash.sha256_into(repeated_byte_buffer(64), reusable_digest), "sha256-into-64-accepted")
   assert(reusable_digest.hex == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", "sha256-into-block-boundary")
   assert(hash.sha256_into(repeated_byte_buffer(65), reusable_digest), "sha256-into-65-accepted")
   assert(reusable_digest.hex == "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0", "sha256-into-multiblock")
   assert(!hash.sha256_into(byte_msg, bytes(31)), "sha256-into-short-output")
   assert(hash.sha256_hex(str.repeat("a", 55)) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", "sha256-55-byte-boundary")
   assert(hash.sha256_hex(str.repeat("a", 56)) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", "sha256-56-byte-boundary")
   assert(hash.sha256_hex(str.repeat("a", 64)) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", "sha256-block-boundary")
   assert(hash.sha256_hex(str.repeat("a", 65)) == "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0", "sha256-multiblock")
   assert(hash.sha256_hmac("key", "The quick brown fox jumps over the lazy dog").hex == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", "hmac-sha256-short-input")
   assert(hash.sha256_hmac_bytes("key", "The quick brown fox jumps over the lazy dog").hex == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", "hmac-sha256-byte-result")
   assert(hash.sha256_hmac("", "").hex == "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad", "hmac-sha256-empty")
   assert(hash.sha256_hmac([107, 101, 121], [84, 104, 101]).hex == "c42933273b78944b2aab3e7bafa21d5da976d162d69904ded282c6c540f26b2e", "hmac-sha256-integer-lists")
   mut long_key = bytes(131)
   mut i = 0
   while i < 131 {
      store8(long_key, 170, i)
      i += 1
   }
   assert(hash.sha256_hmac(long_key, "Test Using Larger Than Block-Size Key - Hash Key First").hex == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "hmac-sha256-long-key")
   assert(hash.pbkdf2_sha256("password".to_bytes, "salt".to_bytes, 1).hex == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b", "pbkdf2-sha256-one-iteration")
   assert(hash.pbkdf2_sha256("password".to_bytes, "salt".to_bytes, 2).hex == "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43", "pbkdf2-sha256-two-iterations")
}

fn test_unix_crypt_vectors() {
   assert(crypt.crypt_des("password", "ab") == "abJnggxhB/yWI", "des-password")
   assert(crypt.crypt_des("", "xx") == "xx95eIq3U1EUg", "des-empty")
   assert(crypt.crypt_md5("password", "$1$salt") == "$1$salt$qJH7.N4xYta3aEG/dfqo/0", "md5-password")
   assert(crypt.crypt_md5("", "$1$salt") == "$1$salt$UsdFqFVB.FsuinRDK5eE..", "md5-empty")
   assert(crypt.crypt_md5("password", "$1$1234567890") == "$1$12345678$o2n/JiO/h5VviOInWJ4OQ/", "md5-salt-trunc")
   assert(crypt.crypt_sha256("password", "$5$salt") == "$5$salt$Gcm6FsVtF/Qa77ZKD.iwsJlCVPY0XSMgLJL0Hnww/c1", "sha256-password")
   assert(crypt.crypt_sha256("", "$5$salt") == "$5$salt$HrcUzzoef72uxM/YhTU5BAi419Fblqlq//zyM.rIOG0", "sha256-empty")
   assert(crypt.crypt_sha512("password", "$6$salt") == "$6$salt$IxDD3jeSOb5eB1CX5LBsqZFVkJdido3OUILO5Ifz5iwMuTS4XMS130MTSuDDl3aCI6WouIL9AjRbLCelDCy.g.", "sha512-password")
   assert(crypt.crypt_sha512("", "$6$salt") == "$6$salt$r6qPcj2UeIkfklWHvleGJk8OKTInFYR/fxyuwcC656IWiZBpIFZ9.hMRG2ZQnnyMFrKOe461f9iT9Ljn0wJ5l.", "sha512-empty")
   assert(crypt.crypt_sha256("password", "$5$rounds=1000$salt") == "$5$rounds=1000$salt$p.wiWs2zrZ7irikO2AL64QDIJo00A3KDq2xWHpLJGgB", "sha256-rounds")
   assert(crypt.crypt_sha512("password", "$6$rounds=10000$salt") == "$6$rounds=10000$salt$dE5fLfpn2uXfkz.eouwYK/BjrHRu.piovQPjwlE06fDJHwMlg2l.IqEBUIfWBzf7YPXOAddB3FM7rnXHHKVNt.", "sha512-rounds")
   assert(crypt.crypt_sha512("password", "$6$longsalt0123456789") == "$6$longsalt01234567$7VT0MHBTsGu2OmxDDaLth3ZoH.4PEIpJ69CSAnswJoL4uiZBywVD7tSxI6ZBdAvUOQ.zwl6eXZFp1wnFlux5W/", "sha512-salt-trunc")
   assert(crypt.crypt_hash("password", "$7$salt") == "", "unknown scheme")
   assert(crypt.crypt_des("password", "?") == "", "bad DES salt")
   assert(unix_crypt_hash("password", "ab") == "abJnggxhB/yWI", "dictionary DES")
   assert(unix_crypt_hash("password", "$1$salt") == "$1$salt$qJH7.N4xYta3aEG/dfqo/0", "dictionary MD5")
}

#main {
   test_sha256_digest_spans()
   test_unix_crypt_vectors()
   print("crypt vectors ok")
}
