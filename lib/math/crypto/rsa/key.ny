;; Keywords: rsa key math crypto
;; RSA key parsing and conversion routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.key(rsa_public_pem_values, rsa_public_pkcs1_der, rsa_public_pkcs1_pem, rsa_private_pem_values, rsa_openssh_public_values)
use std.core
use std.math.crypto.encoding.encoding (asn1_parse, asn1_get_integer, asn1_get_sequence, pem_decode)
use std.math.nt
use std.core.str

fn _asn1_int_at(list seq, int idx) any {
   asn1_get_integer(seq[idx])
}

fn _der_len(any n) list {
   if(n < 128){ return [n] }
   def b = Z(n).bytes
   [128 | b.len].concat(b)
}

fn _der_int(any x) list {
   mut b = Z(x).bytes
   if(b.len == 0){ b = [0] }
   if((b[0] & 128) != 0){ b = [0].concat(b) }
   [2].concat(_der_len(b.len)).concat(b)
}

fn _der_seq(list parts) list {
   mut body = []
   mut i = 0
   while(i < parts.len){
      body = body.concat(parts[i])
      i += 1
   }
   [48].concat(_der_len(body.len)).concat(body)
}

fn _pem_wrap(str label, list der) str {
   def b64 = der.base64
   mut out = Builder(b64.len + 80)
   out = builder_append(out, "-----BEGIN " + label + "-----\n")
   mut i = 0
   while(i < b64.len){
      out = builder_append(out, slice(b64, i, min(i + 64, b64.len)))
      out = builder_append(out, "\n")
      i += 64
   }
   out = builder_append(out, "-----END " + label + "-----\n")
   def s = builder_to_str(out)
   builder_free(out)
   s
}

fn rsa_public_pkcs1_der(any n, any e=65537) list {
   "Build PKCS#1 RSAPublicKey DER from modulus n and public exponent e."
   _der_seq([_der_int(n), _der_int(e)])
}

fn rsa_public_pkcs1_pem(any n, any e=65537) str {
   "Build PKCS#1 RSA PUBLIC KEY PEM from modulus n and public exponent e."
   _pem_wrap("RSA PUBLIC KEY", rsa_public_pkcs1_der(n, e))
}

fn rsa_public_pem_values(str pem) list {
   "Parse a SubjectPublicKeyInfo or PKCS#1 RSA PUBLIC KEY PEM and return [n, e]."
   def top = asn1_get_sequence(asn1_parse(pem_decode(pem))[0])
   assert(top != nil && top.len >= 2, "RSA public key ASN.1 sequence")
   if(top[0]["tag"] == 0x02){
      return [_asn1_int_at(top, 0), _asn1_int_at(top, 1)]
   }
   def bit_value = top[1]["val"]
   def rsa_seq = asn1_get_sequence(asn1_parse(slice(bit_value, 1, bit_value.len))[0])
   assert(rsa_seq != nil && rsa_seq.len >= 2, "RSA public key integers")
   [_asn1_int_at(rsa_seq, 0), _asn1_int_at(rsa_seq, 1)]
}

fn rsa_private_pem_values(str pem) list {
   "Parse a PKCS#1 or PKCS#8 RSA private key PEM and return [n, e, d, p, q, dp, dq, qi]."
   def top = asn1_get_sequence(asn1_parse(pem_decode(pem))[0])
   assert(top != nil, "RSA private key ASN.1 top sequence")
   def seq = (top.len >= 3 && top[2].get("tag") == 0x04) ? asn1_get_sequence(asn1_parse(top[2]["val"])[0]) : top
   assert(seq != nil && seq.len >= 9, "RSA private key ASN.1 sequence")
   [
      _asn1_int_at(seq, 1),
      _asn1_int_at(seq, 2),
      _asn1_int_at(seq, 3),
      _asn1_int_at(seq, 4),
      _asn1_int_at(seq, 5),
      _asn1_int_at(seq, 6),
      _asn1_int_at(seq, 7),
      _asn1_int_at(seq, 8),
   ]
}

fn _ssh_u32(list blob, int pos) int {
   (blob[pos] << 24) | (blob[pos + 1] << 16) | (blob[pos + 2] << 8) | blob[pos + 3]
}

fn _ssh_mpint(list blob, int pos) list {
   def n = _ssh_u32(blob, pos)
   def start = pos + 4
   mut i = start
   while(i < start + n && blob[i] == 0){ i += 1 }
   mut out = Z(0)
   while(i < start + n){
      out = out * Z(256) + Z(blob[i])
      i += 1
   }
   [out, start + n]
}

fn rsa_openssh_public_values(str key) list {
   "Parse an OpenSSH ssh-rsa public key and return [n, e]."
   def parts = split(strip(key), " ")
   assert(parts.len >= 2 && parts[0] == "ssh-rsa", "OpenSSH RSA public key")
   def blob = parts[1].base64_decode
   mut pos = 0
   def kind_len = _ssh_u32(blob, pos)
   pos = pos + 4 + kind_len
   def e_res = _ssh_mpint(blob, pos)
   pos = e_res[1]
   def n_res = _ssh_mpint(blob, pos)
   [n_res[0], e_res[0]]
}
