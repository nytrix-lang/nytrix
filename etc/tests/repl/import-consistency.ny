;; repl-expect: REPL_IMPORT_CONSISTENCY_OK
use std.math.bin as bin
use std.math.crypto.hash
use std.math.nt
use std.math.crypto.symmetric.aes
use std.core.str

def digest = sha1("abc")
def digest_bytes = bin.hex_to_bytes(digest)
def shared = power_mod(Z(2), Z(5), Z(13))

fn main(): int {
   assert(digest == "a9993e364706816aba3e25717850c26c9cd0d89d", "hash import survives pasted imports")
   assert(digest_bytes.len == 20, "bin alias survives pasted imports")
   assert(shared == Z(6), "nt import survives pasted imports")
   print("REPL_IMPORT_CONSISTENCY_OK")
   return 0
}
