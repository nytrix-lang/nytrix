;; Keywords: number-theory modular-arithmetic crt hensel hensel-lifting lucas gf2 recurrence pseudoprime primitive-root roots-of-unity partial-integer math crypto
;; Crypto number-theory facade for CRT, GF(2), recurrences, partial integers, and arithmetic.
;; References:
;; - std.math.crypto
module std.math.crypto.number(arith, crt, gf2, hensel, lucas, partial, pseudoprimes, recurrence, int_to_bits_le, bits_to_int_le, floor_div, ceil_div, square_root_or_nil, symmetric_mod, factor_divisors, make_square_free, largest_prime_factor, primitive_pythagorean_triple_for_area, modinv_range, modinv_list, roots_of_unity_mod_prime, rth_roots_mod_prime, least_significant_bits, two_adic_valuation, mod_sqrt_power2, is_blum_prime, has_blum_prime, random_blum_prime, fast_crt, poly_eval_mod, hensel_lift_linear, hensel_roots, lucas_uv_mod, lucas_u_mod, lucas_v_mod, lucas_encrypt, lucas_apply_exponent_chain, lucas_private_modulus_from_factors, lucas_combine_exponent_chain, lucas_decrypt, lucas_decrypt_exponent_chain, partial_integer_new, partial_integer_add_known, partial_integer_add_unknown, partial_integer_known_lsb, partial_integer_known_msb, partial_integer_known_middle, partial_integer_unknown_lsb, partial_integer_unknown_msb, partial_integer_unknown_middle, partial_integer_matches, partial_integer_sub, partial_integer_known_and_unknowns, partial_integer_unknown_bounds, partial_integer_to_int, partial_integer_to_string_le, partial_integer_to_string_be, partial_integer_to_bits_le, partial_integer_to_bits_be, generate_pseudoprime, linear_recurrence_mod, gf2_solve, gf2_solve_full_rank)
use std.math.nt

#main {
   def roots = mod_sqrt_power2(Z(9), 8)
   assert(roots.len > 0, "mod_sqrt_power2 finds roots of 9 mod 256")
   def r0 = int(roots.get(0))
   assert((r0 * r0) % 256 == 9, "sqrt root squares to original mod 256")
   print("✓ math.crypto.number self-test passed")
}
