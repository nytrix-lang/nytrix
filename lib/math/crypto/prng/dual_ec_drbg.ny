;; Keywords: prng dual-ec-drbg math crypto
;; PRNG analysis routines for Dual EC DRBG prediction and backdoor recovery.
;; Exploit the NIST SP 800-90A backdoor where P = d*Q
;; Reference:
;; - https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-90Ar1.pdf
;; - https://rump2007.cr.yp.to/15-shumow.pdf
;; References:
;; - std.math.crypto.prng
;; - std.math.crypto
module std.math.crypto.prng.dual_ec_drbg(dual_ec_drbg_predict, dual_ec_backdoor, dual_ec_backdoor_recover_tail)
use std.math.nt
use std.math.crypto.ecc.ecc (ecc_scalar_mult, ecc_sqrt_mod)

fn dual_ec_backdoor(list Q, any d, any p) any {
   "Compute the backdoor point P for Dual_EC_DRBG given Q and secret scalar d.
   The backdoor relationship is P = d * Q on the elliptic curve over Fp.
   Q: point [x, y] on the curve
   d: secret scalar(the backdoor key)
   p: prime modulus of the field
   Returns the point P = d * Q as [x, y] or nil if inputs are invalid."
   def qx, qy = Q.get(0), Q.get(1)
   if d == 0 { return [0, 0] }
   def P = _ec_scalar_mult(d, qx, qy, p)
   P
}

fn dual_ec_backdoor_recover_tail(any observed, any d, list Q, any curve_a, any curve_b, any p, int full_x_bytes=32, int emitted_bytes=30, int check_bytes=2, int tail_bytes=28, any high_start=0, any high_stop=nil) any {
   "Recover a future Dual_EC truncated-output tail from one concatenated output window.
   `observed` is emitted_bytes from x(sQ) followed by check_bytes from the next emitted block.
   The helper brute-forces the missing high bytes of x(sQ), applies the backdoor scalar d,
   and verifies the next-output prefix before returning the requested next-output tail.
   `high_start` and `high_stop` optionally bound the missing-high-byte interval for replaying
   preserved transcripts without searching the full truncation space."
   def pp = Z(p)
   def missing_bytes = full_x_bytes - emitted_bytes
   assert(missing_bytes >= 0 && check_bytes <= emitted_bytes, "valid Dual_EC truncation sizes")
   def emitted_bits = 8 * emitted_bytes
   def missing_bits = 8 * missing_bytes
   def check_bits = 8 * check_bytes
   def tail_bits = 8 * tail_bytes
   def observed_z = Z(observed)
   def current_low = observed_z >> check_bits
   def next_check = observed_z & ((Z(1) << check_bits) - Z(1))
   def full_limit = Z(1) << missing_bits
   mut hi = max(Z(0), Z(high_start))
   def limit = min((high_stop == nil) ? full_limit : Z(high_stop), full_limit)
   while hi < limit {
      def x = current_low + (hi << emitted_bits)
      def y2 = mod(mod_mul(x, mod_mul(x, x, pp), pp) + mod_mul(curve_a, x, pp) + Z(curve_b), pp)
      def y = ecc_sqrt_mod(y2, pp)
      if y >= Z(0) {
         def A = ecc_scalar_mult(d, [x, y], curve_a, pp)
         if A != nil {
            def state = A[0]
            def B = ecc_scalar_mult(state, Q, curve_a, pp)
            if B != nil {
               def next_low = B[0] & ((Z(1) << emitted_bits) - Z(1))
               def prefix = next_low >> (emitted_bits - check_bits)
               if prefix == next_check { return next_low & ((Z(1) << tail_bits) - Z(1)) }
            }
         }
      }
      hi += Z(1)
   }
   nil
}

fn dual_ec_drbg_predict(list outputs, list P, list Q, any p) any {
   "Predict future Dual_EC_DRBG outputs given observed outputs and curve points.
   Given outputs [s1, s2, ...] and the backdoor relationship P = d*Q,
   recover the internal state and predict the next output.
   outputs: list of previously observed output x-coordinates
   P: the generator point [x, y]
   Q: the backdoor-related point [x, y]
   p: prime modulus
   Returns the predicted next output x-coordinate, or -1 if prediction fails."
   def n = outputs.len
   if n < 1 { return -1 }
   def last_output = outputs.get(n - 1)
   def qx = Q.get(0)
   def qy = Q.get(1)
   def px = P.get(0)
   def py = P.get(1)
   mut state_x = last_output
   mut found_y = false
   mut state_y = 0
   def y_sq = (state_x * state_x * state_x + state_x) % p
   def y_cand = tonelli_shanks(y_sq, p)
   if y_cand >= 0 {
      def test_pt = _ec_point_add(px, py, qx, qy, p)
      if test_pt.get(0) == state_x {
         state_y = qy
         found_y = true
      }
   }
   if !found_y && y_cand >= 0 {
      def y2 = (p - y_cand) % p
      def test_pt2 = _ec_point_add(px, py, qx, y2, p)
      if test_pt2.get(0) == state_x {
         state_y = y2
         found_y = true
      }
   }
   if !found_y {
      mut trial = 0
      while trial < 2 {
         def ty = (trial == 0) ? y_cand : ((p - y_cand) % p)
         if ty >= 0 {
            def test_pt = _ec_point_add(state_x, ty, qx, qy, p)
            def tx = test_pt.get(0)
            def ty2 = test_pt.get(1)
            def verify = _ec_point_add(tx, ty2, px, py, p)
            if verify.get(0) == state_x {
               state_y = ty
               found_y = true
               trial = 2
            }
         }
         trial += 1
      }
   }
   if !found_y { return -1 }
   def next_state = _ec_point_add(state_x, state_y, qx, qy, p)
   next_state.get(0)
}

fn _ec_scalar_mult(any k, any px, any py, any p) list {
   "Internal: Multiply point P by scalar k using double-and-add on curve y^2 = x^3 + x over Fp.
   Returns the resulting point [x, y]."
   if k == 0 { return [0, 0] }
   mut qx, qy = 0, 0
   mut rx, ry = px, py
   mut has_result = false
   mut kb = k
   while kb > 0 {
      if kb & 1 != 0 {
         if has_result {
            def sum = _ec_point_add(qx, qy, rx, ry, p)
            qx, qy = sum.get(0), sum.get(1)
         } else {
            qx, qy = rx, ry
            has_result = true
         }
      }
      def dbl = _ec_point_add(rx, ry, rx, ry, p)
      rx, ry = dbl.get(0), dbl.get(1)
      kb = kb >> 1
   }
   if has_result { [qx, qy] } else { [0, 0] }
}

fn _ec_point_add(any x1, any y1, any x2, any y2, any p) list {
   "Internal: Add two points on y^2 = x^3 + x over Fp(curve with a=1, b=0).
   Returns the resulting point [x3, y3]."
   if x1 == 0 && y1 == 0 { return [x2, y2] }
   if x2 == 0 && y2 == 0 { return [x1, y1] }
   if x1 == x2 {
      def y_sum = (y1 + y2) % p
      if y_sum == 0 { return [0, 0] }
      def num = (3 * x1 * x1 + 1) % p
      def den = (2 * y1) % p
      def inv_den = inverse_mod(den, p)
      def lam = (num * inv_den) % p
      def x3 = (lam * lam - x1 - x2) % p
      def y3 = (lam * (x1 - x3) - y1) % p
      return [x3, y3]
   }
   def num = (y2 - y1) % p
   def den = (x2 - x1) % p
   def inv_den = inverse_mod(den, p)
   def lam = (num * inv_den) % p
   def x3 = (lam * lam - x1 - x2) % p
   def y3 = (lam * (x1 - x3) - y1) % p
   [x3, y3]
}

#main {
   def p = 23
   def Q = [1, 5]
   def P = dual_ec_backdoor(Q, 5, p)
   assert(P == [1, 5], "dual ec backdoor point")
   assert(dual_ec_drbg_predict([1], P, Q, p) == 0, "dual ec prediction")
   assert(dual_ec_drbg_predict([], P, Q, p) == -1, "empty outputs")
   assert(dual_ec_backdoor([1, 1], 0, 7) == [0, 0], "zero scalar")
   print("✓ std.math.crypto.prng.dual_ec_drbg self-test passed")
}
