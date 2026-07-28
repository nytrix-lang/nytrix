#ifndef NYTRIX_FFI_TRANSITIVE_DEP_H
#define NYTRIX_FFI_TRANSITIVE_DEP_H

typedef struct {
  long opaque[3];
} NytrixFfiTransitiveBlob;

NytrixFfiTransitiveBlob NytrixFfiTransitiveBlobValue(void);

#endif
