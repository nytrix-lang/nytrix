;; Keywords: simmd simd vectorized math
;; Explicit SIMD and instruction-control operations for vectorized numeric code.
;; References:
;; - std.math
module std.math.simmd(has_feature, has_sse2, has_sse3, has_ssse3, has_sse41, has_sse42, has_avx, has_avx2, has_avx512f, has_avx512bw, has_avx512vl, has_bmi1, has_bmi2, has_lzcnt, has_fma, has_popcnt, has_aes, has_pclmul, has_crc32, has_crc32c, has_sha, has_neon, popcnt32, ctz32, clz32, bswap32, rotl32, rotr32, popcnt64, ctz64, clz64, bswap64, rotl64, rotr64, pext64, pdep64, clmul64_lo, clmul64_hi, u8x16_xor_ptr, u8x16_and_ptr, u8x16_or_ptr, u8x16_add_ptr, u8x16_sub_ptr, u8x16_cmpeq_mask_ptr, u8x16_shuffle_ptr, u16x8_add_ptr, u16x8_sub_ptr, u16x8_mullo_ptr, i32x4_add_ptr, i32x4_sub_ptr, i32x4_mullo_ptr, i32x4_xor_ptr, i32x4_and_ptr, i32x4_or_ptr, u32x4_add_ptr, u32x4_sub_ptr, u32x4_xor_ptr, u32x4_and_ptr, u32x4_or_ptr, u64x2_add_ptr, u64x2_xor_ptr, u64x2_and_ptr, u64x2_or_ptr, f32x4_add_ptr, f32x4_sub_ptr, f32x4_mul_ptr, f32x4_div_ptr, f32x4_min_ptr, f32x4_max_ptr, f32x4_sqrt_ptr, f32x4_fma_ptr, f64x2_add_ptr, f64x2_sub_ptr, f64x2_mul_ptr, f64x2_div_ptr, f64x2_sqrt_ptr, f64x2_fma_ptr, prefetch_read, prefetch_write, pause, lfence, sfence, mfence, rdtsc, crc32c_u8, crc32_u8, crc32c_u64, ascii_class_mask, ascii_class_reduce, ascii_class_reduce_ptr, ascii_vowel_reduce, ascii_vowel_reduce_ptr, jsonscan_ascii, i32_hash_put_ptr, i32_hash_probe_sum_ptr, i32_sqlscan_sum_ptr, mat4_mul, mat4_mul_ptr)
use std.core

def _ASCII_VOWEL_MASK = 9150281795239936

fn has_feature(str name) bool {
   "Returns true when this runtime supports a named CPU feature."
   __simmd_has_feature(name)
}

fn has_sse2() bool { has_feature("sse2") }

fn has_sse3() bool { has_feature("sse3") }

fn has_ssse3() bool { has_feature("ssse3") }

fn has_sse41() bool { has_feature("sse4.1") }

fn has_sse42() bool { has_feature("sse4.2") }

fn has_avx() bool { has_feature("avx") }

fn has_avx2() bool { has_feature("avx2") }

fn has_avx512f() bool { has_feature("avx512f") }

fn has_avx512bw() bool { has_feature("avx512bw") }

fn has_avx512vl() bool { has_feature("avx512vl") }

fn has_bmi1() bool { has_feature("bmi1") }

fn has_bmi2() bool { has_feature("bmi2") }

fn has_lzcnt() bool { has_feature("lzcnt") }

fn has_fma() bool { has_feature("fma") }

fn has_popcnt() bool { has_feature("popcnt") }

fn has_aes() bool { has_feature("aes") }

fn has_pclmul() bool { has_feature("pclmul") }

fn has_crc32() bool { has_feature("crc32") }

fn has_crc32c() bool { has_feature("crc32c") }

fn has_sha() bool { has_feature("sha") }

fn has_neon() bool { has_feature("neon") }

@inline
fn popcnt64(int x) int { __simmd_popcnt64(x) }

@inline
fn ctz64(int x) int { __simmd_ctz64(x) }

@inline
fn clz64(int x) int { __simmd_clz64(x) }

@inline
fn bswap64(int x) int { __simmd_bswap64(x) }

@inline
fn rotl64(int x, int k) int { __simmd_rotl64(x, k) }

@inline
fn rotr64(int x, int k) int { __simmd_rotr64(x, k) }

@inline
fn pext64(int x, int mask) int {
   "Parallel bit extract. Uses BMI2 when available, scalar fallback otherwise."
   __simmd_pext64(x, mask)
}

@inline
fn pdep64(int x, int mask) int {
   "Parallel bit deposit. Uses BMI2 when available, scalar fallback otherwise."
   __simmd_pdep64(x, mask)
}

@inline
fn clmul64_lo(int x, int y) int {
   "Low 64 bits of carry-less GF(2) multiplication. Uses PCLMUL where available."
   __simmd_clmul64_lo(x, y)
}

@inline
fn clmul64_hi(int x, int y) int {
   "High 64 bits of carry-less GF(2) multiplication. Uses PCLMUL where available."
   __simmd_clmul64_hi(x, y)
}

fn u8x16_xor_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 16-byte vector XOR from raw pointers."
   __simmd_u8x16_xor_ptr(a, b, out)
}

fn u8x16_and_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 16-byte vector AND from raw pointers."
   __simmd_u8x16_and_ptr(a, b, out)
}

fn u8x16_or_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 16-byte vector OR from raw pointers."
   __simmd_u8x16_or_ptr(a, b, out)
}

fn u8x16_add_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 16-lane u8 wrapping add from raw pointers."
   __simmd_u8x16_add_ptr(a, b, out)
}

fn u8x16_sub_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 16-lane u8 wrapping subtract from raw pointers."
   __simmd_u8x16_sub_ptr(a, b, out)
}

fn u8x16_cmpeq_mask_ptr(ptr a, ptr b) int {
   "Returns a 16-bit equality mask for two unaligned u8x16 vectors."
   __simmd_u8x16_cmpeq_mask_ptr(a, b)
}

fn u8x16_shuffle_ptr(ptr a, ptr mask, ptr out) ptr {
   "Unaligned byte-lane shuffle. Mask bytes 0..15 select lanes; high-bit masks zero a lane."
   __simmd_u8x16_shuffle_ptr(a, mask, out)
}

fn u16x8_add_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 8-lane u16 wrapping add from raw pointers."
   __simmd_u16x8_add_ptr(a, b, out)
}

fn u16x8_sub_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 8-lane u16 wrapping subtract from raw pointers."
   __simmd_u16x8_sub_ptr(a, b, out)
}

fn u16x8_mullo_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 8-lane u16 low-half multiply from raw pointers."
   __simmd_u16x8_mullo_ptr(a, b, out)
}

fn i32x4_add_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 4-lane i32 vector add from raw pointers."
   __simmd_i32x4_add_ptr(a, b, out)
}

fn i32x4_sub_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 4-lane i32 vector subtract from raw pointers."
   __simmd_i32x4_sub_ptr(a, b, out)
}

fn i32x4_mullo_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 4-lane i32 low-half multiply from raw pointers."
   __simmd_i32x4_mullo_ptr(a, b, out)
}

fn i32x4_xor_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 4-lane i32 vector XOR from raw pointers."
   __simmd_i32x4_xor_ptr(a, b, out)
}

fn i32x4_and_ptr(ptr a, ptr b, ptr out) ptr { __simmd_u32x4_and_ptr(a, b, out) }

fn i32x4_or_ptr(ptr a, ptr b, ptr out) ptr { __simmd_u32x4_or_ptr(a, b, out) }

fn u32x4_add_ptr(ptr a, ptr b, ptr out) ptr { __simmd_i32x4_add_ptr(a, b, out) }

fn u32x4_sub_ptr(ptr a, ptr b, ptr out) ptr { __simmd_i32x4_sub_ptr(a, b, out) }

fn u32x4_xor_ptr(ptr a, ptr b, ptr out) ptr { __simmd_i32x4_xor_ptr(a, b, out) }

fn u32x4_and_ptr(ptr a, ptr b, ptr out) ptr { __simmd_u32x4_and_ptr(a, b, out) }

fn u32x4_or_ptr(ptr a, ptr b, ptr out) ptr { __simmd_u32x4_or_ptr(a, b, out) }

fn u64x2_add_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 2-lane u64 wrapping add from raw pointers."
   __simmd_u64x2_add_ptr(a, b, out)
}

fn u64x2_xor_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 2-lane u64 XOR from raw pointers."
   __simmd_u64x2_xor_ptr(a, b, out)
}

fn u64x2_and_ptr(ptr a, ptr b, ptr out) ptr { __simmd_u64x2_and_ptr(a, b, out) }

fn u64x2_or_ptr(ptr a, ptr b, ptr out) ptr { __simmd_u64x2_or_ptr(a, b, out) }

fn f32x4_add_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 4-lane f32 vector add from raw pointers."
   __simmd_f32x4_add_ptr(a, b, out)
}

fn f32x4_sub_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 4-lane f32 vector subtract from raw pointers."
   __simmd_f32x4_sub_ptr(a, b, out)
}

fn f32x4_mul_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 4-lane f32 vector multiply from raw pointers."
   __simmd_f32x4_mul_ptr(a, b, out)
}

fn f32x4_div_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 4-lane f32 vector divide from raw pointers."
   __simmd_f32x4_div_ptr(a, b, out)
}

fn f32x4_min_ptr(ptr a, ptr b, ptr out) ptr { __simmd_f32x4_min_ptr(a, b, out) }

fn f32x4_max_ptr(ptr a, ptr b, ptr out) ptr { __simmd_f32x4_max_ptr(a, b, out) }

fn f32x4_sqrt_ptr(ptr a, ptr out) ptr { __simmd_f32x4_sqrt_ptr(a, out) }

fn f32x4_fma_ptr(ptr a, ptr b, ptr c, ptr out) ptr { __simmd_f32x4_fma_ptr(a, b, c, out) }

fn f64x2_add_ptr(ptr a, ptr b, ptr out) ptr {
   "Unaligned 2-lane f64 vector add from raw pointers."
   __simmd_f64x2_add_ptr(a, b, out)
}

fn f64x2_sub_ptr(ptr a, ptr b, ptr out) ptr { __simmd_f64x2_sub_ptr(a, b, out) }

fn f64x2_mul_ptr(ptr a, ptr b, ptr out) ptr { __simmd_f64x2_mul_ptr(a, b, out) }

fn f64x2_div_ptr(ptr a, ptr b, ptr out) ptr { __simmd_f64x2_div_ptr(a, b, out) }

fn f64x2_sqrt_ptr(ptr a, ptr out) ptr { __simmd_f64x2_sqrt_ptr(a, out) }

fn f64x2_fma_ptr(ptr a, ptr b, ptr c, ptr out) ptr { __simmd_f64x2_fma_ptr(a, b, c, out) }

@inline
fn popcnt32(int x) int { __simmd_popcnt32(x) }

@inline
fn ctz32(int x) int { __simmd_ctz32(x) }

@inline
fn clz32(int x) int { __simmd_clz32(x) }

@inline
fn bswap32(int x) int { __simmd_bswap32(x) }

@inline
fn rotl32(int x, int k) int { __simmd_rotl32(x, k) }

@inline
fn rotr32(int x, int k) int { __simmd_rotr32(x, k) }

fn prefetch_read(any p, int locality=3) any {
   "Prefetches memory for read. Locality is 0..3."
   __simmd_prefetch(p, 0, locality)
}

fn prefetch_write(any p, int locality=3) any {
   "Prefetches memory for write. Locality is 0..3."
   __simmd_prefetch(p, 1, locality)
}

fn pause() int { __simmd_pause() }

fn lfence() int { __simmd_lfence() }

fn sfence() int { __simmd_sfence() }

fn mfence() int { __simmd_mfence() }

fn rdtsc() int { __simmd_rdtsc() }

@inline
fn crc32c_u8(int crc, int byte) int {
   "Updates a CRC32C accumulator with one byte. Uses SSE4.2 where available."
   __simmd_crc32_u8(crc, byte)
}

@inline
fn crc32c_u64(int crc, int word) int {
   "Updates a CRC32C accumulator with one little-endian 64-bit word."
   __simmd_crc32_u64(crc, word)
}

fn crc32_u8(int crc, int byte) int {
   "Compatibility alias for crc32c_u8; the x86 instruction is CRC32C."
   crc32c_u8(crc, byte)
}

fn ascii_class_mask(str chars) list {
   "Builds a two-word ASCII class mask for byte kernels."
   mut lo = 0
   mut hi = 0
   mut i = 0
   while i < chars.len {
      def c = load8(chars, i)
      if c >= 0 && c < 64 {
         lo = lo | (1 << c)
      } elif c < 128 {
         hi = hi | (1 << (c - 64))
      }
      i += 1
   }
   [lo, hi]
}

fn _span_len(any data, int start=0, int count=0) list {
   mut n, s = len(data), start
   if s < 0 { s = n + s }
   if s < 0 { s = 0 }
   if s > n { s = n }
   mut c = count
   if c <= 0 || s + c > n { c = n - s }
   [s, c]
}

fn ascii_class_reduce(any data, int rounds, str chars, int hit=1, int miss=0, int start=0, int count=0) int {
   "Reduces bytes by an ASCII class. AVX2 accelerates common byte-class scans."
   def span = _span_len(data, start, count)
   def s = span.get(0)
   def n = span.get(1)
   def mask = ascii_class_mask(chars)
   __simmd_byte_class_reduce(ptr_add(data, s), n, rounds, mask.get(0), mask.get(1), hit, miss)
}

fn ascii_class_reduce_ptr(ptr data, int n, int rounds, str chars, int hit=1, int miss=0, int start=0, int count=0) int {
   "Reduces bytes by an ASCII class from a raw pointer without constructing a string."
   mut s = start
   if s < 0 { s = n + s }
   if s < 0 { s = 0 }
   if s > n { s = n }
   mut c = count
   if c <= 0 || s + c > n { c = n - s }
   def mask = ascii_class_mask(chars)
   __simmd_byte_class_reduce(ptr_add(data, s), c, rounds, mask.get(0), mask.get(1), hit, miss)
}

fn ascii_vowel_reduce(any data, int rounds, int hit=3, int miss=1, int start=0, int count=0) int {
   "Specialized fast ASCII vowel reducer for byte-scan hot paths."
   mut n, s = len(data), start
   if s < 0 { s = n + s }
   if s < 0 { s = 0 }
   if s > n { s = n }
   mut c = count
   if c <= 0 || s + c > n { c = n - s }
   __simmd_byte_class_reduce(ptr_add(data, s), c, rounds, 0, _ASCII_VOWEL_MASK, hit, miss)
}

fn ascii_vowel_reduce_ptr(ptr data, int n, int rounds, int hit=3, int miss=1, int start=0, int count=0) int {
   "Specialized fast ASCII vowel reducer from a raw pointer without constructing a string."
   mut s = start
   if s < 0 { s = n + s }
   if s < 0 { s = 0 }
   if s > n { s = n }
   mut c = count
   if c <= 0 || s + c > n { c = n - s }
   __simmd_byte_class_reduce(ptr_add(data, s), c, rounds, 0, _ASCII_VOWEL_MASK, hit, miss)
}

fn jsonscan_ascii(any data, int rounds=1, int start=0, int count=0) int {
   "Runs the JSON ASCII scan checksum kernel used by Nytrix byte-scan benchmarks."
   def span = _span_len(data, start, count)
   def s = span.get(0)
   def n = span.get(1)
   __simmd_jsonscan_ascii(ptr_add(data, s), n, rounds)
}

fn i32_hash_put_ptr(ptr keys, ptr values, ptr used, int cap, int key, int value) bool {
   "Inserts one i32 key/value into a raw power-of-two linear-probe table."
   __simmd_i32_hash_put_ptr(keys, values, used, cap, key, value)
}

fn i32_hash_probe_sum_ptr(ptr keys, ptr values, ptr used, int cap, ptr probe_keys, ptr probe_weights, int probe_n, int rounds=1) int {
   "Probes a raw i32 linear-probe table and returns the join checksum."
   __simmd_i32_hash_probe_sum_ptr(keys, values, used, cap, probe_keys, probe_weights,
   probe_n, rounds)
}

fn i32_sqlscan_sum_ptr(ptr region, ptr tier, ptr amount, ptr flags, int n, int rounds=1) int {
   "Runs a raw i32 column filter/aggregate checksum kernel."
   __simmd_i32_sqlscan_sum_ptr(region, tier, amount, flags, n, rounds)
}

fn mat4_mul(list a, list b, any out=0) list {
   "SIMD-backed 4x4 matrix multiply for Ny list matrices. Returns `out`; when omitted, allocates a 16-slot output list."
   if !is_list(out) {
      out = list(16)
      mut i = 0
      while i < 16 {
         out = out.append(0.0)
         i += 1
      }
   }
   __simd_mat4_mul(a, b, out)
}

fn mat4_mul_ptr(ptr a, ptr b, ptr out) ptr {
   "SIMD-backed 4x4 matrix multiply for raw f32 pointer buffers."
   __simd_mat4_mul_ptr(a, b, out)
}

#main {
   def a = malloc(64)
   def b = malloc(64)
   def out = malloc(64)
   mut i = 0
   while i < 16 {
      def diag = (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0 : 0.0
      store32_f32(a, diag, i * 4)
      store32_f32(b, float(i + 1), i * 4)
      store32_f32(out, 0.0, i * 4)
      i += 1
   }
   mat4_mul_ptr(a, b, out)
   assert(load32_f32(out, 0) == 1.0 && load32_f32(out, 20) == 6.0 && load32_f32(out, 60) == 16.0, "simmd mat4_mul_ptr identity")
   free(a)
   free(b)
   free(out)
   assert(popcnt32(0b101010) == 3 && bswap32(0x12345678) == 0x78563412, "simmd scalar helpers")
   print("✓ std.math.simmd self-test passed")
}
