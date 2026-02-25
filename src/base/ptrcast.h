#ifndef NYTRIX_BASE_PTRCAST_H
#define NYTRIX_BASE_PTRCAST_H
#include <stdint.h>

typedef union {
  void *obj;
  void (*func)(void);
  uintptr_t addr;
  intptr_t iaddr;
  const void *c_obj;
  char *str;
  uint8_t *u8;
} ny_ptr_cast_t;

static inline void *ny_cast_f2o(void (*f)(void)) {
  ny_ptr_cast_t c;
  c.func = f;
  return c.obj;
}

static inline void (*ny_cast_o2f(void *o))(void) {
  ny_ptr_cast_t c;
  c.obj = o;
  return c.func;
}

static inline void (*ny_cast_a2f(uintptr_t a))(void) {
  ny_ptr_cast_t c;
  c.addr = a;
  return c.func;
}

static inline uintptr_t ny_cast_f2a(void (*f)(void)) {
  ny_ptr_cast_t c;
  c.func = f;
  return c.addr;
}

#define NY_PTR_CAST(type, val) (((ny_ptr_cast_t){.obj = (void*)(val)}).type)

#endif
