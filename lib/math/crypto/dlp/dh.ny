;; Keywords: dlp discrete-log group-theory dh
;; Discrete-log routines for Diffie-Hellman group checks and shared-secret recovery.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap12.pdf
module std.math.crypto.dlp.dh(dh_public_key, dh_private_key, dh_keygen, dh_derive, dh_default_group)
use std.math.nt

fn dh_public_key(any: h, any: p, any: g, any: q=nil): list {
   "Create a Diffie-Hellman public key tuple [h, p, g, q]."
   [Z(h), Z(p), Z(g), q == nil ? Z(p) - Z(1) : Z(q)]
}

fn dh_private_key(any: x, any: p, any: g, any: q=nil): list {
   "Create a Diffie-Hellman private key tuple [x, p, g, q]."
   [Z(x), Z(p), Z(g), q == nil ? Z(p) - Z(1) : Z(q)]
}

fn dh_default_group(): list {
   "Return a small validation group [p, g, q]. Not for production use."
   def p, g = (Z(1) << Z(1024)) - Z(1093337), Z(7)
   [p, g, p - Z(1)]
}

fn dh_keygen(any: p=nil, any: g=nil, any: q=nil, any: x=nil): list {
   "Generate a Diffie-Hellman [public, private] pair. x may be supplied for deterministic tests."
   if(p == nil || g == nil){
      def group = dh_default_group()
      p, g = p == nil ? group[0] : p, g == nil ? group[1] : g
      q = q == nil ? group[2] : q
   }
   if(q == nil){ q = Z(p) - Z(1) }
   if(x == nil){ x = randint(Z(2), Z(p) - Z(2)) }
   def h = power_mod(g, x, p)
   [dh_public_key(h, p, g, q), dh_private_key(x, p, g, q)]
}

fn dh_derive(list: pubkey, any: x_or_privkey): any {
   "Derive the shared secret h^x mod p from a public key and private exponent/key."
   def h, p = pubkey[0], pubkey[1]
   def x = is_list(x_or_privkey) ? x_or_privkey[0] : x_or_privkey
   power_mod(h, x, p)
}
