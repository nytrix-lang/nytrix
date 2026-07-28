typedef struct NytrixInternalBuffer {
  long value;
} NytrixInternalBuffer;

void *memset(NytrixInternalBuffer *dst, int value, unsigned long count);

