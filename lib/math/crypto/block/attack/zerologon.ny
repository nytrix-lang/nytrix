;; Keywords: block-cipher attack zerologon math crypto
;; Block-cipher attack routines for Zerologon verification and attack steps.
;; Model the all-zero AES-CFB8 credential condition used by ZeroLogon.
;; Reference:
;; - https://www.secura.com/uploads/whitepapers/Zerologon.pdf
;; - https://nvd.nist.gov/vuln/detail/CVE-2020-1472
;; - https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf
;; References:
;; - std.math.crypto.block.attack
;; - std.math.crypto
module std.math.crypto.block.attack.zerologon(zerologon_attack_step, zerologon_verify)
use std.core
use std.math.bin

fn _zero_bytes(int n) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(0)
      i += 1
   }
   out
}

fn _all_zero(list bs) bool {
   mut i = 0
   while i < bs.len {
      if bs[i] != 0 { return false }
      i += 1
   }
   bs.len > 0
}

fn zerologon_attack_step(fnptr encrypt_oracle, int nonce_size) dict {
   "Try the all-zero AES-CFB8 credential condition used by ZeroLogon.
   Netlogon accepts repeated all-zero attempts because AES-CFB8 with an
   all-zero challenge can emit an all-zero credential with probability 1/256.
   encrypt_oracle: function(attempt_count, nonce) -> returns encrypted data
   nonce_size: size of nonce/IV in bytes(typically 8 for Netlogon)
   Returns a dict with:
   .attempts: number of attempts to find zero encryption
   .success: boolean indicating if zero ciphertext was found
   .recovered_byte: diagnostic byte marker, 0 on success and -1 otherwise"
   def nonce = _zero_bytes(nonce_size)
   mut attempts = 0
   mut found = false
   def max_attempts = 5000
   while attempts < max_attempts && !found {
      def ct = encrypt_oracle(attempts, nonce)
      if _all_zero(ct) { found = true }
      attempts += 1
   }
   def recovered = found ? 0 : -1
   {"attempts": attempts, "success": found, "recovered_byte": recovered}
}

fn zerologon_verify(fnptr auth_oracle, list session_key) bool {
   "Verify recovered session key authenticates successfully.
   Uses the recovered key to compute a valid authentication signature
   and checks it against the server's expected response.
   auth_oracle: function(session_key, challenge) -> returns auth response
   session_key: recovered session key as byte list
   Returns true if authentication succeeds, false otherwise."
   def key_len = session_key.len
   if key_len == 0 { return false }
   def challenge = _zero_bytes(8)
   def response = auth_oracle(session_key, challenge)
   def resp_len = response.len
   if resp_len == 0 { return false }
   mut all_match = true
   mut i = 0
   while i < resp_len {
      def expected = bxor(session_key[i % key_len], challenge[i % 8])
      if response[i] != expected {
         all_match = false
         i = resp_len
      }
      i += 1
   }
   all_match
}
