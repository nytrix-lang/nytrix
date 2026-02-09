;; Keywords: math hash crc adler xxh32 fnv sha1 md5
;; Hashing algorithms (CRC32, Adler-32, FNV-1a, XXH32, SHA-1, MD5).

module std.math.hash (
   crc32, adler32, fnv1a,
   xxh32,
   sha1, md5
)
use std.core *
use std.core.mem (zalloc, memcpy)
use std.str as str

;; Helper for bitwise sanity
fn _bit(v) { 
    "Internal: Ensures a value is treated as a 64-bit integer internally for bitwise operations."
    from_int(to_int(v)) 
}

fn _u32(x) { 
    "Internal: Truncates a number to its 32-bit unsigned representation."
    band(x, 4294967295) 
}

fn _lshr32(x, n){
    "Internal: Logical shift right for 32-bit integers."
    if(n <= 0){ return _u32(x) }
    _u32(x) >> n
}

fn _rotl32(x, n) { 
    "Internal: Rotates a 32-bit integer left by `n` bits."
    def v = _u32(x)
    _u32(bor(bshl(v, n), _lshr32(v, (32 - n))))
}

fn _norm_span(s, start, len){
    "Internal: Normalizes start and length for string/buffer operations."
    if(!is_int(start)){ start = 0 }
    if(start < 0){ start = 0 }
    def n = str.len(s)
    if(start > n){ start = n }
    if(!is_int(len) || len <= 0){ len = n - start }
    if(start + len > n){ len = n - start }
    [start, len]
}

fn crc32(s, start=0, len=0){
    "Calculates the CRC32 (Cyclic Redundancy Check) checksum of a string or buffer."
    def span = _norm_span(s, start, len)
    start = get(span, 0)
    def slen = get(span, 1)
    mut c = 4294967295
    mut i = 0
    while(i < slen){
        c = bxor(c, load8(s, start + i))
        mut j = 0
        while(j < 8){
            if(band(c, 1) != 0){
                c = bxor(_lshr32(c, 1), 3988292384)
            } else {
                c = _lshr32(c, 1)
            }
            j = j + 1
        }
        i = i + 1
    }
    _u32(bxor(c, 4294967295))
}

fn adler32(s, start=0, len=0){
    "Calculates the Adler-32 checksum of a string or buffer."
    def span = _norm_span(s, start, len)
    start = get(span, 0)
    def slen = get(span, 1)
    mut a = 1
    mut b = 0
    mut i = 0
    while(i < slen){
        a = (a + _bit(load8(s, start + i))) % 65521
        b = (b + a) % 65521
        i = i + 1
    }
    _u32(bor(bshl(b, 16), a))
}

fn fnv1a(s, start=0, len=0){
    "Calculates the 32-bit FNV-1a (Fowler-Noll-Vo) hash of a string or buffer."
    def span = _norm_span(s, start, len)
    start = get(span, 0)
    def slen = get(span, 1)
    mut h = 2166136261
    mut i = 0
    while(i < slen){
        h = _u32(mul(bxor(h, _bit(load8(s, start + i))), 16777619))
        i = i + 1
    }
    h
}

fn xxh32(s, seed=0, start=0, len=0){
    "Calculates the XXH32 hash, a fast non-cryptographic hash algorithm."
    def span = _norm_span(s, start, len)
    start = get(span, 0)
    def slen = get(span, 1)
    def P1 = 2654435761
    def P2 = 2246822519
    def P3 = 3266489917
    def P4 = 668265263
    def P5 = 374761393
    mut h32 = 0
    mut p = 0
    if(slen >= 16){
        mut v1 = _u32(seed + P1 + P2)
        mut v2 = _u32(seed + P2)
        mut v3 = _u32(seed)
        mut v4 = _u32(seed - P1)
        while(p + 16 <= slen){
            v1 = _u32(mul(_rotl32(v1 + _u32(mul(_bit(load32(s, start + p)), P2)), 13), P1))
            v2 = _u32(mul(_rotl32(v2 + _u32(mul(_bit(load32(s, start + p + 4)), P2)), 13), P1))
            v3 = _u32(mul(_rotl32(v3 + _u32(mul(_bit(load32(s, start + p + 8)), P2)), 13), P1))
            v4 = _u32(mul(_rotl32(v4 + _u32(mul(_bit(load32(s, start + p + 12)), P2)), 13), P1))
            p = p + 16
        }
        h32 = _u32(_rotl32(v1, 1) + _rotl32(v2, 7) + _rotl32(v3, 12) + _rotl32(v4, 18))
    } else {
        h32 = _u32(seed + P5)
    }
    h32 = _u32(h32 + slen)
    while(p + 4 <= slen){
        h32 = _u32(mul(_rotl32(_u32(h32 + _u32(mul(_bit(load32(s, start + p)), P3))), 17), P4))
        p = p + 4
    }
    while(p < slen){
        h32 = _u32(mul(_rotl32(_u32(h32 + _u32(mul(_bit(load8(s, start + p)), P5))), 11), P1))
        p = p + 1
    }
    h32 = bxor(h32, _lshr32(h32, 15))
    h32 = _u32(mul(h32, P2))
    h32 = bxor(h32, _lshr32(h32, 13))
    h32 = _u32(mul(h32, P3))
    h32 = bxor(h32, _lshr32(h32, 16))
    h32
}

fn sha1(s, start=0, len=0){
    "Calculates the SHA-1 hash of a string or buffer.
    
    Returns the 160-bit SHA-1 hash as a hexadecimal string."
    def span = _norm_span(s, start, len)
    start = get(span, 0)
    def slen = get(span, 1)
    mut h0 = 1732584193
    mut h1 = 4023233417
    mut h2 = 2562383102
    mut h3 = 271733878
    mut h4 = 3285377520
    def p_len = ((slen + 8) / 64 + 1) * 64
    mut m = zalloc(p_len)
    
    ;; Use loop to copy string bytes correctly
    mut cpi = 0
    while(cpi < slen){
        store8(m, load8(s, start + cpi), cpi)
        cpi = cpi + 1
    }
    
    store8(m, 128, slen)
    def bitlen = slen * 8
    store8(m, band(bshr(bitlen, 56), 255), p_len - 8)
    store8(m, band(bshr(bitlen, 48), 255), p_len - 7)
    store8(m, band(bshr(bitlen, 40), 255), p_len - 6)
    store8(m, band(bshr(bitlen, 32), 255), p_len - 5)
    store8(m, band(bshr(bitlen, 24), 255), p_len - 4)
    store8(m, band(bshr(bitlen, 16), 255), p_len - 3)
    store8(m, band(bshr(bitlen, 8), 255), p_len - 2)
    store8(m, band(bitlen, 255), p_len - 1)
    mut w = malloc(320)
    mut off = 0
    while(off < p_len){
        mut t = 0
        while(t < 16){
            def base_idx = off + t * 4
            def val = bor(bor(bor(bshl(_bit(load8(m, base_idx)), 24), bshl(_bit(load8(m, base_idx + 1)), 16)), bshl(_bit(load8(m, base_idx + 2)), 8)), _bit(load8(m, base_idx + 3)))
            store32(w, to_int(_u32(val)), t * 4)
            t = t + 1
        }
        while(t < 80){
            def v = bxor(bxor(bxor(_bit(load32(w, (t - 3) * 4)), _bit(load32(w, (t - 8) * 4))), _bit(load32(w, (t - 14) * 4))), _bit(load32(w, (t - 16) * 4)))
            store32(w, to_int(_rotl32(v, 1)), t * 4)
            t = t + 1
        }
        mut ha = h0
        mut hb = h1
        mut hc = h2
        mut hd = h3
        mut he = h4
        mut i = 0
        while(i < 80){
            mut f = 0
            mut k = 0
            if(i < 20){ 
                f = bor(band(hb, hc), band(bxor(hb, 4294967295), hd))
                k = 1518500249
            } else if(i < 40){
                f = bxor(bxor(hb, hc), hd)
                k = 1859775393
            } else if(i < 60) {
                f = bor(bor(band(hb, hc), band(hb, hd)), band(hc, hd))
                k = 2400959708
            } else {
                f = bxor(bxor(hb, hc), hd)
                k = 3395469782
            }
            def temp = _u32(_rotl32(ha, 5) + f + he + k + _bit(load32(w, i * 4)))
            he = hd
            hd = hc
            hc = _rotl32(hb, 30)
            hb = ha
            ha = temp
            i = i + 1
        }
        h0 = _u32(h0 + ha)
        h1 = _u32(h1 + hb)
        h2 = _u32(h2 + hc)
        h3 = _u32(h3 + hd)
        h4 = _u32(h4 + he)
        off = off + 64
    }
    free(m)
    free(w)
    return str.to_hex(h0, 8) + str.to_hex(h1, 8) + str.to_hex(h2, 8) + str.to_hex(h3, 8) + str.to_hex(h4, 8)
}

fn md5(s, start=0, len=0){
    "Calculates the MD5 (Message-Digest Algorithm 5) hash of a string or buffer.
    
    Returns the 128-bit MD5 hash as a hexadecimal string."
    def span = _norm_span(s, start, len)
    start = get(span, 0)
    def slen = get(span, 1)
    mut h0 = 1732584193
    mut h1 = 4023233417
    mut h2 = 2562383102
    mut h3 = 271733878
    def p_len = ((slen + 8) / 64 + 1) * 64
    mut m = zalloc(p_len)
    
    ;; Loop copy instead of memcpy for safety with string objects
    mut cpi = 0
    while(cpi < slen){
        store8(m, load8(s, start + cpi), cpi)
        cpi = cpi + 1
    }
    
    store8(m, 128, slen)
    def bitlen = slen * 8
    store8(m, band(bitlen, 255), p_len - 8)
    store8(m, band(bshr(bitlen, 8), 255), p_len - 7)
    store8(m, band(bshr(bitlen, 16), 255), p_len - 6)
    store8(m, band(bshr(bitlen, 24), 255), p_len - 5)
    store8(m, band(bshr(bitlen, 32), 255), p_len - 4)
    store8(m, band(bshr(bitlen, 40), 255), p_len - 3)
    store8(m, band(bshr(bitlen, 48), 255), p_len - 2)
    store8(m, band(bshr(bitlen, 56), 255), p_len - 1)
    def K = [3614090360, 3905402710, 606105819, 3250441966, 4118548399, 1200080426, 2821735955, 4249261313, 1770035416, 2336552879, 4294925233, 2304563134, 1804603682, 4254626195, 2792965006, 1236535329, 4129170786, 3225465664, 643717713, 3921069994, 3593408605, 38016083, 3634488961, 3889429448, 568446438, 3275163606, 4107603335, 1163531501, 2850285829, 4243563512, 1735328473, 2368359562, 4294588738, 2272392833, 1839030562, 4259657740, 2763975236, 1272893353, 4139469664, 3200236656, 681279174, 3936430074, 3572445317, 76029189, 3654602809, 3873151461, 530742520, 3299628645, 4096336452, 1126891415, 2878612391, 4237533241, 1700485571, 2399980690, 4293915773, 2240044497, 1873313359, 4264355552, 2734768916, 1309151649, 4149444226, 3174756917, 718787259, 3951481745]
    def S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21]
    mut off = 0
    while(off < p_len){
        mut a = h0
        mut b = h1
        mut c = h2
        mut d = h3
        mut i = 0
        while(i < 64){
            mut f = 0
            mut g = 0
            if(i < 16){
                f = bor(band(b, c), band(bxor(b, 4294967295), d))
                g = i
            } else if(i < 32){
                f = bor(band(d, b), band(bxor(d, 4294967295), c))
                g = (5 * i + 1) % 16
            } else if(i < 48){
                f = bxor(bxor(b, c), d)
                g = (3 * i + 5) % 16
            } else {
                f = bxor(c, bor(b, bxor(d, 4294967295)))
                g = (7 * i) % 16
            }
            def base_idx = off + g * 4
            def M_g = bor(bor(bor(load8(m, base_idx), bshl(load8(m, base_idx + 1), 8)), bshl(load8(m, base_idx + 2), 16)), bshl(load8(m, base_idx + 3), 24))
            
            ;; Correct logic: temp = b + rot(a + F + K + M, s)
            def temp = _u32(b + _rotl32(_u32(a + f + get(K, i) + M_g), get(S, i)))
            
            a = d
            d = c
            c = b
            b = temp
            
            i = i + 1
        }
        h0 = _u32(h0 + a)
        h1 = _u32(h1 + b)
        h2 = _u32(h2 + c)
        h3 = _u32(h3 + d)
        off = off + 64
    }
    free(m)
    return str.to_hex(band(h0, 255), 2) + str.to_hex(band(_lshr32(h0, 8), 255), 2) + str.to_hex(band(_lshr32(h0, 16), 255), 2) + str.to_hex(band(_lshr32(h0, 24), 255), 2) + str.to_hex(band(h1, 255), 2) + str.to_hex(band(_lshr32(h1, 8), 255), 2) + str.to_hex(band(_lshr32(h1, 16), 255), 2) + str.to_hex(band(_lshr32(h1, 24), 255), 2) + str.to_hex(band(h2, 255), 2) + str.to_hex(band(_lshr32(h2, 8), 255), 2) + str.to_hex(band(_lshr32(h2, 16), 255), 2) + str.to_hex(band(_lshr32(h2, 24), 255), 2) + str.to_hex(band(h3, 255), 2) + str.to_hex(band(_lshr32(h3, 8), 255), 2) + str.to_hex(band(_lshr32(h3, 16), 255), 2) + str.to_hex(band(_lshr32(h3, 24), 255), 2)
}
