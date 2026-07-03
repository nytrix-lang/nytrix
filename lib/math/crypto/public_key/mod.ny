;; Keywords: public-key rsa ecc dlp dh diffie-hellman elgamal blum-goldwasser paillier rabin key-exchange encryption math crypto
;; Public-key facade for RSA, ECC, DLP/DH, ElGamal, Blum-Goldwasser, Paillier, and Rabin entrypoints.
;;
;; families still live in their focused modules (`rsa`, `ecc`, `dlp`) so old
;; imports stay stable; this module gives users one public-key entrypoint.
;; References:
;; - std.math.crypto
module std.math.crypto.public_key(rsa, ecc, dlp, dh_public_key, dh_private_key, dh_keygen, dh_derive, dh_default_group, elgamal_public_key, elgamal_private_key, elgamal_keygen, elgamal_encrypt, elgamal_decrypt, elgamal_derive_shared, elgamal_nonce_reuse, elgamal_recover_key_from_nonce_reuse, elgamal_key_nonce_reuse, elgamal_key_nonce_reuse_all, elgamal_recover_plaintext_from_nonce_reuse, elgamal_multiply_ciphertext, elgamal_unsafe_generator_leak, elgamal_sign, elgamal_verify, elgamal_signature_nonce_reuse, blum_goldwasser, paillier, paillier_keygen, paillier_encrypt, paillier_decrypt, paillier_encrypt_default, paillier_add, paillier_mul_plain, paillier_rerandomize, rabin, rabin_keygen, rabin_encrypt, rabin_valid_blum_ciphertext, rabin_principal_decrypt_blum, rabin_decrypt_candidates, rabin_select_candidate, rabin_decrypt_with_padding, rabin_sign_int, rabin_verify_int, rabin_sign, rabin_verify, damgard_jurik, damgard_jurik_keygen, damgard_jurik_encrypt, damgard_jurik_decrypt)
use std.math.crypto.dlp.dh
use std.math.crypto.ecc.elgamal
use std.math.crypto.public_key.paillier
use std.math.crypto.public_key.rabin
use std.math.crypto.public_key.damgard_jurik
