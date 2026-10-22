;; Keywords: lattice howgrave-graham math crypto number-theory
;; Lattice routines for Howgrave-Graham lattice bounds and reductions.
;; Reference:
;; - https://link.springer.com/chapter/10.1007/3-540-48910-X_6
;; References:
;; - core.math.crypto.lattice
;; - core.math.crypto
module core.math.crypto.lattice.howgrave_graham(hg_modular_univariate)
use core.math.crypto.lattice.small_roots

fn hg_modular_univariate(any f, any N, any m, any t, any X, str reduction_method="ny") list {
   "Compute small modular roots of a univariate polynomial using the shared
   Howgrave-Graham lattice construction."
   modular_univariate(f, N, m, t, X, nil, reduction_method)
}
