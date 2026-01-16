#include "code/jit.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <stdint.h>

// Runtime symbols declarations
extern int64_t __malloc(int64_t);
extern int64_t __free(int64_t);
extern int64_t __realloc(int64_t, int64_t);
extern int64_t __memcpy(int64_t, int64_t, int64_t);
extern int64_t __memset(int64_t, int64_t, int64_t);
extern int64_t __memcmp(int64_t, int64_t, int64_t);
extern int64_t __load8_idx(int64_t, int64_t);
extern int64_t __store8_idx(int64_t, int64_t, int64_t);

extern int64_t __syscall(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t);
extern int64_t __execve(int64_t, int64_t, int64_t);
extern int64_t __dlopen(int64_t, int64_t);
extern int64_t __dlsym(int64_t, int64_t);
extern int64_t __dlclose(int64_t);
extern int64_t __dlerror(void);
extern int64_t __call0(int64_t);
extern int64_t __call1(int64_t, int64_t);
extern int64_t __call2(int64_t, int64_t, int64_t);
extern int64_t __call3(int64_t, int64_t, int64_t, int64_t);
extern int64_t __call4(int64_t, int64_t, int64_t, int64_t, int64_t);
extern int64_t __call5(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
extern int64_t __call6(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t);
extern int64_t __call7(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t);
extern int64_t __call8(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t);
extern int64_t __call9(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t);
extern int64_t __call10(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                        int64_t, int64_t, int64_t, int64_t, int64_t);
extern int64_t __call11(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
extern int64_t __call12(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                        int64_t);
extern int64_t __call13(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                        int64_t, int64_t);
extern int64_t __set_args(int64_t, int64_t, int64_t);
extern int64_t __argc(void);
extern int64_t __argv(int64_t);
extern int64_t __envp(void);
extern int64_t __envc(void);
extern int64_t __errno(void);
extern int64_t __parse_ast(int64_t source_ptr);
extern int64_t __globals(void);
extern int64_t __set_globals(int64_t);
extern int64_t __panic(int64_t);
extern int64_t __set_panic_env(int64_t);
extern int64_t __clear_panic_env(void);
extern int64_t __jmpbuf_size(void);
extern int64_t __get_panic_val(void);
extern int64_t __thread_spawn(int64_t, int64_t);
extern int64_t __thread_join(int64_t);
extern int64_t __mutex_new(void);
extern int64_t __mutex_lock64(int64_t);
extern int64_t __mutex_unlock64(int64_t);
extern int64_t __mutex_free(int64_t);
extern int64_t __load16_idx(int64_t, int64_t);
extern int64_t __load32_idx(int64_t, int64_t);
extern int64_t __load64_idx(int64_t, int64_t);
extern int64_t __store16_idx(int64_t, int64_t, int64_t);
extern int64_t __store32_idx(int64_t, int64_t, int64_t);
extern int64_t __store64_idx(int64_t, int64_t, int64_t);
extern int64_t __sys_read_off(int64_t, int64_t, int64_t, int64_t);
extern int64_t __sys_write_off(int64_t, int64_t, int64_t, int64_t);
extern int64_t __add(int64_t, int64_t);
extern int64_t __sub(int64_t, int64_t);
extern int64_t __mul(int64_t, int64_t);
extern int64_t __div(int64_t, int64_t);
extern int64_t __mod(int64_t, int64_t);
extern int64_t __and(int64_t, int64_t);
extern int64_t __or(int64_t, int64_t);
extern int64_t __xor(int64_t, int64_t);
extern int64_t __shl(int64_t, int64_t);
extern int64_t __shr(int64_t, int64_t);
extern int64_t __not(int64_t);
extern int64_t __is_int(int64_t);
extern int64_t __is_ptr(int64_t);
extern int64_t __is_str(int64_t);
extern int64_t __is_flt(int64_t);
extern int64_t __to_str(int64_t);
extern int64_t __str_concat(int64_t, int64_t);
extern int64_t __eq(int64_t, int64_t);
extern int64_t __lt(int64_t, int64_t);
extern int64_t __le(int64_t, int64_t);
extern int64_t __gt(int64_t, int64_t);
extern int64_t __ge(int64_t, int64_t);
extern int64_t __kwarg(int64_t, int64_t);
extern int64_t __flt_box_val(int64_t);
extern int64_t __flt_unbox_val(int64_t);
extern int64_t __rand64(void);
extern int64_t __srand(int64_t);
extern int64_t __flt_add(int64_t, int64_t);
extern int64_t __flt_sub(int64_t, int64_t);
extern int64_t __flt_mul(int64_t, int64_t);
extern int64_t __flt_div(int64_t, int64_t);
extern int64_t __flt_lt(int64_t, int64_t);
extern int64_t __flt_gt(int64_t, int64_t);
extern int64_t __flt_eq(int64_t, int64_t);
extern int64_t __flt_from_int(int64_t);
extern int64_t __flt_to_int(int64_t);
extern int64_t __flt_trunc(int64_t);

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod) {
#define MAP(name, fn_ptr)                                                      \
  do {                                                                         \
    LLVMValueRef val = LLVMGetNamedFunction(mod, name);                        \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)fn_ptr);                           \
    }                                                                          \
  } while (0)

  MAP("__malloc", __malloc);
  MAP("__free", __free);
  MAP("__realloc", __realloc);
  MAP("__load8_idx", __load8_idx);
  MAP("__load16_idx", __load16_idx);
  MAP("__load32_idx", __load32_idx);
  MAP("__load64_idx", __load64_idx);
  MAP("__store8_idx", __store8_idx);
  MAP("__store16_idx", __store16_idx);
  MAP("__store32_idx", __store32_idx);
  MAP("__store64_idx", __store64_idx);
  MAP("__sys_read_off", __sys_read_off);
  MAP("__sys_write_off", __sys_write_off);
  MAP("__syscall", __syscall);
  MAP("__execve", __execve);
  MAP("__add", __add);
  MAP("__sub", __sub);
  MAP("__mul", __mul);
  MAP("__div", __div);
  MAP("__mod", __mod);
  MAP("__and", __and);
  MAP("__or", __or);
  MAP("__xor", __xor);
  MAP("__shl", __shl);
  MAP("__shr", __shr);
  MAP("__not", __not);
  MAP("__is_int", __is_int);
  MAP("__is_ptr", __is_ptr);
  MAP("__is_str", __is_str);
  MAP("__is_flt", __is_flt);
  MAP("__to_str", __to_str);
  MAP("__str_concat", __str_concat);
  MAP("__eq", __eq);
  MAP("__lt", __lt);
  MAP("__le", __le);
  MAP("__gt", __gt);
  MAP("__ge", __ge);
  MAP("__flt_box_val", __flt_box_val);
  MAP("__flt_unbox_val", __flt_unbox_val);
  MAP("__flt_add", __flt_add);
  MAP("__flt_sub", __flt_sub);
  MAP("__flt_mul", __flt_mul);
  MAP("__flt_div", __flt_div);
  MAP("__flt_lt", __flt_lt);
  MAP("__flt_gt", __flt_gt);
  MAP("__flt_eq", __flt_eq);
  MAP("__flt_from_int", __flt_from_int);
  MAP("__flt_to_int", __flt_to_int);
  MAP("__flt_trunc", __flt_trunc);
  MAP("__dlsym", __dlsym);
  MAP("__dlopen", __dlopen);
  MAP("__dlclose", __dlclose);
  MAP("__dlerror", __dlerror);
  MAP("__kwarg", __kwarg);
  MAP("__memcpy", __memcpy);
  MAP("__memcmp", __memcmp);
  MAP("__memset", __memset);
  MAP("__call0", __call0);
  MAP("__call1", __call1);
  MAP("__call2", __call2);
  MAP("__call3", __call3);
  MAP("__call4", __call4);
  MAP("__call5", __call5);
  MAP("__call6", __call6);
  MAP("__call7", __call7);
  MAP("__call8", __call8);
  MAP("__call9", __call9);
  MAP("__call10", __call10);
  MAP("__call11", __call11);
  MAP("__call12", __call12);
  MAP("__call13", __call13);
  MAP("__set_args", __set_args);
  MAP("__argc", __argc);
  MAP("__argv", __argv);
  MAP("__envp", __envp);
  MAP("__envc", __envc);
  MAP("__errno", __errno);
  MAP("__parse_ast", __parse_ast);
  MAP("__globals", __globals);
  MAP("__set_globals", __set_globals);
  MAP("__panic", __panic);
  MAP("__set_panic_env", __set_panic_env);
  MAP("__clear_panic_env", __clear_panic_env);
  MAP("__jmpbuf_size", __jmpbuf_size);
  MAP("__get_panic_val", __get_panic_val);
  MAP("__thread_spawn", __thread_spawn);
  MAP("__thread_join", __thread_join);
  MAP("__mutex_new", __mutex_new);
  MAP("__mutex_lock64", __mutex_lock64);
  MAP("__mutex_unlock64", __mutex_unlock64);
  MAP("__mutex_free", __mutex_free);
#undef MAP
}
