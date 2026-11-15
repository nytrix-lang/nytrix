/*
 * Shared object-buffer operations and ELF32 serialization helpers.
 */
#include "code/backend/mach/object.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void nyr_obj_free(nyr_obj_buf_t *b) {
  if (!b)
    return;
  free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

bool nyr_native_debug_line_append(nyr_native_debug_line_t **lines,
                                  size_t *count, size_t *cap,
                                  nyr_native_debug_line_t line) {
  if (!lines || !count || !cap || *count > *cap)
    return false;
  if (*count == *cap) {
    if (*count == SIZE_MAX)
      return false;
    size_t next = *cap > SIZE_MAX / 2 ? *count + 1 : (*cap ? *cap * 2 : 32);
    if (next < *count + 1 || next > SIZE_MAX / sizeof(**lines))
      return false;
    nyr_native_debug_line_t *grown = realloc(*lines, next * sizeof(**lines));
    if (!grown)
      return false;
    *lines = grown;
    *cap = next;
  }
  (*lines)[(*count)++] = line;
  return true;
}

static bool ny_obj_reserve(nyr_obj_buf_t *b, size_t add) {
  if (!b)
    return false;
  if (add > (size_t)-1 - b->len)
    return false;
  size_t need = b->len + add;
  if (need <= b->cap)
    return true;
  size_t cap = b->cap ? b->cap * 2 : 256;
  while (cap < need) {
    if (cap > (size_t)-1 / 2)
      return false;
    cap *= 2;
  }
  unsigned char *p = realloc(b->data, cap);
  if (!p)
    return false;
  b->data = p;
  b->cap = cap;
  return true;
}

bool nyr_obj_emit(nyr_obj_buf_t *b, const void *data, size_t len) {
  if (!b || (len && !data))
    return false;
  if (len == 0)
    return true;
  if (!ny_obj_reserve(b, len))
    return false;
  memcpy(b->data + b->len, data, len);
  b->len += len;
  return true;
}

bool nyr_obj_u8(nyr_obj_buf_t *b, unsigned v) {
  unsigned char c = (unsigned char)v;
  return nyr_obj_emit(b, &c, 1);
}

bool nyr_obj_pad_to(nyr_obj_buf_t *b, size_t align) {
  if (align == 0)
    return true;
  while ((b->len % align) != 0) {
    if (!nyr_obj_u8(b, 0))
      return false;
  }
  return true;
}

bool nyr_obj_zero(nyr_obj_buf_t *b, size_t len) {
  if (!ny_obj_reserve(b, len))
    return false;
  memset(b->data + b->len, 0, len);
  b->len += len;
  return true;
}

bool nyr_obj_u16(nyr_obj_buf_t *b, uint16_t v) {
  unsigned char c[2] = {(unsigned char)(v & 0xff), (unsigned char)(v >> 8)};
  return nyr_obj_emit(b, c, sizeof(c));
}

bool nyr_obj_u32(nyr_obj_buf_t *b, uint32_t v) {
  unsigned char c[4] = {
      (unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
      (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff)};
  return nyr_obj_emit(b, c, sizeof(c));
}

bool nyr_obj_u64(nyr_obj_buf_t *b, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    if (!nyr_obj_u8(b, (unsigned)((v >> (i * 8)) & 0xff)))
      return false;
  }
  return true;
}

void nyr_obj_patch_u16(nyr_obj_buf_t *b, size_t off, uint16_t v) {
  b->data[off + 0] = (unsigned char)(v & 0xff);
  b->data[off + 1] = (unsigned char)(v >> 8);
}

void nyr_obj_patch_u32(nyr_obj_buf_t *b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b->data[off + (size_t)i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

void nyr_obj_patch_u64(nyr_obj_buf_t *b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    b->data[off + (size_t)i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

bool nyr_elf32_write_sym(nyr_obj_buf_t *b, uint32_t name, uint32_t value,
                         uint32_t size, unsigned char info, uint16_t shndx) {
  return nyr_obj_u32(b, name) && nyr_obj_u32(b, value) &&
         nyr_obj_u32(b, size) && nyr_obj_u8(b, info) && nyr_obj_u8(b, 0) &&
         nyr_obj_u16(b, shndx);
}

bool nyr_elf32_write_sh(nyr_obj_buf_t *b, uint32_t name, uint32_t type,
                        uint32_t flags, uint32_t off, uint32_t size,
                        uint32_t link, uint32_t info, uint32_t addralign,
                        uint32_t entsize) {
  return nyr_obj_u32(b, name) && nyr_obj_u32(b, type) &&
         nyr_obj_u32(b, flags) && nyr_obj_u32(b, 0) && nyr_obj_u32(b, off) &&
         nyr_obj_u32(b, size) && nyr_obj_u32(b, link) && nyr_obj_u32(b, info) &&
         nyr_obj_u32(b, addralign) && nyr_obj_u32(b, entsize);
}
