#include "base/sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint32_t ny_sha256_rotr32(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32u - n));
}

static void ny_sha256_block(uint32_t state[8],
                                const unsigned char block[64]) {
  static const uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  uint32_t w[64];
  for (size_t i = 0; i < 16; ++i) {
    size_t j = i * 4;
    w[i] = ((uint32_t)block[j] << 24) | ((uint32_t)block[j + 1] << 16) |
           ((uint32_t)block[j + 2] << 8) | (uint32_t)block[j + 3];
  }
  for (size_t i = 16; i < 64; ++i) {
    uint32_t s0 = ny_sha256_rotr32(w[i - 15], 7) ^ ny_sha256_rotr32(w[i - 15], 18) ^
                  (w[i - 15] >> 3);
    uint32_t s1 = ny_sha256_rotr32(w[i - 2], 17) ^ ny_sha256_rotr32(w[i - 2], 19) ^
                  (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4],
           f = state[5], g = state[6], h = state[7];
  for (size_t i = 0; i < 64; ++i) {
    uint32_t S1 =
        ny_sha256_rotr32(e, 6) ^ ny_sha256_rotr32(e, 11) ^ ny_sha256_rotr32(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + k[i] + w[i];
    uint32_t S0 =
        ny_sha256_rotr32(a, 2) ^ ny_sha256_rotr32(a, 13) ^ ny_sha256_rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

bool ny_sha256_file(const char *path, char *out, size_t out_len) {
  if (!path || !out || out_len < 72)
    return false;
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  unsigned char block[64];
  uint64_t total = 0;
  size_t used = 0;
  while (!feof(f)) {
    size_t n = fread(block + used, 1, sizeof(block) - used, f);
    total += n;
    used += n;
    if (used == sizeof(block)) {
      ny_sha256_block(state, block);
      used = 0;
    }
    if (ferror(f)) {
      fclose(f);
      return false;
    }
  }
  fclose(f);
  uint64_t bits = total * UINT64_C(8);
  block[used++] = 0x80;
  if (used > 56) {
    memset(block + used, 0, sizeof(block) - used);
    ny_sha256_block(state, block);
    used = 0;
  }
  memset(block + used, 0, 56 - used);
  for (size_t i = 0; i < 8; ++i)
    block[63 - i] = (unsigned char)(bits >> (i * 8));
  ny_sha256_block(state, block);
  char *w = out;
  size_t left = out_len;
  int n = snprintf(w, left, "sha256:");
  if (n < 0 || (size_t)n >= left)
    return false;
  w += n;
  left -= (size_t)n;
  for (size_t i = 0; i < 8; ++i) {
    n = snprintf(w, left, "%08x", state[i]);
    if (n != 8 || (size_t)n >= left)
      return false;
    w += n;
    left -= (size_t)n;
  }
  return true;
}

