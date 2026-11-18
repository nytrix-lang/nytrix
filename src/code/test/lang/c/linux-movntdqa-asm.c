/*
 * C front-end regression test: linux movntdqa asm.
 */
void copy_movntdqa_movaps4(void *destination, const void *source) {
  asm("movntdqa   (%0), %%xmm0\n"
      "movntdqa 16(%0), %%xmm1\n"
      "movntdqa 32(%0), %%xmm2\n"
      "movntdqa 48(%0), %%xmm3\n"
      "movaps %%xmm0,   (%1)\n"
      "movaps %%xmm1, 16(%1)\n"
      "movaps %%xmm2, 32(%1)\n"
      "movaps %%xmm3, 48(%1)\n"
      :
      : "r"(source), "r"(destination)
      : "memory");
}

void copy_movntdqa_movaps1(void *destination, const void *source) {
  asm("movntdqa (%0), %%xmm0\n"
      "movaps %%xmm0, (%1)\n"
      :
      : "r"(source), "r"(destination)
      : "memory");
}

void copy_movntdqa_movups4(void *destination, const void *source) {
  asm("movntdqa   (%0), %%xmm0\n"
      "movntdqa 16(%0), %%xmm1\n"
      "movntdqa 32(%0), %%xmm2\n"
      "movntdqa 48(%0), %%xmm3\n"
      "movups %%xmm0,   (%1)\n"
      "movups %%xmm1, 16(%1)\n"
      "movups %%xmm2, 32(%1)\n"
      "movups %%xmm3, 48(%1)\n"
      :
      : "r"(source), "r"(destination)
      : "memory");
}

void copy_movntdqa_movups1(void *destination, const void *source) {
  asm("movntdqa (%0), %%xmm0\n"
      "movups %%xmm0, (%1)\n"
      :
      : "r"(source), "r"(destination)
      : "memory");
}
