;; Keywords: error panic precondition require validation crypto-error math crypto
;; Crypto error and precondition routines for consistent failure reporting.
;; References:
;; - std.math.crypto
module std.math.crypto.error(crypto_fail, crypto_require, crypto_require_nonempty, crypto_require_len)
use std.core

fn crypto_fail(str scope, str msg) any {
   "Raise a standardized crypto module panic with a scoped message."
   panic("std.math.crypto." + scope + ": " + msg)
}

fn crypto_require(bool cond, str scope, str msg) bool {
   "Require a condition or raise a standardized crypto module panic."
   if(!cond){ crypto_fail(scope, msg) }
   true
}

fn crypto_require_nonempty(any value, str scope, str label="value") bool {
   "Require a value with positive length."
   if(value == nil){ crypto_fail(scope, label + " is nil") }
   if(value.len <= 0){ crypto_fail(scope, label + " is empty") }
   true
}

fn crypto_require_len(any value, int expected_len, str scope, str label="value") bool {
   "Require a value with exactly expected_len elements."
   crypto_require(value != nil, scope, label + " is nil")
   crypto_require(value.len == expected_len, scope, label + " must have length " + str(expected_len))
   true
}
