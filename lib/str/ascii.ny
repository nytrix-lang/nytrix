;; Keywords: text ascii rfc20 string constants
;; ASCII character class constants.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc20.html

module std.str.ascii (
   lowercase, uppercase, letters,
   digits, hexdigits, octdigits,
   punctuation, whitespace, printable
)

use std.core *

def lowercase   = "abcdefghijklmnopqrstuvwxyz"
def uppercase   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
def letters     = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
def digits      = "0123456789"
def hexdigits   = "0123456789abcdefABCDEF"
def octdigits   = "01234567"
def punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
def whitespace  = " \t\n\r\x0B\x0C"
def printable   = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\x0B\x0C"

if(comptime{__main()}){
   use std.core *
   use std.str.ascii *

   assert(lowercase   == "abcdefghijklmnopqrstuvwxyz", "lowercase")
   assert(uppercase   == "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "uppercase")
   assert(letters     == "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ", "letters")
   assert(digits      == "0123456789", "digits")
   assert(hexdigits   == "0123456789abcdefABCDEF", "hexdigits")
   assert(octdigits   == "01234567", "octdigits")
   print("✓ std.str.ascii tests passed")
}
