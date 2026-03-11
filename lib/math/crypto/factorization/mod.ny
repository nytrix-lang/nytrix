;; Keywords: factorization integer-factorization fermat pollard pminus1 ecm primality known-primes chunked-primes quadratic-form nfs classical
;; Integer-factorization facade for classical methods, Fermat, Pollard, ECM, primality, and known-prime helpers.
;; References:
;; - https://cacr.uwaterloo.ca/hac/about/chap8.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.factorization(fermat, pollard, known_phi, twin_primes, roca, unbalanced, xor_factor, hybrid_factor, complex_multiplication, gaa, branch_and_prune, implicit, base_conversion, shor, fixed_sum, special_forms, sequence_gcd, classical, known_primes, ecm, primality, aprcl_data, nfs, chunked_primes, quadratic_form)
