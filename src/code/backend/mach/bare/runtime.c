/*
 * Freestanding Linux ELF64 runtime payload. Regenerate with ./make bare-payload.
 * No libc, relocatable data, floating-point arithmetic, or host runtime calls.
 * The entry ABI is four raw words followed by a private operation number.
 */
typedef unsigned long U;
typedef long I;
typedef unsigned int W;
#define MAX (~(U)0)
#define MAGIC 0x545249584e5954UL

static I syscall6(I nr, U a, U b, U c, U d, U e, U f) {
#if defined(__x86_64__)
  register U r10 __asm__("r10") = d;
  register U r8 __asm__("r8") = e;
  register U r9 __asm__("r9") = f;
  register I ax __asm__("rax") = nr;
  __asm__ volatile("syscall"
                   : "+a"(ax)
                   : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return ax;
#else
  register U x0 __asm__("x0") = a;
  register U x1 __asm__("x1") = b;
  register U x2 __asm__("x2") = c;
  register U x3 __asm__("x3") = d;
  register U x4 __asm__("x4") = e;
  register U x5 __asm__("x5") = f;
  register I x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory", "cc");
  return (I)x0;
#endif
}
#if defined(__x86_64__)
#define MMAP 9
#define MUNMAP 11
#define WRITE 1
#else
#define MMAP 222
#define MUNMAP 215
#define WRITE 64
#endif
static void *map(U n) {
  U p = (U)syscall6(MMAP, 0, n, 3, 34, MAX, 0);
  return p >= (U)-4095 ? (void *)0 : (void *)p;
}
static void unmap(void *p, U n) { syscall6(MUNMAP, (U)p, n, 0, 0, 0, 0); }
static U length(const char *s) {
  U n = 0;
  if (s)
    while (s[n])
      ++n;
  return n;
}
static void copy(char *d, const char *s, U n) {
  for (U i = 0; i < n; ++i)
    d[i] = s[i];
}
static U page_size(U n) { return n > MAX - 4095 ? 0 : (n + 4095) & ~(U)4095; }
static char *string(U n) {
  /*
   * The tagged length must remain representable as well as the allocation.
   */
  if (n > (MAX >> 1) || n > MAX - 33)
    return 0;
  U size = page_size(n + 33);
  if (!size)
    return 0;
  U *p = map(size);
  if (!p)
    return 0;
  p[0] = MAGIC;
  p[1] = size;
  p[2] = (n << 1) | 1;
  p[3] = 120;
  char *s = (char *)(p + 4);
  s[n] = 0;
  return s;
}
static char *owned(const char *s, U n) {
  char *d = string(n);
  if (d)
    copy(d, s, n);
  return d;
}
static U integer(char *out, I v) {
  char rev[20];
  U n = 0, k = 0, x = v < 0 ? 0 - (U)v : (U)v;
  do {
    rev[n++] = '0' + x % 10;
    x /= 10;
  } while (x);
  if (v < 0)
    out[k++] = '-';
  while (n)
    out[k++] = rev[--n];
  return k;
}
/*
 * Exact binary64 -> decimal, then precision-six, ties-to-even %g.
 * M * 2^e becomes an integer times 10^-scale by multiplying M by 5^-e
 * when e < 0. Base 10^9 keeps all intermediates within 64 bits. At most
 * 86 limbs / 768 decimal digits are needed (128/1152 reserved here).
 * Unlike approximate scaling, this also preserves halfway/sticky digits.
 */
static U floating(char *out, U bits) {
  U sign = bits >> 63, mant = bits & 0xfffffffffffffUL;
  int exp = (int)((bits >> 52) & 2047);
  U k = 0;
  if (sign)
    out[k++] = '-';
  if (exp == 2047) {
    copy(out + k, mant ? "nan" : "inf", 3);
    return k + 3;
  }
  if (!exp && !mant) {
    out[k++] = '0';
    return k;
  }
  int e = exp ? exp - 1023 - 52 : -1074;
  if (exp)
    mant |= 1UL << 52;
  W limbs[128];
  int used = 0;
  do {
    limbs[used++] = mant % 1000000000;
    mant /= 1000000000;
  } while (mant);
  int steps = e < 0 ? -e : e;
  W factor = e < 0 ? 5 : 2;
  for (int j = 0; j < steps; ++j) {
    U carry = 0;
    for (int i = 0; i < used; ++i) {
      U v = (U)limbs[i] * factor + carry;
      limbs[i] = v % 1000000000;
      carry = v / 1000000000;
    }
    if (carry)
      limbs[used++] = (W)carry;
  }
  char digits[1152], rev[10];
  int n = 0, r = 0;
  W top = limbs[used - 1];
  do {
    rev[r++] = '0' + top % 10;
    top /= 10;
  } while (top);
  while (r)
    digits[n++] = rev[--r];
  for (int i = used - 2; i >= 0; --i) {
    W v = limbs[i], div = 100000000;
    for (int j = 0; j < 9; ++j) {
      digits[n++] = '0' + v / div;
      v %= div;
      div /= 10;
    }
  }
  int x = n - 1 + (e < 0 ? e : 0);
  int live = n < 6 ? n : 6;
  if (n > 6) {
    int sticky = 0;
    for (int i = 7; i < n; ++i)
      sticky |= digits[i] != '0';
    int up = digits[6] > '5' ||
             (digits[6] == '5' && (sticky || ((digits[5] - '0') & 1)));
    if (up) {
      int i = 5;
      while (i >= 0 && digits[i] == '9')
        digits[i--] = '0';
      if (i < 0) {
        digits[0] = '1';
        ++x;
      } else
        ++digits[i];
    }
  }
  while (live > 1 && digits[live - 1] == '0')
    --live;
  if (x >= -4 && x < 6) {
    if (x < 0) {
      out[k++] = '0';
      out[k++] = '.';
      for (int i = -1; i > x; --i)
        out[k++] = '0';
      copy(out + k, digits, live);
      k += live;
    } else {
      for (int i = 0; i <= x; ++i)
        out[k++] = i < live ? digits[i] : '0';
      if (live > x + 1) {
        out[k++] = '.';
        for (int i = x + 1; i < live; ++i)
          out[k++] = digits[i];
      }
    }
  } else {
    out[k++] = digits[0];
    if (live > 1) {
      out[k++] = '.';
      for (int i = 1; i < live; ++i)
        out[k++] = digits[i];
    }
    out[k++] = 'e';
    out[k++] = x < 0 ? '-' : '+';
    if (x < 0)
      x = -x;
    if (x >= 100)
      out[k++] = '0' + x / 100;
    out[k++] = '0' + (x / 10) % 10;
    out[k++] = '0' + x % 10;
  }
  return k;
}
static char *int_string(I v) {
  char buf[24];
  U n = integer(buf, v);
  return owned(buf, n);
}
static char *float_string(U v) {
  char buf[24];
  U n = floating(buf, v);
  return owned(buf, n);
}
static char *any(U v) {
  if (!v)
    return "nil";
  if (v == 2)
    return "false";
  if (v == 8)
    return "true";
  if (v & 1)
    return int_string((I)v >> 1);
  if (v > 4096 && !(v & 7)) {
    U tag = ((U *)v)[-1];
    if (tag == 120 || tag == 121)
      return (char *)v;
    if (tag == 110 && (v & 15) == 8)
      return float_string(*(U *)v);
  } else if ((v & 7) != 4 && (v & 7) != 6)
    return "nil";
  int fn = (v & 7) == 4;
  if (fn)
    v &= ~(U)3;
  char buf[32], rev[16];
  U k = fn ? 6 : 7, n = 0;
  copy(buf, fn ? "<fn 0x" : "<ptr 0x", k);
  do {
    U d = v & 15;
    rev[n++] = d < 10 ? '0' + d : 'a' + d - 10;
    v >>= 4;
  } while (v);
  while (n)
    buf[k++] = rev[--n];
  buf[k++] = '>';
  return owned(buf, k);
}
static int match(const char *s, const char *old, U n) {
  for (U i = 0; i < n; ++i)
    if (!s[i] || s[i] != old[i])
      return 0;
  return 1;
}
static char *replace(const char *s, const char *old, const char *with) {
  if (!s)
    s = "";
  if (!old)
    old = "";
  if (!with)
    with = "";
  U a = length(s), b = length(old), c = length(with), count = 0;
  if (!b)
    return owned(s, a);
  for (U i = 0; i < a;) {
    if (match(s + i, old, b)) {
      ++count;
      i += b;
    } else
      ++i;
  }
  U n = a;
  if (c >= b) {
    if (count && c - b > (MAX - a) / count)
      return 0;
    n += count * (c - b);
  } else
    n -= count * (b - c);
  char *out = string(n);
  if (!out)
    return 0;
  U k = 0;
  for (U i = 0; i < a;) {
    if (match(s + i, old, b)) {
      copy(out + k, with, c);
      k += c;
      i += b;
    } else
      out[k++] = s[i++];
  }
  return out;
}
static U width(const unsigned char *s, U n) {
  unsigned a = s[0];
  if (a <= 0x7f)
    return 1;
  if (n >= 2 && a >= 0xc2 && a <= 0xdf && (s[1] & 0xc0) == 0x80)
    return 2;
  if (n >= 3 && (s[2] & 0xc0) == 0x80) {
    if (a == 0xe0 && s[1] >= 0xa0 && s[1] <= 0xbf)
      return 3;
    if (((a >= 0xe1 && a <= 0xec) || (a >= 0xee && a <= 0xef)) &&
        (s[1] & 0xc0) == 0x80)
      return 3;
    if (a == 0xed && s[1] >= 0x80 && s[1] <= 0x9f)
      return 3;
  }
  if (n >= 4 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
    if (a == 0xf0 && s[1] >= 0x90 && s[1] <= 0xbf)
      return 4;
    if (a >= 0xf1 && a <= 0xf3 && (s[1] & 0xc0) == 0x80)
      return 4;
    if (a == 0xf4 && s[1] >= 0x80 && s[1] <= 0x8f)
      return 4;
  }
  return 1;
}
static char *slice(const char *s, I start, I stop) {
  if (!s)
    return string(0);
  U bytes = length(s);
  /*
   * All bare literal/managed strings have readable tag windows. Builder
   * buffers are raw mmap bases, so avoid probing before a page boundary.
   */
  if (((U)s & 7) == 0 && ((U)s & 4095) >= 16 &&
      (((U *)s)[-1] == 120 || ((U *)s)[-1] == 121))
    bytes = ((U *)s)[-2] >> 1;
  I count = 0;
  for (U pos = 0; pos < bytes; ++count)
    pos += width((const unsigned char *)s + pos, bytes - pos);
  if (start < 0)
    start += count;
  if (stop < 0)
    stop += count;
  if (start < 0)
    start = 0;
  if (stop < 0)
    stop = 0;
  if (start > count)
    start = count;
  if (stop > count)
    stop = count;
  if (start >= stop)
    return string(0);
  U pos = 0, begin = 0;
  for (I cp = 0; cp < stop; ++cp) {
    if (cp == start)
      begin = pos;
    pos += width((const unsigned char *)s + pos, bytes - pos);
  }
  return owned(s + begin, pos - begin);
}
static char *reverse_string(const char *s) {
  if (!s)
    return string(0);
  U bytes = length(s);
  if (((U)s & 7) == 0 && ((U)s & 4095) >= 16 &&
      (((U *)s)[-1] == 120 || ((U *)s)[-1] == 121))
    bytes = ((U *)s)[-2] >> 1;
  char *out = string(bytes);
  if (!out)
    return 0;
  U read = 0, write = bytes;
  while (read < bytes) {
    U n = width((const unsigned char *)s + read, bytes - read);
    write -= n;
    copy(out + write, s + read, n);
    read += n;
  }
  return out;
}
static U typed_buffer_clone(U data) {
  if (!data || data < 32)
    return 0;
  U *source = (U *)(data - 32);
  if (source[0] != 0x4e59544255464d47UL)
    return 0;
  I count = (I)source[1], stride = (I)source[2], capacity = (I)source[3];
  if (count < 0 || stride <= 0 || (capacity & (I)0x7fffffffffffffffUL) < count)
    return 0;
  U elements = (U)count, width = (U)stride;
  if (elements && width > (MAX - 32) / elements)
    return 0;
  U bytes = 32 + elements * width;
  U size = page_size(bytes);
  if (!size)
    return 0;
  U *copy_header = map(size);
  if (!copy_header)
    return 0;
  copy((char *)copy_header, (const char *)source, bytes);
  /*
   * New storage has capacity=count; retain the tuple flag bit.
   */
  copy_header[3] = ((U)capacity & (1UL << 63)) | elements;
  return (U)(copy_header + 4);
}

static U value_tag(U value) {
  if (!value)
    return 0;
  if (value > 0x1000 && !(value & 7)) {
    U header = *(U *)(value - 8);
    if (header == 120 || header == 121)
      return 120;
    if (header == 110)
      return 110;
    U *buffer = (U *)(value - 32);
    if (buffer[0] == 0x4e59544255464d47UL)
      return buffer[3] >> 63 ? 103 : 100;
  }
  return (value & 1) ? 1 : 3;
}

typedef struct {
  U magic, size, reserved[2];
  char *buf;
  U len, cap;
} Builder;
static int reserve(Builder *b, U need) {
  if (need < b->cap)
    return 1;
  if (need == MAX)
    return 0;
  U size = page_size(need + 1);
  if (!size)
    return 0;
  if (b->cap <= MAX / 2 && size < b->cap * 2)
    size = b->cap * 2;
  char *p = map(size);
  if (!p)
    return 0;
  copy(p, b->buf, b->len + 1);
  unmap(b->buf, b->cap);
  b->buf = p;
  b->cap = size;
  return 1;
}
static U builder_new(const char *initial) {
  if (!initial)
    initial = "";
  U n = length(initial), size = n == MAX ? 0 : page_size(n + 1);
  if (!size)
    return 0;
  Builder *b = map(4096);
  if (!b)
    return 0;
  b->buf = map(size);
  if (!b->buf) {
    unmap(b, 4096);
    return 0;
  }
  b->magic = MAGIC;
  b->size = 4096;
  b->len = n;
  b->cap = size;
  copy(b->buf, initial, n + 1);
  return (U)b;
}
static U append(U bv, const char *s, I known, int use_known) {
  if (bv <= 4096 || !s || (use_known && known < 0))
    return 0;
  Builder *b = (Builder *)bv;
  U n = use_known ? (U)known : length(s);
  if (n > MAX - b->len)
    return 0;
  U offset = (U)s - (U)b->buf;
  int alias = !use_known && (U)s >= (U)b->buf && offset <= b->len;
  if (!reserve(b, b->len + n))
    return 0;
  if (alias)
    s = b->buf + offset;
  /*
   * General append's source lies in the initialized prefix, destination
   * follows it, so forward copy has memmove semantics even on self-append.
   */
  copy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = 0;
  return bv;
}
static U repeat(U bv, const char *s, I len, I count) {
  if (bv <= 4096 || !s || len < 0 || count < 0)
    return 0;
  Builder *b = (Builder *)bv;
  U n = (U)len, reps = (U)count;
  if ((reps && n > MAX / reps) || n * reps > MAX - b->len ||
      !reserve(b, b->len + n * reps)) {
    /*
     * Preserve the runtime oracle's partial-append fallback on failure.
     */
    for (I i = 0; i < count; ++i)
      (void)append(bv, s, len, 1);
    return bv;
  }
  U total = n * reps;
  if (total) {
    char *dst = b->buf + b->len;
    copy(dst, s, n);
    for (U filled = n; filled < total;) {
      U chunk = filled < total - filled ? filled : total - filled;
      copy(dst + filled, dst, chunk);
      filled += chunk;
    }
    b->len += total;
    b->buf[b->len] = 0;
  }
  return bv;
}
/*
 * Keep these IDs synchronized with payload.h, independently of public enums.
 */
__attribute__((visibility("hidden"))) U ny_bare_entry(U a, U b, U c, U d,
                                                      U op) {
  switch (op) {
  case 1:
    return (U)int_string((I)a);
  case 2:
    return (U)float_string(a);
  case 3: {
    char buf[24];
    U n = floating(buf, a);
    syscall6(WRITE, 1, (U)buf, n, 0, 0, 0);
    return 0;
  }
  case 4:
    return (U)any(a);
  case 5:
    return a;
  case 6: {
    U *p = map(4096);
    if (!p)
      return 0;
    p[0] = MAGIC;
    p[1] = 4096;
    p[2] = 110;
    p[3] = a;
    return (U)(p + 3);
  }
  case 7:
    return (a << 1) | 1;
  case 8:
    return (U)replace((char *)a, (char *)b, (char *)c);
  case 9:
    return (U)slice((char *)a, (I)b, (I)c);
  case 10:
    return builder_new((char *)a);
  case 11:
    return append(a, (char *)b, 0, 0);
  case 12:
    return append(a, (char *)b, (I)c, 1);
  case 13:
    return repeat(a, (char *)b, (I)c, (I)d);
  case 14: {
    if (!a)
      return 0;
    Builder *p = (Builder *)a;
    U buf = (U)p->buf;
    unmap(p, 4096);
    return buf;
  }
  case 15:
    return typed_buffer_clone(a);
  case 16:
    return value_tag(a);
  case 17:
    return (U)reverse_string((char *)a);
  default:
    return 0;
  }
}
