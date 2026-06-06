;; Keywords: public-key rsa ecc dlp dh diffie-hellman blum-goldwasser key-exchange encryption math crypto
;; Public-key facade for RSA, ECC, DLP/DH, and Blum-Goldwasser entrypoints.
;;
;; families still live in their focused modules (`rsa`, `ecc`, `dlp`) so old
;; imports stay stable; this module gives users one public-key entrypoint.
;; References:
;; - std.math.crypto
module std.math.crypto.public_key(rsa, ecc, dlp, dh_public_key, dh_private_key, dh_keygen, dh_derive, dh_default_group, blum_goldwasser)
use std.math.crypto.dlp.dh
