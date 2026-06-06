;; Keywords: ecc elliptic-curve ecdsa elgamal edwards montgomery secp curve-arithmetic point-add point-mul public-key math crypto
;; ECC facade for curve arithmetic, ECDSA, Edwards/Montgomery forms, and ElGamal helpers.
;; References:
;; - https://www.secg.org/sec1-v2.pdf
;; - https://www.rfc-editor.org/rfc/rfc8032
module std.math.crypto.ecc(ecc, ecdsa, elgamal, ecdlp, edwards, invalid_curve, singular_curve, smart_attack, identification, montgomery)
