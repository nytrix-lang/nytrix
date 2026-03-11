;; Keywords: encoding ascii
;; Encoding routines for ASCII conversion and scoring operations.
;; Reference:
module std.math.crypto.encoding.ascii(lowercase, uppercase, letters, digits, hexdigits, octdigits, punctuation, whitespace, printable)
use std.core

def lowercase   = "abcdefghijklmnopqrstuvwxyz"
def uppercase   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
def letters     = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
def digits      = "0123456789"
def hexdigits   = "0123456789abcdefABCDEF"
def octdigits   = "01234567"
def punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
def whitespace  = " \t\n\r\x0B\x0C"
def printable   = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\x0B\x0C"
