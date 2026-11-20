/* Checked JIT mapping layout, branch displacements, and relocation stubs. */
#ifndef NYTRIX_NATIVE_JIT_H
#define NYTRIX_NATIVE_JIT_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  size_t text_size;
  size_t stub_offset;
  size_t stub_capacity;
  size_t stub_size;
  size_t data_size;
  size_t code_size;
  size_t mapping_size;
} nyr_jit_layout_t;

static inline bool nyr_jit_align(size_t size, size_t align, size_t *out) {
  if (!out || !align || (align & (align - 1)) || size > SIZE_MAX - (align - 1))
    return false;
  *out = (size + align - 1) & ~(align - 1);
  return true;
}

static inline bool nyr_jit_layout_init(size_t text_size, size_t reloc_count,
                                       size_t stub_size, size_t data_size,
                                       bool adjacent_data,
                                       nyr_jit_layout_t *out) {
  size_t stub_offset, aligned_data, code_size;
  if (!out || !stub_size || !nyr_jit_align(text_size, 16, &stub_offset) ||
      reloc_count > (SIZE_MAX - stub_offset) / stub_size ||
      !nyr_jit_align(stub_offset + reloc_count * stub_size, 4096, &code_size) ||
      !nyr_jit_align(data_size, 16, &aligned_data))
    return false;
  if (adjacent_data &&
      (code_size > INT32_MAX || aligned_data > (size_t)INT32_MAX - code_size))
    return false;
  if (adjacent_data && aligned_data > SIZE_MAX - code_size)
    return false;
  *out = (nyr_jit_layout_t){text_size,
                            stub_offset,
                            reloc_count * stub_size,
                            stub_size,
                            aligned_data,
                            code_size,
                            code_size + (adjacent_data ? aligned_data : 0)};
  return true;
}

static inline bool nyr_jit_range(size_t length, size_t off, size_t width) {
  return off <= length && width <= length - off;
}

static inline bool nyr_jit_take_stub(const nyr_jit_layout_t *layout,
                                     size_t *cursor, size_t *offset) {
  if (!layout || !cursor || !offset || *cursor < layout->stub_offset ||
      *cursor > layout->code_size ||
      layout->stub_size > layout->code_size - *cursor ||
      *cursor - layout->stub_offset > layout->stub_capacity ||
      layout->stub_size >
          layout->stub_capacity - (*cursor - layout->stub_offset))
    return false;
  *offset = *cursor;
  *cursor += layout->stub_size;
  return true;
}

static inline bool nyr_jit_rel32(uintptr_t target, uintptr_t next,
                                 int32_t *out) {
  if (!out)
    return false;
  if (target >= next) {
    uintptr_t dist = target - next;
    if (dist > INT32_MAX)
      return false;
    *out = (int32_t)dist;
  } else {
    uintptr_t dist = next - target;
    if (dist > (uintptr_t)INT32_MAX + 1)
      return false;
    *out = -(int64_t)dist;
  }
  return true;
}

static inline bool nyr_jit_call26(uintptr_t target, uintptr_t pc,
                                  uint32_t *encoded) {
  if (!encoded || (target & 3) || (pc & 3))
    return false;
  if (target >= pc) {
    uintptr_t dist = target - pc;
    if (dist > (((uintptr_t)1 << 25) - 1) * 4)
      return false;
    *encoded = ((uint32_t)(dist / 4)) & 0x03ffffffu;
  } else {
    uintptr_t dist = pc - target;
    if (dist > ((uintptr_t)1 << 25) * 4)
      return false;
    *encoded = ((uint32_t)(-(int64_t)(dist / 4))) & 0x03ffffffu;
  }
  return true;
}

#endif
