#ifndef NYTRIX_INTERNAL_LOWERING_ABI_H
#define NYTRIX_INTERNAL_LOWERING_ABI_H

/* Focused ABI-lowering probe header for the nytrix internal C frontend.
 * Each aggregate targets one C type-lowering fix and a known byte size that
 * the test asserts through __layout_size / load_layout. Sizes assume SysV
 * x86-64 (the default test ABI). */

/* L-1: struct with array fields lowers as [N x T]: char[4] + pad(0) + int[3]. */
typedef struct NyLowerArray {
    char tag[4];
    int values[3];
} NyLowerArray;

/* L-2: union lowers as [size x i8]; size = max member = pointer (8). */
typedef union NyLowerWord {
    int i;
    void *p;
    long l;
} NyLowerWord;

/* L-3: __attribute__((packed)): 1 + 4 = 5, no padding. */
typedef struct NyLowerPacked {
    char tag;
    int value;
} __attribute__((packed)) NyLowerPacked;

/* L-5: long double is a 16-byte slot on SysV x86-64. */
typedef struct NyLowerLongDouble {
    long double x;
} NyLowerLongDouble;

/* L-6: __int128 is 16 bytes. */
typedef struct NyLowerInt128 {
    __int128 v;
} NyLowerInt128;

/* L-7: _Complex float = 8 bytes (c64), _Complex double = 16 bytes (c128). */
typedef struct NyLowerComplexFloat {
    float _Complex z;
} NyLowerComplexFloat;

typedef struct NyLowerComplexDouble {
    double _Complex z;
} NyLowerComplexDouble;

/* L-12: typedef struct Bar {...} Foo registers under both tag and typedef. */
typedef struct NyLowerAliasedTag {
    int a;
    int b;
} NyLowerAliasedTypedef;

#endif
