;; Keywords: crypto encoding hash hmac aes des chacha20 salsa20 rsa ecc ecdsa lattice lll cvp factorization fermat pollard ecm prng mt19937 lcg analysis side-channel math
;; Crypto facade: encodings, hashes, symmetric ciphers, RSA/ECC helpers,
;; lattice/factorization helpers, PRNGs, and text/side-channel analysis.
;; References:
;; - std.math
module std.math.crypto(analysis, block, cipher, ct, dlp, ecc, encoding, factorization, hash, hnp, lattice, number, prng, protocol, public_key, rsa, support, symmetric)
