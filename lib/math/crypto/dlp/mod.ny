;; Keywords: dlp discrete-log group-theory diffie-hellman dh dsa baby-step-giant-step bsgs pohlig-hellman pollard-rho clock math crypto
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap3.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap5.pdf
;; References:
;; - std.math.crypto
module std.math.crypto.dlp(multiplicative_order_from_factors, multiplicative_order_factorization, baby_step_giant_step, pohlig_hellman, pohlig_hellman_bounded, ph_prime_power, ph_recombine, pohlig_hellman_prime_power, pohlig_hellman_recombine, pollard_rho_dlp, dlp_brute_force, solve_dlp, dh_small_subgroup_element, dh_small_subgroup_confinement, dh_public_key, dh_private_key, dh_keygen, dh_derive, dh_default_group, dsa_sign_hash, dsa_verify_hash, dsa_recover_key_from_nonce, dsa_recover_nonce_reuse, dsa_recover_nonce_lcg_two_sigs, dsa_nonce_polynomial_value, dsa_verify_nonce_polynomial, dsa_recover_key_from_nonce_polynomial, dsa_recover_key_from_quadratic_nonces, dsa_verify_hash_or_bounds_zero_inverse_bug, dsa_zero_inverse_bypass_signature, dsa, clock_identity, clock_add, clock_sub, clock_neg, clock_scalar_mult, clock_on_curve, clock_recover_modulus, clock_baby_step_giant_step, clock_pohlig_hellman)
use std.math.crypto.dlp.dlp
use std.math.crypto.dlp.dh
use std.math.crypto.dlp.dsa
use std.math.crypto.dlp.subgroup_confinement
use std.math.crypto.dlp.clock
