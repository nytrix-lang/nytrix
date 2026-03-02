;; Keywords: text base64 base32 base16 rfc4648
;; Implementation of Base64, Base32, and Base16.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc4648.html

module std.text.base (
    encode64, decode64,
    encode64_url, decode64_url,
    encode32, decode32,
    encode32_hex, decode32_hex,
    encode16, decode16
)

use std.core *
use std.text.ascii

;; Constants

def B64_CHARS = std.text.ascii.uppercase + std.text.ascii.lowercase + std.text.ascii.digits + "+/"
def B64_URL_CHARS = std.text.ascii.uppercase + std.text.ascii.lowercase + std.text.ascii.digits + "-_"
def B32_CHARS = std.text.ascii.uppercase + "234567"
def B32_HEX_CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUV"
def B16_CHARS = std.text.ascii.digits + "ABCDEF"

;; Base 64 Implementation

fn _b64_unmap(c, url=false){
    "Internal: Maps a character code to its 6-bit value based on RFC 4648."
    if(c >= 65 && c <= 90){ return c - 65 } ;; A-Z
    if(c >= 97 && c <= 122){ return c - 71 } ;; a-z
    if(c >= 48 && c <= 57){ return c + 4 } ;; 0-9
    if(!url){
        if(c == 43){ return 62 } ;; +
        if(c == 47){ return 63 } ;; /
    } else {
        if(c == 45){ return 62 } ;; -
        if(c == 95){ return 63 } ;; _
    }
    -1
}

fn _encode64_internal(data, alphabet, padding=true){
    "Internal: Shared Base64 encoding logic."
    if(!is_str(data)){ return "" }
    def n = len(data)
    mut out = ""
    mut i = 0
    while(i < n){
        def b1 = load8(data, i)
        def b2 = (i + 1 < n) ? load8(data, i + 1) : 0
        def b3 = (i + 2 < n) ? load8(data, i + 2) : 0
        out = out + chr(load8(alphabet, b1 >> 2))
        out = out + chr(load8(alphabet, ((b1 & 3) << 4) | (b2 >> 4)))
        if(i + 1 < n){
            out = out + chr(load8(alphabet, ((b2 & 15) << 2) | (b3 >> 6)))
        } elif(padding) {
            out = out + "="
        }
        if(i + 2 < n){
            out = out + chr(load8(alphabet, b3 & 63))
        } elif(padding) {
            out = out + "="
        }
        i += 3
    }
    out
}

fn _decode64_internal(s, url=false){
    "Internal: Shared Base64 decoding logic."
    if(!is_str(s)){ return "" }
    def n = len(s)
    mut out = malloc(n)
    mut p = 0
    mut i = 0
    while(i < n){
        def v1 = load8(s, i)
        if(v1 == 0){ break }
        def c1 = _b64_unmap(v1, url)
        if(c1 == -1){
            if(v1 == 61){ break } ;; RFC 4648 padding char '='
            i += 1
            ;; Skip other characters
            while(i < n && _b64_unmap(load8(s, i), url) == -1){ i += 1 }
            continue
        }
        if(i + 1 >= n){ break }
        def c2 = _b64_unmap(load8(s, i + 1), url)
        if(c2 == -1){ break }
        store8(out, (c1 << 2) | (c2 >> 4), p)
        p += 1
        if(i + 2 < n){
            def v3 = load8(s, i + 2)
            if(v3 != 61){
                def c3 = _b64_unmap(v3, url)
                if(c3 != -1){
                    store8(out, ((c2 & 15) << 4) | (c3 >> 2), p)
                    p += 1
                    if(i + 3 < n){
                        def v4 = load8(s, i + 3)
                        if(v4 != 61){
                            def c4 = _b64_unmap(v4, url)
                            if(c4 != -1){
                                store8(out, ((c3 & 3) << 6) | c4, p)
                                p += 1
                            }
                        }
                    }
                }
            }
        }
        i += 4
    }
    init_str(out, p)
    out
}

fn encode64(data){
    "Encodes a byte string into Base64 (RFC 4648 Section 4)."
    _encode64_internal(data, B64_CHARS)
}

fn decode64(s){
    "Decodes a Base64 string into bytes (RFC 4648 Section 4)."
    _decode64_internal(s, false)
}

fn encode64_url(data){
    "Encodes a byte string into Base64 URL and Filename Safe (RFC 4648 Section 5)."
    _encode64_internal(data, B64_URL_CHARS)
}

fn decode64_url(s){
    "Decodes a Base64 URL and Filename Safe string into bytes (RFC 4648 Section 5)."
    _decode64_internal(s, true)
}

;; Base 32 Implementation

fn _b32_unmap(c, hex=false){
    "Internal: Maps a character code to its 5-bit value based on RFC 4648."
    if(!hex){
        if(c >= 65 && c <= 90){ return c - 65 } ;; A-Z
        if(c >= 97 && c <= 122){ return c - 97 } ;; a-z (decoded case-insensitively)
        if(c >= 50 && c <= 55){ return c - 24 } ;; 2-7
    } else {
        if(c >= 48 && c <= 57){ return c - 48 } ;; 0-9
        if(c >= 65 && c <= 86){ return c - 55 } ;; A-V
        if(c >= 97 && c <= 118){ return c - 87 } ;; a-v
    }
    -1
}

fn _encode32_internal(data, alphabet, padding=true){
    "Internal: Shared Base32 encoding logic."
    if(!is_str(data)){ return "" }
    def n = len(data)
    mut out = ""
    mut i = 0
    while(i < n){
        def b1 = load8(data, i)
        def b2 = (i + 1 < n) ? load8(data, i + 1) : 0
        def b3 = (i + 2 < n) ? load8(data, i + 2) : 0
        def b4 = (i + 3 < n) ? load8(data, i + 3) : 0
        def b5 = (i + 4 < n) ? load8(data, i + 4) : 0
        out = out + chr(load8(alphabet, b1 >> 3))
        out = out + chr(load8(alphabet, ((b1 & 7) << 2) | (b2 >> 6)))
        if(i + 1 < n){
            out = out + chr(load8(alphabet, (b2 >> 1) & 31))
            out = out + chr(load8(alphabet, ((b2 & 1) << 4) | (b3 >> 4)))
        } elif(padding){
            out = out + "======"
            break
        }
        if(i + 2 < n){
            out = out + chr(load8(alphabet, ((b3 & 15) << 1) | (b4 >> 7)))
        } elif(padding){
            out = out + "===="
            break
        }
        if(i + 3 < n){
            out = out + chr(load8(alphabet, (b4 >> 2) & 31))
            out = out + chr(load8(alphabet, ((b4 & 3) << 3) | (b5 >> 5)))
        } elif(padding){
            out = out + "==="
            break
        }
        if(i + 4 < n){
            out = out + chr(load8(alphabet, b5 & 31))
        } elif(padding){
            out = out + "="
            break
        }
        i += 5
    }
    out
}

fn _decode32_internal(s, hex=false){
    "Internal: Shared Base32 decoding logic."
    if(!is_str(s)){ return "" }
    def n = len(s)
    mut out = malloc(n)
    mut p = 0
    mut bits = 0
    mut val = 0
    mut i = 0
    while(i < n){
        def c = load8(s, i)
        if(c == 61){ break }
        def v = _b32_unmap(c, hex)
        if(v == -1){
            i += 1
            continue
        }
        val = (val << 5) | v
        bits += 5
        if(bits >= 8){
            store8(out, (val >> (bits - 8)) & 255, p)
            p += 1
            bits -= 8
        }
        i += 1
    }
    init_str(out, p)
    out
}

fn encode32(data){
    "Encodes a byte string into Base32 (RFC 4648 Section 6)."
    _encode32_internal(data, B32_CHARS)
}

fn decode32(s){
    "Decodes a Base32 string into bytes (RFC 4648 Section 6)."
    _decode32_internal(s, false)
}

fn encode32_hex(data){
    "Encodes a byte string into Base32 with Hex Alphabet (RFC 4648 Section 7)."
    _encode32_internal(data, B32_HEX_CHARS)
}

fn decode32_hex(s){
    "Decodes a Base32 Hex string into bytes (RFC 4648 Section 7)."
    _decode32_internal(s, true)
}

;; Base 16 Implementation

fn encode16(data){
    "Encodes a byte string into Base16 (Hex) (RFC 4648 Section 8)."
    if(!is_str(data)){ return "" }
    def n = len(data)
    mut out = ""
    mut i = 0
    while(i < n){
        def b = load8(data, i)
        out = out + chr(load8(B16_CHARS, b >> 4))
        out = out + chr(load8(B16_CHARS, b & 15))
        i += 1
    }
    out
}

fn decode16(s){
    "Decodes a Base16 (Hex) string into bytes (RFC 4648 Section 8)."
    if(!is_str(s)){ return "" }
    def n = len(s)
    mut out = malloc(n / 2)
    mut p = 0
    mut i = 0
    while(i + 1 < n){
        def c1 = load8(s, i)
        def c2 = load8(s, i + 1)
        mut v1 = -1
        if(c1 >= 48 && c1 <= 57){ v1 = c1 - 48 }
        elif(c1 >= 65 && c1 <= 70){ v1 = c1 - 55 }
        elif(c1 >= 97 && c1 <= 102){ v1 = c1 - 87 }
        mut v2 = -1
        if(c2 >= 48 && c2 <= 57){ v2 = c2 - 48 }
        elif(c2 >= 65 && c2 <= 70){ v2 = c2 - 55 }
        elif(c2 >= 97 && c2 <= 102){ v2 = c2 - 87 }
        if(v1 != -1 && v2 != -1){
            store8(out, (v1 << 4) | v2, p)
            p += 1
        }
        i += 2
    }
    init_str(out, p)
    out
}

;; Module Tests

if(comptime{__main()}){
    use std.core.error *

    ;; Base64 Tests
    def s = "hello"
    def enc64 = encode64(s)
    assert(enc64 == "aGVsbG8=", "base64 encode")
    assert(decode64(enc64) == s, "base64 decode")

    ;; Base32 Tests
    def enc32 = encode32(s)
    assert(enc32 == "NBSWY3DP", "base32 encode")
    assert(decode32(enc32) == s, "base32 decode")

    ;; Base32 Hex Tests
    def enc32hex = encode32_hex(s)
    assert(enc32hex == "D1IMOR3F", "base32 hex encode")
    assert(decode32_hex(enc32hex) == s, "base32 hex decode")

    ;; Base32 Hex RFC 4648 Test Vectors
    assert(encode32_hex("") == "", "base32 hex vector 1")
    assert(encode32_hex("f") == "CO======", "base32 hex vector 2")
    assert(encode32_hex("fo") == "CPNG====", "base32 hex vector 3")
    assert(encode32_hex("foo") == "CPNMU===", "base32 hex vector 4")
    assert(encode32_hex("foob") == "CPNMUOG=", "base32 hex vector 5")
    assert(encode32_hex("fooba") == "CPNMUOJ1", "base32 hex vector 6")
    assert(encode32_hex("foobar") == "CPNMUOJ1E8======", "base32 hex vector 7")

    assert(decode32_hex("") == "", "base32 hex decode vector 1")
    assert(decode32_hex("CO======") == "f", "base32 hex decode vector 2")
    assert(decode32_hex("CPNG====") == "fo", "base32 hex decode vector 3")
    assert(decode32_hex("CPNMU===") == "foo", "base32 hex decode vector 4")
    assert(decode32_hex("CPNMUOG=") == "foob", "base32 hex decode vector 5")
    assert(decode32_hex("CPNMUOJ1") == "fooba", "base32 hex decode vector 6")
    assert(decode32_hex("CPNMUOJ1E8======") == "foobar", "base32 hex decode vector 7")

    ;; Base16 Tests
    def enc16 = encode16(s)
    assert(enc16 == "68656C6C6F", "base16 encode")
    assert(decode16(enc16) == s, "base16 decode")
    assert(decode16("68656c6c6f") == s, "base16 decode lowercase")

    ;; URL Safe Tests
    assert(encode64_url("\xff\xff\xff") == "____", "base64 url encode")
    assert(decode64_url("____") == "\xff\xff\xff", "base64 url decode")

    print("✓ std.text.base tests passed")
}
