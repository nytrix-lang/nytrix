#ifndef NYTRIX_INTERNAL_COMPLEX_PROGRAM_H
#define NYTRIX_INTERNAL_COMPLEX_PROGRAM_H

#define NY_COMPLEX_WIDTH (1u << 4)

typedef struct NyComplexInner {
  int x;
  int y;
} NyComplexInner;

typedef struct NyComplexOuter {
  NyComplexInner inner;
  unsigned long tag;
  int values[NY_COMPLEX_WIDTH];
} NyComplexOuter;

typedef int (*NyComplexCompare)(const void *, const void *);

typedef struct NyComplexDiv {
  int quot;
  int rem;
} NyComplexDiv;

extern int optind;
int snprintf(char *dst, unsigned long cap, const char *format, ...);
void qsort(void *base, unsigned long count, unsigned long width,
           NyComplexCompare compare);
NyComplexDiv div(int numerator, int denominator);

#endif
