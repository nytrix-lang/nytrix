;; Keywords: ecc identification math crypto public-key
;; Elliptic-curve routines for identification protocol analysis.
;; Reference:
;; - Benjamin Smith, ECC school notes on identification with ephemeral keys
;; References:
;; - std.math.crypto.ecc
;; - std.math.crypto
module std.math.crypto.ecc.identification(ecc_ident_public_key, ecc_ident_ephemeral_commit, ecc_ident_response, ecc_ident_verify, ecc_ident_verify_strict, ecc_ident_forge, ecc_ident_detect_cheating)
use std.math.crypto.ecc.ecc
use std.math.nt

fn ecc_ident_public_key(number x, list P, number a, number p, any n=nil) any {
   "Compute public key Q = x*P for the identification protocol."
   ecc_scalar_mult(x, P, a, p, n)
}

fn ecc_ident_ephemeral_commit(number r, list P, number a, number p, any n=nil) any {
   "Compute ephemeral commitment R = r*P."
   ecc_scalar_mult(r, P, a, p, n)
}

fn ecc_ident_response(number x, number r, any n=nil) number {
   "Compute s = x + r(or mod n if subgroup order is known)."
   if(n == nil){ return x + r }
   mod(x + r, n)
}

fn ecc_ident_verify(list P, list Q, list R, number s, number a, number p, any n=nil) bool {
   "Verify s*P == Q + R."
   def lhs, rhs = ecc_scalar_mult(s, P, a, p, n), ecc_point_add(Q, R, a, p)
   if(lhs == nil || rhs == nil){ return false }
   lhs[0] == rhs[0] && lhs[1] == rhs[1]
}

fn _ecc_ident_point_strict_ok(any point, number a, number b, number p, any n=nil, any P=nil) bool {
   if(point == nil || !is_list(point) || point.len < 2){ return false }
   if(!ecc_is_on_curve(point, a, b, p)){ return false }
   if(n != nil && P != nil){
      def check = ecc_scalar_mult(n, point, a, p, n)
      if(check != nil){ return false }
      if(point[0] < 0 || point[0] >= n || point[1] < 0 || point[1] >= n){ return false }
   }
   true
}

fn ecc_ident_verify_strict(list P, list Q, list R, number s, number a, number b, number p, any n=nil) bool {
   "Strict EC-auth verification with curve and subgroup checks, matching crypton-style EC-Auth servers."
   if(!_ecc_ident_point_strict_ok(P, a, b, p, n, P)){ return false }
   if(!_ecc_ident_point_strict_ok(Q, a, b, p, n, P)){ return false }
   if(!_ecc_ident_point_strict_ok(R, a, b, p, n, P)){ return false }
   ecc_ident_verify(P, Q, R, s, a, p, n)
}

fn ecc_ident_forge(list P, list Q, number s, number a, number p, any n=nil) any {
   "Forge a valid-looking identification transcript without knowing x.
   Choose s, then set R = s*P - Q."
   def sP = ecc_scalar_mult(s, P, a, p, n)
   if(sP == nil){ return nil }
   ecc_sub(sP, Q, a, p)
}

fn ecc_ident_detect_cheating(list P, list Q, list R, number s, number r, number a, number p, any n=nil) bool {
   "Check both verification equation and that R really equals r*P.
   This detects the trivial forgery that only picks s and derives R."
   if(!ecc_ident_verify(P, Q, R, s, a, p, n)){ return false }
   def expect_R = ecc_scalar_mult(r, P, a, p, n)
   if(expect_R == nil || R == nil){ return false }
   expect_R[0] == R[0] && expect_R[1] == R[1]
}
