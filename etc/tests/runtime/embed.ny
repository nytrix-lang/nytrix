use std.str.io *
use std.str *
use std.core.reflect *

def RES = embed("etc/assets/text/lorem.txt")
def EXPECTED = "Nytrix asset text fixture.
The quick brown fox jumps over the lazy dog.
0123456789 +-*/= _ . , : ; ! ? ( ) [ ] { } < > | Ω Δ π Σ 🫠"

print("RES length:")
print(len(RES))
print("EXPECTED length:")
print(len(EXPECTED))

if (RES == EXPECTED) {
    print("✓ embed test passed")
} else {
    print("✗ embed test failed")
}
