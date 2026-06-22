;; Keywords: stream-cipher ctr otp rc4 keystream xor hamming-distance english-score crib-drag math crypto block-cipher
;; Stream-cipher facade for CTR helpers, OTP, RC4, keystream scoring, and bytewise XOR workflows.
;; References:
;; - std.math.crypto.block
;; - std.math.crypto
module std.math.crypto.block.stream(core, otp, rc4, ctr_xor_plaintexts, ctr_recover_keystream, ctr_bit_flip_byte, ctr_bit_flipping, ctr_score_english_byte, ctr_recover_periodic_keystream_english, ctr_apply_periodic_keystream, mtp_xor_all, mtp_guess_key_byte, mtp_crib_drag, otp_reuse_attack, otp_decrypt_known_plaintext, otp_hamming_distance, otp_guess_key_sizes, otp_score_english, otp_recover_reused_key, otp_apply_key, rc4_ksa, rc4_prga, rc4_decrypt_known_key, otp_timestamp_sha256_key, otp_timestamp_sha256_xor, otp_timestamp_sha256_bruteforce)
#main {
   def key = [42, 17, 99, 42, 17, 99]
   def plain = "hello".to_bytes
   def ct = otp_apply_key(plain, key)
   assert(otp_apply_key(ct, key) == plain, "otp apply key roundtrip")
   assert(otp_score_english("the quick brown fox".to_bytes) > 0, "otp score english")
   print("✓ std.math.crypto.block.stream self-test passed")
}
