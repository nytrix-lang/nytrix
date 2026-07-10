#ifndef NYTRIX_INTERNAL_AGGREGATE_H
#define NYTRIX_INTERNAL_AGGREGATE_H
#define NYTRIX_INTERNAL_AGGREGATE_MARKER 1
#define NYTRIX_INTERNAL_AGGREGATE_COUNT 3
#define NYTRIX_INTERNAL_AGGREGATE_TOTAL (NYTRIX_INTERNAL_AGGREGATE_COUNT * 2 + 1)
#define NYTRIX_INTERNAL_AGGREGATE_SHIFT (1 << 3)
#define NYTRIX_INTERNAL_AGGREGATE_MASK ((1 << 4) | 3)
#define NYTRIX_INTERNAL_AGGREGATE_CMP ((8 >= 8) + 3)
#define NYTRIX_INTERNAL_AGGREGATE_LOGIC (((1 << 2) == 4) && !0)
#define NYTRIX_INTERNAL_AGGREGATE_MOD (10 % 3)
#define NYTRIX_INTERNAL_AGGREGATE_COND (NYTRIX_INTERNAL_AGGREGATE_COUNT > 2 ? 5 : 1)
#define NYTRIX_INTERNAL_AGGREGATE_UNARY ((+4 + -2) + ((~0) & 7))
#define NYTRIX_INTERNAL_AGGREGATE_HEX (0x10u >> 2)
#define NYTRIX_INTERNAL_AGGREGATE_CHAR ('A' - 60)
#define NYTRIX_INTERNAL_AGGREGATE_CONTINUED (NYTRIX_INTERNAL_AGGREGATE_HEX + \
                                             3)

#if NYTRIX_INTERNAL_AGGREGATE_HEX == 4
#define NYTRIX_INTERNAL_AGGREGATE_COND_ACTIVE 1
#elif NYTRIX_INTERNAL_AGGREGATE_HEX == 5
#define NYTRIX_INTERNAL_AGGREGATE_COND_INACTIVE 1
#endif

#ifdef NYTRIX_INTERNAL_AGGREGATE_COND_ACTIVE
#define NYTRIX_INTERNAL_AGGREGATE_IFDEF_SEEN 1
#endif

#ifndef NYTRIX_INTERNAL_AGGREGATE_COND_MISSING
#define NYTRIX_INTERNAL_AGGREGATE_IFNDEF_SEEN 1
#endif

#if defined(NYTRIX_INTERNAL_AGGREGATE_IFDEF_SEEN) && defined NYTRIX_INTERNAL_AGGREGATE_IFNDEF_SEEN
#define NYTRIX_INTERNAL_AGGREGATE_DEFINED_EXPR 1
#endif

#if defined(NYTRIX_INTERNAL_AGGREGATE_COND_MISSING)
#define NYTRIX_INTERNAL_AGGREGATE_DEFINED_MISSING 1
#endif

#define NYTRIX_INTERNAL_AGGREGATE_FNLIKE(x) ((x) + 1)
#define NYTRIX_INTERNAL_AGGREGATE_UNSUPPORTED_EXPR (NYTRIX_INTERNAL_UNKNOWN_MACRO + 1)

#if 0
typedef struct NytrixInternalInactiveUnsupported {
  int (*broken)(;
} NytrixInternalInactiveUnsupported;
#endif

typedef struct NytrixInternalPoint {
  int x;
  int y;
} NytrixInternalPoint;

_Static_assert(sizeof(NytrixInternalPoint) == 8, "point layout");
static_assert(sizeof(int) == 4, "int layout");
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic pop")
;

typedef struct NytrixInternalMixed {
  char tag;
  long long value;
  void *payload;
} NytrixInternalMixed;

typedef union NytrixInternalWord {
  int i;
  void *p;
} NytrixInternalWord;

typedef struct NytrixInternalArray {
  char tag[4];
  int values[3];
} NytrixInternalArray;

typedef struct NytrixInternalPacked {
  char tag;
  int value;
} __attribute__((packed)) NytrixInternalPacked;

#pragma pack(push, 1)
typedef struct NytrixInternalPragmaPacked {
  char tag;
  int value;
} NytrixInternalPragmaPacked;
#pragma pack(pop)

#pragma pack(push, 2)
typedef struct NytrixInternalPragmaPack2 {
  char tag;
  int value;
  short code;
} NytrixInternalPragmaPack2;
#pragma pack(pop)

typedef struct NytrixInternalNested {
  NytrixInternalPoint point;
  char tag;
} NytrixInternalNested;

struct NytrixInternalTaggedPoint {
  int x;
  int y;
};

typedef struct NytrixInternalTaggedBox {
  struct NytrixInternalTaggedPoint min;
  struct NytrixInternalTaggedPoint max;
} NytrixInternalTaggedBox;

typedef struct NytrixInternalAnonymousAggregate {
  int id;
  struct {
    int x;
    int y;
  };
  union {
    int code;
    char tag;
  };
} NytrixInternalAnonymousAggregate;

typedef struct NytrixInternalBits {
  unsigned a : 3;
  unsigned b : 5;
  unsigned c : 0x8u;
  unsigned d : 12;
} NytrixInternalBits;

typedef struct NytrixInternalBitGaps {
  unsigned a : 3;
  unsigned : 0;
  unsigned b : 5;
  unsigned : 7;
  unsigned c : 3;
} NytrixInternalBitGaps;

typedef struct NytrixInternalMacroArray {
  char tag[NYTRIX_INTERNAL_AGGREGATE_MARKER];
  int values[NYTRIX_INTERNAL_AGGREGATE_MARKER];
} NytrixInternalMacroArray;

typedef struct NytrixInternalMacroExprArray {
  char tag[NYTRIX_INTERNAL_AGGREGATE_TOTAL];
  int values[NYTRIX_INTERNAL_AGGREGATE_COUNT + 1];
} NytrixInternalMacroExprArray;

typedef struct NytrixInternalSizeofArray {
  char bytes[sizeof(int)];
  char point[sizeof(NytrixInternalPoint)];
  char pointer[sizeof(void *)];
} NytrixInternalSizeofArray;

typedef struct NytrixInternalCastArray {
  char numeric[(unsigned)4];
  char macro[(unsigned)(NYTRIX_INTERNAL_AGGREGATE_COUNT + 1)];
  char sized[(unsigned)sizeof(NytrixInternalPoint)];
} NytrixInternalCastArray;

typedef struct NytrixInternalShiftArray {
  char left[NYTRIX_INTERNAL_AGGREGATE_SHIFT];
  char right[(32 >> 2)];
  char mixed[(NYTRIX_INTERNAL_AGGREGATE_COUNT + 1) << 1];
} NytrixInternalShiftArray;

typedef struct NytrixInternalBitwiseArray {
  char mask[NYTRIX_INTERNAL_AGGREGATE_MASK & 7];
  char xor_value[(3 ^ 5)];
  char or_value[((1 << 2) | 2)];
} NytrixInternalBitwiseArray;

typedef struct NytrixInternalCompareArray {
  char pointer[NYTRIX_INTERNAL_AGGREGATE_CMP];
  char equal[(sizeof(NytrixInternalPoint) == 8) + 2];
  char less[(NYTRIX_INTERNAL_AGGREGATE_COUNT < 4) + 3];
} NytrixInternalCompareArray;

typedef struct NytrixInternalLogicArray {
  char truth[(1 && !0) + 2];
  char either[(0 || 1) + 3];
  char both[(1 && 1) + 4];
} NytrixInternalLogicArray;

typedef struct NytrixInternalModuloArray {
  char macro[NYTRIX_INTERNAL_AGGREGATE_MOD + 2];
  char inline_value[(17 % 5) + 3];
  char mixed[((20 % 6) * 2) + 2];
} NytrixInternalModuloArray;

typedef struct NytrixInternalConditionalArray {
  char macro[NYTRIX_INTERNAL_AGGREGATE_COND + 1];
  char inline_true[(1 ? 4 : 1)];
  char inline_false[(0 ? 8 : 3)];
} NytrixInternalConditionalArray;

typedef struct NytrixInternalUnaryArray {
  char macro[NYTRIX_INTERNAL_AGGREGATE_UNARY + 1];
  char plus[(+3)];
  char negative[(-2 + 7)];
  char invert[((~0) & 3) + !0];
} NytrixInternalUnaryArray;

typedef struct NytrixInternalIntegerLiteralArray {
  char hex[NYTRIX_INTERNAL_AGGREGATE_HEX + 1];
  char octal[(010 + 1)];
  char binary[(0b101 + 2)];
  char suffix[(0x3ULL + 1)];
} NytrixInternalIntegerLiteralArray;

typedef struct NytrixInternalCharLiteralArray {
  char macro[NYTRIX_INTERNAL_AGGREGATE_CHAR];
  char newline[('\n' - 7)];
  char hex[('\x41' - 60)];
  char octal[('\101' - 61)];
} NytrixInternalCharLiteralArray;

typedef struct NytrixInternalContinuedMacroArray {
  char macro[NYTRIX_INTERNAL_AGGREGATE_CONTINUED];
  char folded[(NYTRIX_INTERNAL_AGGREGATE_CONTINUED == 7) ? 2 : 1];
} NytrixInternalContinuedMacroArray;

enum NytrixInternalEnumExtent {
  NYTRIX_INTERNAL_ENUM_BASE = 2,
  NYTRIX_INTERNAL_ENUM_COUNT = NYTRIX_INTERNAL_ENUM_BASE + 2,
  NYTRIX_INTERNAL_ENUM_LOGIC = NYTRIX_INTERNAL_AGGREGATE_LOGIC,
  NYTRIX_INTERNAL_ENUM_COND = NYTRIX_INTERNAL_AGGREGATE_COND,
  NYTRIX_INTERNAL_ENUM_UNARY = NYTRIX_INTERNAL_AGGREGATE_UNARY,
  NYTRIX_INTERNAL_ENUM_HEX = NYTRIX_INTERNAL_AGGREGATE_HEX,
  NYTRIX_INTERNAL_ENUM_CHAR = NYTRIX_INTERNAL_AGGREGATE_CHAR,
  NYTRIX_INTERNAL_ENUM_CONTINUED = NYTRIX_INTERNAL_AGGREGATE_CONTINUED
};

typedef struct NytrixInternalEnumArray {
  int values[NYTRIX_INTERNAL_ENUM_COUNT];
  char tags[NYTRIX_INTERNAL_ENUM_COUNT - 1];
} NytrixInternalEnumArray;

typedef struct NytrixInternalMultiDecl {
  int x, y;
  char tag, code;
} NytrixInternalMultiDecl;

typedef struct NytrixInternalMultiArrayDecl {
  int lanes[2], masks[3];
  char names[2], codes[NYTRIX_INTERNAL_AGGREGATE_COUNT];
} NytrixInternalMultiArrayDecl;

typedef struct NytrixInternalMultiPointerDecl {
  void *head, *tail;
  int *items[2], *fallback;
} NytrixInternalMultiPointerDecl;

typedef struct NytrixInternalCallbackTable {
  int (*compare)(int left, int right);
  void (*visit)(void *ctx);
  int (*handlers[3])(int code);
  void *ctx;
} NytrixInternalCallbackTable;

typedef struct NytrixInternalCommaCallbackTable {
  int (*open)(int code), (*close)(int code);
  void (*enter)(void *ctx), (*leave)(void *ctx);
} NytrixInternalCommaCallbackTable;

typedef struct NytrixInternalPointerToArray {
  char tag;
  int (*rows)[3];
  char tail;
} NytrixInternalPointerToArray;

typedef struct NytrixInternalAlignedField {
  char tag;
  int value __attribute__((aligned(16)));
  char tail;
} NytrixInternalAlignedField;

typedef struct NytrixInternalFlexibleArray {
  int length;
  char data[];
} NytrixInternalFlexibleArray;

typedef struct {
  int x;
} __attribute__((aligned(16))) NytrixInternalAlignedTag;

typedef struct {
  char a;
  int b;
} __attribute__((packed, aligned(8))) NytrixInternalPackedAligned;

typedef struct {
  int m[2][3];
} NytrixInternalMultiDimField;

typedef struct {
  char a;
  int b;
} __attribute__((aligned(8), packed)) NytrixInternalAlignedPacked;

_Noreturn void nytrix_internal_noreturn_probe(void);
extern int nytrix_internal_aggregate_probe(NytrixInternalPoint p);

#undef NYTRIX_INTERNAL_AGGREGATE_TOTAL
#undef NYTRIX_INTERNAL_AGGREGATE_COUNT
#undef NYTRIX_INTERNAL_AGGREGATE_MARKER
#undef NYTRIX_INTERNAL_AGGREGATE_SHIFT
#undef NYTRIX_INTERNAL_AGGREGATE_MASK
#undef NYTRIX_INTERNAL_AGGREGATE_CMP
#undef NYTRIX_INTERNAL_AGGREGATE_LOGIC
#undef NYTRIX_INTERNAL_AGGREGATE_MOD
#undef NYTRIX_INTERNAL_AGGREGATE_COND
#undef NYTRIX_INTERNAL_AGGREGATE_UNARY
#undef NYTRIX_INTERNAL_AGGREGATE_HEX
#undef NYTRIX_INTERNAL_AGGREGATE_CHAR
#undef NYTRIX_INTERNAL_AGGREGATE_CONTINUED
#undef NYTRIX_INTERNAL_AGGREGATE_COND_ACTIVE
#undef NYTRIX_INTERNAL_AGGREGATE_IFDEF_SEEN
#undef NYTRIX_INTERNAL_AGGREGATE_IFNDEF_SEEN
#undef NYTRIX_INTERNAL_AGGREGATE_DEFINED_EXPR
#undef NYTRIX_INTERNAL_AGGREGATE_FNLIKE
#undef NYTRIX_INTERNAL_AGGREGATE_UNSUPPORTED_EXPR

#endif
