#ifndef NYTRIX_TEST_FFICONSTS_H
#define NYTRIX_TEST_FFICONSTS_H

#define NYTRIX_FFI_CONST_HEX 0x2a
#define NYTRIX_FFI_CONST_SHIFT (1 << 5)
#define NYTRIX_FFI_CONST_MASK (NYTRIX_FFI_CONST_HEX | NYTRIX_FFI_CONST_SHIFT)

enum {
  NYTRIX_FFI_ENUM_FIRST = 7,
  NYTRIX_FFI_ENUM_SECOND = 11
};

typedef struct NytrixFfiColor {
  unsigned char r;
  unsigned char g;
  unsigned char b;
  unsigned char a;
} NytrixFfiColor;

typedef struct NytrixFfiImage {
  void *data;
  int width;
  int height;
  int mipmaps;
  int format;
} NytrixFfiImage;

#endif
