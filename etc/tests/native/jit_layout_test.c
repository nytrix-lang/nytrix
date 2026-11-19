#include "code/backend/lower/jit_layout.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

int main(void) {
  const size_t sizes[] = {0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 65,
                          127, 128, 255, 256, 257};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
    nyr_jit_layout_t layout;
    assert(nyr_jit_layout_init(sizes[i], 3, 16, sizes[i], true, &layout));
    assert(layout.stub_offset >= sizes[i]);
    assert(layout.stub_capacity == 48);
    assert(layout.mapping_size == layout.code_size + layout.data_size);
    size_t cursor = layout.stub_offset, offset;
    for (int j = 0; j < 3; ++j) {
      assert(nyr_jit_take_stub(&layout, &cursor, &offset));
      assert(offset >= layout.stub_offset);
      assert(offset + 16 <= layout.code_size);
    }
    assert(!nyr_jit_take_stub(&layout, &cursor, &offset));
    assert(nyr_jit_range(sizes[i], 0, sizes[i]));
    assert(!nyr_jit_range(sizes[i], sizes[i], 1));
    assert(!nyr_jit_range(sizes[i], SIZE_MAX, 1));
  }
  nyr_jit_layout_t layout;
  assert(!nyr_jit_layout_init(SIZE_MAX, 1, 16, 0, true, &layout));
  assert(!nyr_jit_layout_init(16, SIZE_MAX, 16, 0, true, &layout));
  assert(!nyr_jit_layout_init(16, 1, 0, 0, true, &layout));
  assert(!nyr_jit_layout_init(16, 1, 16, SIZE_MAX, true, &layout));
  assert(!nyr_jit_layout_init((size_t)INT32_MAX, 1, 16, 16, true, &layout));
  assert(nyr_jit_layout_init(16, 1, 16, 0, false, &layout));
  size_t off = layout.stub_offset - 1;
  size_t stub;
  assert(!nyr_jit_take_stub(&layout, &off, &stub));
  int32_t disp = 0;
  uintptr_t base = (uintptr_t)UINT64_C(0x100000000);
  assert(nyr_jit_rel32(base + INT32_MAX, base, &disp) && disp == INT32_MAX);
  assert(!nyr_jit_rel32(base + (uintptr_t)INT32_MAX + 1, base, &disp));
  assert(nyr_jit_rel32(base - (uintptr_t)INT32_MAX - 1, base, &disp) &&
         disp == INT32_MIN);
  assert(!nyr_jit_rel32(base - (uintptr_t)INT32_MAX - 2, base, &disp));
  uint32_t imm = 0;
  assert(nyr_jit_call26(base + ((uintptr_t)1 << 25) * 4 - 4, base, &imm));
  assert(!nyr_jit_call26(base + ((uintptr_t)1 << 25) * 4, base, &imm));
  assert(nyr_jit_call26(base - ((uintptr_t)1 << 25) * 4, base, &imm));
  assert(!nyr_jit_call26(base - ((uintptr_t)1 << 25) * 4 - 4, base, &imm));
  assert(!nyr_jit_call26(base + 1, base, &imm));
  puts("jit-layout: ok");
  return 0;
}
