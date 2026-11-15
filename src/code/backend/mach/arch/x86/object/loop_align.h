/*
 * Private x86-64 loop and function alignment helpers, included by object.c.
 */
/*
 * Recommended AMD/Intel multi-byte NOP forms, indexed by length.
 */
static bool mach_emit_align_nops(nyr_x64_mach_enc_t *e, size_t align) {
  static const unsigned char nops[10][9] = {
      {0},
      {0x90},
      {0x66, 0x90},
      {0x0f, 0x1f, 0x00},
      {0x0f, 0x1f, 0x40, 0x00},
      {0x0f, 0x1f, 0x44, 0x00, 0x00},
      {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},
      {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00},
      {0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}};
  size_t pad = (align - (e->code.len & (align - 1))) & (align - 1);
  while (pad) {
    size_t n = pad > 9 ? 9 : pad;
    for (size_t i = 0; i < n; ++i)
      if (!mach_u8(e, nops[n][i]))
        return false;
    pad -= n;
  }
  return true;
}

/*
 * Give backward-branch targets a fixed position: 32 bytes for a compact loop
 * and 16 for a large one. Function starts are aligned by the appenders below.
 */
static bool mach_build_block_align(nyr_x64_mach_enc_t *e) {
  const nyr_mach_func_t *m = e->mach;
  free(e->block_align);
  e->block_align = NULL;
  if (!m->block_len)
    return true;
  e->block_align = calloc(m->block_len, 1);
  if (!e->block_align)
    return false;
  for (size_t latch = 0; latch < m->block_len; ++latch) {
    const nyr_mach_block_t *blk = &m->blocks[latch];
    for (size_t n = 0; n < blk->inst_count; ++n) {
      const nyr_mach_inst_t *in = &m->insts[blk->first_inst + n];
      if ((in->opcode != NYR_MACH_BR && in->opcode != NYR_MACH_BR_IF) ||
          in->src1.kind != NYR_MACH_OPERAND_BLOCK ||
          in->src1.as.block_index > latch)
        continue;
      size_t head = in->src1.as.block_index;
      size_t span = (size_t)blk->first_inst + blk->inst_count -
                    m->blocks[head].first_inst;
      unsigned char want = span <= 48 ? 32 : 16;
      if (want > e->block_align[head])
        e->block_align[head] = want;
    }
  }
  return true;
}

/*
 * Loop alignment is relative to the function's first byte; its start must
 * therefore be aligned to the largest boundary used by the encoder. The gap
 * is never executed.
 */
static bool mach_align_function_start(nyr_obj_buf_t *code) {
  static const unsigned char trap = 0xcc;
  while (code->len & 31)
    if (!nyr_obj_emit(code, &trap, 1))
      return false;
  return true;
}
