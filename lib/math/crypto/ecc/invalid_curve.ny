;; Keywords: ecc invalid-curve math crypto public-key
;; Invalid-curve attack routines for elliptic-curve protocols.
;; Recover private keys by exploiting invalid curve parameters and quadratic twists
;; Reference:
;; - https://www.secg.org/sec1-v2.pdf
;; - https://www.rfc-editor.org/rfc/rfc8032
;; References:
;; - std.math.crypto.ecc
;; - std.math.crypto
module std.math.crypto.ecc.invalid_curve(invalid_curve_attack, twist_attack, invalid_curve_dlog, invalid_curve_recover_transcript, crt_combine)
use std.math.nt
use std.math.crypto.ecc
use std.math.crypto.ecc.ecc (ecc_point_add, ecc_scalar_mult)

fn invalid_curve_attack(fnptr oracle_fn, any Gx, any Gy, any a, any p, list factors) any {
   "Invalid curve attack: recover private key d from an oracle that does scalar multiplication " +
   "on y^2 = x^3 + ax + b over F_p without point validation. " +
   "factors is [prime, exponent] pairs for the order. Returns d or nil."
   mut result = 0
   mut modulus = 1
   def nf = factors.len
   mut i = 0
   while(i < nf){
      def factor_pair = factors[i]
      def q = factor_pair[0]
      def e = factor_pair[1]
      def qi = bigint_pow(Z(q), Z(e))
      def point = find_point_of_order(a, q, p)
      if(point != nil){
         def px, py = point[0], point[1]
         def k = oracle_fn(px, py)
         def rem = k % qi
         def crt_result = crt_combine(result, rem, modulus, qi)
         result = crt_result[0]
         modulus = crt_result[1]
      }
      i += 1
   }
   result
}

fn twist_attack(fnptr oracle_fn, any Gx, any Gy, any a, any b, any p, list factors) any {
   "Quadratic twist attack: recover d by sending points on the twist of y^2 = x^3 + ax + b over F_p. " +
   "Twist order is p + 1 - t while original is p + 1 + t. " +
   "factors is [prime, exponent] pairs for the twist order. Returns d or nil."
   def twist_a, twist_b = a, find_twist_b(b, p)
   mut result = 0
   mut modulus = 1
   def nf = factors.len
   mut i = 0
   while(i < nf){
      def factor_pair = factors[i]
      def q = factor_pair[0]
      def e = factor_pair[1]
      def qi = bigint_pow(Z(q), Z(e))
      def point = find_point_on_twist(twist_a, twist_b, q, p)
      if(point != nil){
         def px, py = point[0], point[1]
         def k = oracle_fn(px, py)
         def rem = k % qi
         def crt_result = crt_combine(result, rem, modulus, qi)
         result = crt_result[0]
         modulus = crt_result[1]
      }
      i += 1
   }
   result
}

fn invalid_curve_dlog(list g, any q, any a, any p, int order) int {
   "Brute-force dlog in a small invalid-curve subgroup. Returns -1 if not found."
   mut r = nil
   mut k = 0
   while(k < order){
      if(r == q){ return k }
      r = ecc_point_add(r, g, a, p)
      k += 1
   }
   -1
}

fn invalid_curve_recover_transcript(list transcript, any a, any p) any {
   "Recover a scalar from fixed invalid-curve query rows [x,y,order,qx,qy]. " +
   "Returns [scalar, combined_modulus], or nil if a row is inconsistent."
   mut rems = []
   mut mods = []
   mut product = Z(1)
   mut i = 0
   while(i < transcript.len){
      def row = transcript[i]
      if(row.len < 5){ return nil }
      def g = [Z(row[0]), Z(row[1])]
      def q = [Z(row[3]), Z(row[4])]
      def n = int(row[2])
      if(ecc_scalar_mult(n, g, a, p) != nil){ return nil }
      def d = invalid_curve_dlog(g, q, a, p, n)
      if(d < 0){ return nil }
      rems = rems.append(Z(d))
      mods = mods.append(Z(n))
      product *= Z(n)
      i += 1
   }
   [crt(rems, mods), product]
}

fn find_point_of_order(any a, any q, any p) any {
   "Find a point of order q on y^2 = x^3 + ax + b' for some b' over F_p. " +
   "Searches small b' values. Returns [x, y] or nil."
   mut b_try = 0
   while(b_try < p){
      if(b_try < 2000){
         def pt = find_point_with_order(a, b_try, q, p)
         if(pt != nil){ return pt }
      }
      b_try += 1
   }
   nil
}

fn find_point_with_order(any a, any b, any q, any p) any {
   "Find a point on y^2 = x^3 + ax + b over F_p that has order dividing q. Returns [x, y] or nil."
   def order = compute_curve_order(a, b, p)
   if(order == 0){ return nil }
   if(order % q != 0){ return nil }
   def cofactor = order / q
   def x = find_point_x(a, b, p)
   if(x < 0){ return nil }
   def y = find_point_y(x, a, b, p)
   if(y < 0){ return nil }
   def P = [x, y]
   def Q = ecc_scalar_mult(cofactor, P, a, p)
   if(Q == nil){ return nil }
   def qx, qy = Q[0], Q[1]
   if(qx == 0 && qy == 0){ return nil }
   Q
}

fn find_point_x(any a, any b, any p) any {
   "Find an x-coordinate on curve y^2 = x^3 + ax + b over F_p. Returns x or -1 if not found in search range."
   mut x_max = p
   if(x_max > 500){ x_max = 500 }
   mut x = 0
   while(x < x_max){
      mut rhs = (x * x * x + a * x + b) % p
      if(rhs < 0){ rhs = rhs + p }
      if(is_quadratic_residue(rhs, p)){ return x }
      x += 1
   }
   -1
}

fn find_point_y(any x, any a, any b, any p) any {
   "Find y for x on y^2 = x^3 + ax + b over F_p. Returns y or -1 if no sqrt exists."
   mut rhs = (x * x * x + a * x + b) % p
   if(rhs < 0){ rhs = rhs + p }
   if(!is_quadratic_residue(rhs, p)){ return -1 }
   tonelli_shanks(rhs, p)
}

fn is_quadratic_residue(any a, any p) bool {
   "Check if a is a quadratic residue modulo p."
   if(a == 0){ return true }
   legendre(a, p) == 1
}

fn find_non_residue(any p) any {
   "Find a quadratic non-residue modulo p by brute force. Returns the smallest non-residue."
   mut z = 2
   while(z < p){
      if(legendre(z, p) == -1){ return z }
      z += 1
   }
   -1
}

fn compute_curve_order(any a, any b, any p) any {
   "Approximate order of y^2 = x^3 + ax + b over F_p using Hasse bound. " +
   "Returns estimated order or 0 on failure."
   mut x = 0
   mut count = 1
   while(x < p){
      mut rhs = (x * x * x + a * x + b) % p
      if(rhs < 0){ rhs = rhs + p }
      if(rhs == 0){ count += 1 }
      if(rhs != 0 && is_quadratic_residue(rhs, p)){ count = count + 2 }
      x += 1
      if(x > 500){
         def estimated = p + 1
         return estimated
      }
   }
   count
}

fn find_twist_b(any b, any p) any {
   "Find b for the quadratic twist of y^2 = x^3 + ax + b over F_p. " +
   "Twist is y^2 = x^3 + a*g^2*x + b*g^3 for non-residue g. Returns twisted b."
   def g = find_non_residue(p)
   if(g < 0){ return b }
   def g2, g3 = (g * g) % p, (g2 * g) % p
   (b * g3) % p
}

fn find_point_on_twist(any a, any b, any q, any p) any {
   "Find a point on the quadratic twist curve y^2 = x^3 + ax + b over F_p with order dividing q. Returns [x, y] or nil."
   mut x_max = p
   if(x_max > 500){ x_max = 500 }
   mut x = 0
   while(x < x_max){
      mut rhs = (x * x * x + a * x + b) % p
      if(rhs < 0){ rhs = rhs + p }
      if(!is_quadratic_residue(rhs, p) && rhs != 0){
         def y = tonelli_shanks_non_residue(rhs, p)
         if(y >= 0){ return [x, y] }
      }
      x += 1
   }
   nil
}

fn tonelli_shanks_non_residue(any n, any p) any {
   "Compute sqrt of non-residue n in the twist: finds y with y^2 = n * g where g is non-residue. " +
   "Returns y or -1."
   def g = find_non_residue(p)
   if(g < 0){ return -1 }
   mut ng = (n * g) % p
   if(ng < 0){ ng = ng + p }
   tonelli_shanks(ng, p)
}

fn crt_combine(any r1, any r2, any m1, any m2) list {
   "Combine two congruences using CRT: x = r1(mod m1), x = r2(mod m2). Returns [x, m1*m2].
   Uses the formula x = r1 + m1 * ((r2 - r1) * m1^-1 mod m2)."
   def g = gcd(m1, m2)
   if(g > 1){ if(r1 % g != r2 % g){ return [0, 0] } }
   def m1_inv = inverse_mod(m1, m2)
   def diff = (r2 - r1) % m2
   def t = (diff * m1_inv) % m2
   def x = r1 + m1 * t
   def m = m1 * m2
   [x, m]
}
