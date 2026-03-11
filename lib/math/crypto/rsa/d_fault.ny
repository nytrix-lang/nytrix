;; Keywords: rsa d-fault
;; RSA private-exponent fault attack routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; Reference idea:
;; - bit flips during exponentiation leak bits of d through valid/faulty signatures
module std.math.crypto.rsa.d_fault(d_fault_bits, d_fault_attack)
use std.math.nt
use std.math.crypto.number.partial as partial

fn d_fault_bits(any: n, any: e, any: valid_sig, list: faulty_sigs): any {
   "Recover known bits of d from signatures generated with one-bit faults in d.
   Returns a PartialInteger in little-endian bit order."
   def bits_n = bit_length(Z(n))
   mut d_bits = []
   mut i = 0
   while(i < bits_n){
      d_bits = d_bits.append(nil)
      i += 1
   }
   def m = Z(2)
   mut mi = dict()
   mut idx = 0
   while(idx < bits_n){
      mi = mi.set(power_mod(m, Z(1) << Z(idx), Z(n)), idx)
      idx += 1
   }
   mut j = 0
   while(j < faulty_sigs.len){
      def sf = Z(faulty_sigs.get(j))
      def di0 = (Z(valid_sig).invmod(Z(n)) * sf) % Z(n)
      def di1 = (Z(valid_sig) * sf.invmod(Z(n))) % Z(n)
      if(mi.contains(di0)){ d_bits[mi.get(di0)] = 0 }
      if(mi.contains(di1)){ d_bits[mi.get(di1)] = 1 }
      j += 1
   }
   partial.partial_integer_from_bits_le(d_bits)
}

fn d_fault_attack(any: n, any: e, any: valid_sig, list: faulty_sigs): any {
   "Return a PartialInteger view of d from valid/faulty signatures."
   d_fault_bits(n, e, valid_sig, faulty_sigs)
}
