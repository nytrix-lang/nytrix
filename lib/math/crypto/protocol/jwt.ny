;; Keywords: protocol jwt
;; Protocol-analysis routines for JWT decoding, signing input, alg-none, and HS256 confusion checks.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc7519
module std.math.crypto.protocol.jwt(jwt_base64url_encode_bytes, jwt_base64url_encode_str, jwt_base64url_decode_bytes, jwt_base64url_decode_str, jwt_decode_header_json, jwt_decode_payload_json, jwt_decode_unverified, jwt_signing_input, jwt_alg_none_json, jwt_hs256_json, jwt_hs256_confusion_json, jwt_hs256_verify)
use std.core
use std.math.bin as bin
use std.math.crypto.hash (sha256_hmac)
use std.parse.data.json
use std.core.str as str

fn _strip_b64_padding(str: s): str {
   mut n = s.len
   while(n > 0 && slice(s, n - 1, n) == "="){ n -= 1 }
   slice(s, 0, n)
}

fn _b64url_to_b64(str: s): str {
   mut out = str.str_replace(str.str_replace(s, "-", "+"), "_", "/")
   def rem = out.len % 4
   if(rem == 2){
      out = out + "=="
   } elif(rem == 3){
      out = out + "="
   } elif(rem == 1){
      panic("invalid base64url length")
   }
   out
}

fn jwt_base64url_encode_bytes(list: data): str {
   "Encode bytes with unpadded RFC 7515 base64url."
   def b64 = data.base64
   _strip_b64_padding(str.str_replace(str.str_replace(b64, "+", "-"), "/", "_"))
}

fn jwt_base64url_encode_str(str: data): str {
   "Encode a string with unpadded RFC 7515 base64url."
   jwt_base64url_encode_bytes(data.to_bytes)
}

fn jwt_base64url_decode_bytes(str: data): list {
   "Decode unpadded RFC 7515 base64url to bytes."
   _b64url_to_b64(data).base64_decode
}

fn jwt_base64url_decode_str(str: data): str {
   "Decode unpadded RFC 7515 base64url to text."
   jwt_base64url_decode_bytes(data).text
}

fn _jwt_parts(str: token): list {
   def parts = str.split(token, ".")
   assert(parts.len >= 2, "JWT has at least header and payload")
   parts
}

fn _jwt_equal_fixed_time(str: a, str: b): bool {
   if(a.len != b.len){ return false }
   mut ok = true
   mut i = 0
   while(i < a.len){
      if(load8(a, i) != load8(b, i)){ ok = false }
      i += 1
   }
   ok
}

fn jwt_signing_input(str: token): str {
   "Return the compact JWT signing input: base64url(header) + '.' + base64url(payload)."
   def parts = _jwt_parts(token)
   parts.get(0) + "." + parts.get(1)
}

fn jwt_decode_header_json(str: token): str {
   "Return the decoded JWT header JSON without verifying the signature."
   jwt_base64url_decode_str(_jwt_parts(token).get(0))
}

fn jwt_decode_payload_json(str: token): str {
   "Return the decoded JWT payload JSON without verifying the signature."
   jwt_base64url_decode_str(_jwt_parts(token).get(1))
}

fn jwt_decode_unverified(str: token): dict {
   "Decode header and payload JSON without verifying the signature."
   def parts = _jwt_parts(token)
   mut out = dict(3)
   out = out.set("header", json_decode(jwt_base64url_decode_str(parts.get(0))))
   out = out.set("payload", json_decode(jwt_base64url_decode_str(parts.get(1))))
   out = out.set("signature", parts.len > 2 ? parts.get(2) : "")
   out
}

fn jwt_alg_none_json(str: payload_json, str: header_json="{\"alg\":\"none\",\"typ\":\"JWT\"}"): str {
   "Build an unsigned alg=none JWT from compact JSON strings."
   jwt_base64url_encode_str(header_json) + "." + jwt_base64url_encode_str(payload_json) + "."
}

fn jwt_hs256_json(str: payload_json, str: secret, str: header_json="{\"alg\":\"HS256\",\"typ\":\"JWT\"}"): str {
   "Build an HS256 JWT from compact JSON strings."
   def signing_input = jwt_base64url_encode_str(header_json) + "." + jwt_base64url_encode_str(payload_json)
   def tag = sha256_hmac(secret, signing_input)
   signing_input + "." + jwt_base64url_encode_bytes(tag)
}

fn jwt_hs256_confusion_json(str: payload_json, str: public_key_pem, str: header_json="{\"alg\":\"HS256\",\"typ\":\"JWT\"}"): str {
   "Build a JWT for RSA/HMAC algorithm-confusion tests by using the public key bytes as the HS256 secret."
   jwt_hs256_json(payload_json, public_key_pem, header_json)
}

fn jwt_hs256_verify(str: token, str: secret): bool {
   "Verify a compact JWT with HS256 and return false for alg mismatches or bad signatures."
   def parts = str.split(token, ".")
   if(parts.len != 3){ return false }
   def header = json_decode(jwt_base64url_decode_str(parts.get(0)))
   if(header.get("alg", "") != "HS256"){ return false }
   def signing_input = parts.get(0) + "." + parts.get(1)
   def expected = jwt_base64url_encode_bytes(sha256_hmac(secret, signing_input))
   _jwt_equal_fixed_time(expected, parts.get(2))
}
